// Copyright (c) 2026-present. All rights reserved.
// AMTV M3a Workload Driver: Mixed Read Load Boundary Audit (GetLive + Scan + Put + DeleteRange)

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cmath>
#include <cstring>
#include <cassert>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/slice.h"
#include "rocksdb/iterator.h"
#include "rocksdb/utilities/options_type.h"
#include "db/column_family.h"
#include "db/memtable.h"
#include "db/amtv.h"
#include "db/read_path_audit.h"
#include "tools/formal_v2/formal_event_listener.h"

#define CHECK_INVARIANT(cond, fmt, ...)                                                        \
  do {                                                                                         \
    if (!(cond)) {                                                                             \
      char _buf[1024];                                                                         \
      snprintf(_buf, sizeof(_buf), "[FATAL INVARIANT BREAK at %s:%d] " fmt,                   \
               __FILE__, __LINE__, ##__VA_ARGS__);                                             \
      std::cerr << _buf << std::endl;                                                          \
      std::abort();                                                                            \
    }                                                                                          \
  } while (0)

#pragma pack(push, 1)
struct FormalTraceRecord {
    uint8_t  phase_id;                 // 0=A, 1=B, 2=C
    uint8_t  op_type;                  // 0=GetLive, 1=Scan, 2=Put, 3=DeleteRange
    uint8_t  scan_type;                // 0=None, 1=PlannedIntersect, 2=Intersect, 3=NonIntersect
    uint8_t  expected_visible_keys;    // 0(non-scan), 50, 100
    uint32_t worker_local_op_index;   // 0 .. 37499
    uint64_t key1;                    // start_key
    uint64_t key2;                    // end_key
};
#pragma pack(pop)
static_assert(sizeof(FormalTraceRecord) == 24, "FormalTraceRecord must be exactly 24 bytes");

static inline std::string FormatKey(uint64_t k) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lu", k);
    return std::string(buf);
}

static inline std::string GenerateInitialValue(uint64_t k, size_t val_size = 256) {
    std::string val;
    val.reserve(val_size);
    char prefix[32];
    int len = snprintf(prefix, sizeof(prefix), "INIT:%016lu:", k);
    val.append(prefix, len);
    while (val.size() < val_size) {
        val.push_back(static_cast<char>('a' + ((k + val.size()) % 26)));
    }
    return val;
}

static inline std::string GeneratePutValue(uint64_t k, uint8_t phase, uint32_t op_id, size_t val_size = 256) {
    std::string val;
    val.reserve(val_size);
    char prefix[48];
    int len = snprintf(prefix, sizeof(prefix), "PUT:P%u:OP%06u:K%016lu:", phase, op_id, k);
    val.append(prefix, len);
    while (val.size() < val_size) {
        val.push_back(static_cast<char>('A' + ((k + op_id + val.size()) % 26)));
    }
    return val;
}

// External State Model for Absolute Ground-Truth State Reconciliation
class ExternalStateModel {
public:
    explicit ExternalStateModel(uint64_t total_keys) : values_(total_keys) {
        for (uint64_t k = 0; k < total_keys; ++k) {
            values_[k] = GenerateInitialValue(k, 256);
        }
    }

    void ApplyPut(uint64_t k, const std::string& val) {
        CHECK_INVARIANT(k < values_.size(), "Put key %lu out of range", k);
        values_[k] = val;
    }

    void ApplyDeleteRange(uint64_t k1, uint64_t k2) {
        CHECK_INVARIANT(k1 <= k2 && k2 <= values_.size(), "DeleteRange %lu..%lu out of range", k1, k2);
        for (uint64_t k = k1; k < k2; ++k) {
            values_[k].clear(); // Tombstoned
        }
    }

    const std::string& Get(uint64_t k) const {
        CHECK_INVARIANT(k < values_.size(), "Get key %lu out of range", k);
        return values_[k];
    }

    bool IsLive(uint64_t k) const {
        return k < values_.size() && !values_[k].empty();
    }

    std::pair<uint64_t, std::string> ComputeStateDigest() const {
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        uint64_t live_count = 0;
        for (uint64_t k = 0; k < values_.size(); ++k) {
            if (!values_[k].empty()) {
                live_count++;
                std::string k_str = FormatKey(k);
                SHA256_Update(&ctx, k_str.data(), k_str.size());
                SHA256_Update(&ctx, values_[k].data(), values_[k].size());
            }
        }
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &ctx);
        std::ostringstream oss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        return {live_count, oss.str()};
    }

private:
    std::vector<std::string> values_;
};

struct M3aRunConfig {
    std::string exp_id = "m3a_audit_smoke_seed90001";
    std::string config_name = "AMTV-T0";
    std::string db_path = "./run-db/m3a_smoke_db";
    std::string seed_db_path = "./run-db/m2d_canonical_seed_db";
    std::string trace_dir = "traces/m3a_smoke_seed90001";
    std::string output_dir = "results/amtv_m3a/raw";
    std::string mode = "audit";
    int rep = 1;
    int num_workers = 8;
    uint64_t total_keys = 500000;
};

// High-fidelity Latency Histogram
class LatencyHistogram {
public:
    LatencyHistogram() { Clear(); }

    void Clear() {
        count_ = 0;
        sum_ns_ = 0;
        min_ns_ = UINT64_MAX;
        max_ns_ = 0;
        samples_.clear();
        samples_.reserve(100000);
    }

    void Record(uint64_t lat_ns) {
        count_++;
        sum_ns_ += lat_ns;
        if (lat_ns < min_ns_) min_ns_ = lat_ns;
        if (lat_ns > max_ns_) max_ns_ = lat_ns;
        samples_.push_back(lat_ns);
    }

    void MergeFrom(const LatencyHistogram& o) {
        count_ += o.count_;
        sum_ns_ += o.sum_ns_;
        if (o.min_ns_ < min_ns_) min_ns_ = o.min_ns_;
        if (o.max_ns_ > max_ns_) max_ns_ = o.max_ns_;
        samples_.insert(samples_.end(), o.samples_.begin(), o.samples_.end());
    }

    uint64_t Count() const { return count_; }
    uint64_t SumNanos() const { return sum_ns_; }
    double MeanMicros() const { return count_ ? (sum_ns_ / 1000.0 / count_) : 0.0; }
    double MinMicros() const { return (count_ && min_ns_ != UINT64_MAX) ? (min_ns_ / 1000.0) : 0.0; }
    double MaxMicros() const { return count_ ? (max_ns_ / 1000.0) : 0.0; }

    double PercentileMicros(double p) {
        if (samples_.empty()) return 0.0;
        std::sort(samples_.begin(), samples_.end());
        size_t idx = static_cast<size_t>(std::ceil((p / 100.0) * samples_.size())) - 1;
        if (idx >= samples_.size()) idx = samples_.size() - 1;
        return samples_[idx] / 1000.0;
    }

private:
    uint64_t count_ = 0;
    uint64_t sum_ns_ = 0;
    uint64_t min_ns_ = UINT64_MAX;
    uint64_t max_ns_ = 0;
    std::vector<uint64_t> samples_;
};

struct WorkerPhaseAuditSnapshot {
#ifdef ROCKSDB_READ_PATH_AUDIT
    rocksdb::ReadPathAuditStats stats[static_cast<size_t>(rocksdb::AuditOpType::kMax)];
    rocksdb::AMTVGetProbeStats amtv_get_stats;
#endif
    uint64_t returned_keys[static_cast<size_t>(rocksdb::AuditOpType::kMax)] = {0};
    uint64_t scan_duration_nanos[static_cast<size_t>(rocksdb::AuditOpType::kMax)] = {0};
};

// Build Canonical Seed DB if needed
static void BuildCanonicalSeedDb(const std::string& db_path, uint64_t total_keys) {
    std::cout << "Building Canonical Seed DB at: " << db_path << " (" << total_keys << " keys)...\n";
    std::string wipe_cmd = "rm -rf " + db_path + " && mkdir -p " + db_path;
    int ret = system(wipe_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to clean dir %s", db_path.c_str());

    rocksdb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.target_file_size_base = 64 * 1024 * 1024;
    options.max_bytes_for_level_base = 256 * 1024 * 1024;

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, &db);
    CHECK_INVARIANT(s.ok(), "Failed to open DB: %s", s.ToString().c_str());

    rocksdb::WriteOptions wopts;
    for (uint64_t i = 0; i < total_keys; ++i) {
        std::string key = FormatKey(i);
        std::string val = GenerateInitialValue(i, 256);
        s = db->Put(wopts, key, val);
        CHECK_INVARIANT(s.ok(), "Put failed at %lu", i);
    }

    rocksdb::FlushOptions fopts;
    fopts.wait = true;
    s = db->Flush(fopts);
    CHECK_INVARIANT(s.ok(), "Flush failed");
    db.reset();
    std::cout << "Canonical Seed DB build complete.\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " --build-seed-db <dir> OR --exp-id <id> [options...]\n";
        return 1;
    }

    std::string first_arg = argv[1];
    if (first_arg == "--build-seed-db") {
        std::string seed_dir = (argc >= 3) ? argv[2] : "./run-db/m2d_canonical_seed_db";
        BuildCanonicalSeedDb(seed_dir, 500000);
        return 0;
    }

    M3aRunConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--exp-id" && i + 1 < argc) cfg.exp_id = argv[++i];
        else if (arg == "--config" && i + 1 < argc) cfg.config_name = argv[++i];
        else if (arg == "--db-path" && i + 1 < argc) cfg.db_path = argv[++i];
        else if (arg == "--seed-db" && i + 1 < argc) cfg.seed_db_path = argv[++i];
        else if (arg == "--trace-dir" && i + 1 < argc) cfg.trace_dir = argv[++i];
        else if (arg == "--output-dir" && i + 1 < argc) cfg.output_dir = argv[++i];
        else if (arg == "--mode" && i + 1 < argc) cfg.mode = argv[++i];
        else if (arg == "--rep" && i + 1 < argc) cfg.rep = std::stoi(argv[++i]);
    }

    std::cout << "\n======================================================================\n";
    std::cout << "Starting AMTV M3a Run: " << cfg.exp_id << " (Config: " << cfg.config_name 
              << ", Mode: " << cfg.mode << ", Rep: " << cfg.rep << ")\n";
    std::cout << "======================================================================\n";

    // 1. Verify CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    std::string affinity_str = "";
    int core_count = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
            affinity_str += std::to_string(i) + " ";
            core_count++;
        }
    }
    std::cout << "  Active CPU Affinity (" << core_count << " cores): [" << affinity_str << "]\n";

    // 2. Physical copy from Canonical Seed DB
    std::string wipe_cmd = "rm -rf " + cfg.db_path + " && mkdir -p " + cfg.db_path;
    int ret = system(wipe_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to wipe db dir: %s", cfg.db_path.c_str());

    std::string cp_cmd = "cp -r " + cfg.seed_db_path + "/* " + cfg.db_path + "/";
    ret = system(cp_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to copy seed DB from %s to %s", cfg.seed_db_path.c_str(), cfg.db_path.c_str());

    // 3. Load Trace files for 8 workers
    std::vector<std::vector<FormalTraceRecord>> worker_traces(cfg.num_workers);
    for (int w = 0; w < cfg.num_workers; ++w) {
        std::string trace_file = cfg.trace_dir + "/worker_" + std::to_string(w) + ".trace";
        std::ifstream f(trace_file, std::ios::binary);
        CHECK_INVARIANT(f.is_open(), "Failed to open trace file: %s", trace_file.c_str());
        FormalTraceRecord rec;
        while (f.read(reinterpret_cast<char*>(&rec), sizeof(rec))) {
            worker_traces[w].push_back(rec);
        }
        CHECK_INVARIANT(worker_traces[w].size() == 37500, "Worker %d trace size mismatch: expected 37500, got %zu", w, worker_traces[w].size());
    }

    // 4. Configure RocksDB Options
    rocksdb::Options options;
    options.create_if_missing = false;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.level0_file_num_compaction_trigger = 4;
    options.level0_slowdown_writes_trigger = 8;
    options.level0_stop_writes_trigger = 12;
    options.target_file_size_base = 64 * 1024 * 1024;
    options.max_bytes_for_level_base = 256 * 1024 * 1024;
    options.max_background_jobs = 8;
    rocksdb::Env::Default()->SetBackgroundThreads(4, rocksdb::Env::Priority::BOTTOM);
    rocksdb::Env::Default()->SetBackgroundThreads(4, rocksdb::Env::Priority::HIGH);
    rocksdb::Env::Default()->SetBackgroundThreads(8, rocksdb::Env::Priority::LOW);

    rocksdb::BlockBasedTableOptions table_opts;
    table_opts.block_cache = rocksdb::NewLRUCache(128 * 1024 * 1024);
    table_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_opts));
    options.statistics = rocksdb::CreateDBStatistics();

    std::shared_ptr<study::formal::FormalEventListener> listener = 
        std::make_shared<study::formal::FormalEventListener>();
    options.listeners.push_back(listener);

    // Apply benchmark configuration
    if (cfg.config_name == "Native-T0") {
        options.enable_amtv = false;
        options.memtable_max_range_deletions = 0;
    } else if (cfg.config_name == "Native-T512") {
        options.enable_amtv = false;
        options.memtable_max_range_deletions = 512;
    } else if (cfg.config_name == "AMTV-T0") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 0;
        options.amtv_delta_tombstones = 64;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 32;
    } else if (cfg.config_name == "AMTV-T512") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 512;
        options.amtv_delta_tombstones = 64;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 32;
    } else {
        CHECK_INVARIANT(false, "Unknown config: %s", cfg.config_name.c_str());
    }

    std::cout << "  Configured Options: enable_amtv=" << options.enable_amtv
              << ", delta_tombstones=" << options.amtv_delta_tombstones
              << ", hard_layer_limit=" << options.amtv_hard_layer_limit
              << ", memtable_max_range_deletions=" << options.memtable_max_range_deletions << "\n";

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(options, cfg.db_path, &db);
    CHECK_INVARIANT(s.ok(), "Failed to open DB: %s", s.ToString().c_str());

    rocksdb::ColumnFamilyData* cfd =
        static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();

    // 5. Deterministic Read-only Warmup: 10,000 GetLive across 8 workers (1,250 each) from live region
    std::cout << "  Executing deterministic read-only warm-up (10,000 GetLive from live region)...\n";
    {
        std::vector<std::thread> warmup_workers;
        warmup_workers.reserve(cfg.num_workers);
        for (int w = 0; w < cfg.num_workers; ++w) {
            warmup_workers.emplace_back([&, w]() {
                rocksdb::ReadOptions ropts;
                std::string val;
                uint64_t base_k = w * 62500 + 50000; // Permanently live zone
                for (uint64_t i = 0; i < 1250; ++i) {
                    uint64_t k = base_k + ((i * 17) % 12500);
                    std::string key = FormatKey(k);
                    rocksdb::Status ws = db->Get(ropts, key, &val);
                    CHECK_INVARIANT(ws.ok(), "Warmup Get failed at key %lu: %s", k, ws.ToString().c_str());
                }
            });
        }
        for (auto& t : warmup_workers) t.join();
    }

    // Wait for warmup to settle
    for (int retry = 0; retry < 200; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        bool amtv_stable = true;
        if (options.enable_amtv && cfd->mem() && cfd->mem()->GetAMTVState()) {
            amtv_stable = cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (running_flushes == 0 && running_compactions == 0 && amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "  Warm-up complete. Resetting metrics before Phase A...\n";

    options.statistics->Reset();
#ifdef ROCKSDB_READ_PATH_AUDIT
    rocksdb::SetReadPathAuditEnabled(true);
    rocksdb::ResetAllAuditStats();
    rocksdb::tl_amtv_get_probe_stats.Reset();
    rocksdb::g_amtv_get_probe_stats_enabled.store(true, std::memory_order_relaxed);
#endif

    // Setup Boundary Snapshots
    struct BoundarySnapshot {
        uint64_t flush_bytes = 0;
        uint64_t compaction_write_bytes = 0;
        uint64_t compaction_read_bytes = 0;
        size_t flush_events = 0;
        size_t compaction_events = 0;
    };

    auto capture_boundary = [&]() -> BoundarySnapshot {
        BoundarySnapshot snap;
        snap.flush_bytes = listener->GetTotalCumulativeFlushBytes();
        snap.compaction_write_bytes = listener->GetTotalCumulativeCompactionWriteBytes();
        snap.compaction_read_bytes = listener->GetTotalCumulativeCompactionReadBytes();
        snap.flush_events = listener->GetFlushEvents().size();
        snap.compaction_events = listener->GetCompactionEvents().size();
        return snap;
    };

    BoundarySnapshot b0 = capture_boundary();

    // Latency histograms: [worker][phase][op_type]
    LatencyHistogram hist_ops[8][3][static_cast<size_t>(rocksdb::AuditOpType::kMax)];
    WorkerPhaseAuditSnapshot worker_snapshots[8][3];

    std::barrier sync_barrier(cfg.num_workers + 1);
    std::vector<double> phase_elapsed_sec(3, 0.0);

    // Spawn 8 Worker Threads
    std::vector<std::thread> workers;
    workers.reserve(cfg.num_workers);

    for (int w = 0; w < cfg.num_workers; ++w) {
        workers.emplace_back([&, w]() {
#ifdef ROCKSDB_READ_PATH_AUDIT
            rocksdb::ResetAllAuditStats();
            rocksdb::tl_amtv_get_probe_stats.Reset();
#endif
            const auto& trace = worker_traces[w];
            rocksdb::ReadOptions ropts;
            rocksdb::WriteOptions wopts;
            wopts.disableWAL = false;

            size_t trace_idx = 0;

            for (int phase = 0; phase < 3; ++phase) {
                sync_barrier.arrive_and_wait(); // Wait for phase release

                size_t phase_records = 12500;
                for (size_t i = 0; i < phase_records; ++i) {
                    const auto& rec = trace[trace_idx++];
                    CHECK_INVARIANT(rec.phase_id == phase, "Trace phase mismatch: expected %d, got %u", phase, rec.phase_id);

                    if (rec.op_type == 0) { // GetLive
#ifdef ROCKSDB_READ_PATH_AUDIT
                        rocksdb::AuditOpScope scope(rocksdb::AuditOpType::kGetLive);
#endif
                        auto op_start = std::chrono::steady_clock::now();
                        std::string val;
                        std::string key = FormatKey(rec.key1);
                        rocksdb::Status s_get = db->Get(ropts, key, &val);
                        CHECK_INVARIANT(s_get.ok(), "Worker %d GetLive key %lu failed: %s", w, rec.key1, s_get.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_ops[w][phase][static_cast<size_t>(rocksdb::AuditOpType::kGetLive)].Record(lat_ns);

                    } else if (rec.op_type == 1) { // Scan
                        rocksdb::AuditOpType op_tag = rocksdb::AuditOpType::kNone;
                        if (rec.scan_type == 1) op_tag = rocksdb::AuditOpType::kScanPlannedIntersect;
                        else if (rec.scan_type == 2) op_tag = rocksdb::AuditOpType::kScanIntersect;
                        else if (rec.scan_type == 3) op_tag = rocksdb::AuditOpType::kScanNonIntersect;
                        else CHECK_INVARIANT(false, "Unknown scan_type: %u", rec.scan_type);

#ifdef ROCKSDB_READ_PATH_AUDIT
                        rocksdb::AuditOpScope scope(op_tag);
#endif
                        auto op_start = std::chrono::steady_clock::now();

                        std::string start_k = FormatKey(rec.key1);
                        std::string end_k = FormatKey(rec.key2);
                        rocksdb::Slice end_slice(end_k);

                        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ropts));
                        uint64_t returned_keys = 0;
                        for (it->Seek(start_k); it->Valid() && it->key().compare(end_slice) < 0; it->Next()) {
                            returned_keys++;
                        }
                        CHECK_INVARIANT(it->status().ok(), "Worker %d Scan [%lu, %lu) failed: %s",
                                        w, rec.key1, rec.key2, it->status().ToString().c_str());
                        it.reset(); // Destroy iterator under RAII scope
                        auto op_end = std::chrono::steady_clock::now();

                        // HARD ASSERTION: Exact returned visible keys match record expectation!
                        CHECK_INVARIANT(returned_keys == rec.expected_visible_keys,
                                        "Worker %d op %u Scan [%lu, %lu) expected %u keys, got %lu",
                                        w, rec.worker_local_op_index, rec.key1, rec.key2, rec.expected_visible_keys, returned_keys);

                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        size_t tag_idx = static_cast<size_t>(op_tag);
                        hist_ops[w][phase][tag_idx].Record(lat_ns);
                        worker_snapshots[w][phase].returned_keys[tag_idx] += returned_keys;
                        worker_snapshots[w][phase].scan_duration_nanos[tag_idx] += lat_ns;

                    } else if (rec.op_type == 2) { // Put
#ifdef ROCKSDB_READ_PATH_AUDIT
                        rocksdb::AuditOpScope scope(rocksdb::AuditOpType::kPut);
#endif
                        auto op_start = std::chrono::steady_clock::now();
                        std::string key = FormatKey(rec.key1);
                        std::string val = GeneratePutValue(rec.key1, rec.phase_id, rec.worker_local_op_index, 256);
                        rocksdb::Status s_put = db->Put(wopts, key, val);
                        CHECK_INVARIANT(s_put.ok(), "Worker %d Put key %lu failed: %s", w, rec.key1, s_put.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_ops[w][phase][static_cast<size_t>(rocksdb::AuditOpType::kPut)].Record(lat_ns);

                    } else if (rec.op_type == 3) { // DeleteRange
#ifdef ROCKSDB_READ_PATH_AUDIT
                        rocksdb::AuditOpScope scope(rocksdb::AuditOpType::kDeleteRange);
#endif
                        auto op_start = std::chrono::steady_clock::now();
                        std::string k1 = FormatKey(rec.key1);
                        std::string k2 = FormatKey(rec.key2);
                        rocksdb::Status s_del = db->DeleteRange(wopts, k1, k2);
                        CHECK_INVARIANT(s_del.ok(), "Worker %d DeleteRange %lu..%lu failed: %s", w, rec.key1, rec.key2, s_del.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_ops[w][phase][static_cast<size_t>(rocksdb::AuditOpType::kDeleteRange)].Record(lat_ns);

                    } else {
                        CHECK_INVARIANT(false, "Unknown op_type: %u", rec.op_type);
                    }
                }

                // TakeAndReset into worker exclusive phase slot
#ifdef ROCKSDB_READ_PATH_AUDIT
                if (cfg.mode == "audit" || cfg.mode == "smoke") {
                    for (size_t op = 0; op < static_cast<size_t>(rocksdb::AuditOpType::kMax); ++op) {
                        worker_snapshots[w][phase].stats[op] = *rocksdb::GetReadPathAuditStats(static_cast<rocksdb::AuditOpType>(op));
                    }
                    worker_snapshots[w][phase].amtv_get_stats = rocksdb::tl_amtv_get_probe_stats;
                    rocksdb::ResetAllAuditStats();
                    rocksdb::tl_amtv_get_probe_stats.Reset();
                }
#endif
                sync_barrier.arrive_and_wait(); // Signal phase complete
            }
        });
    }

    // Phase Execution Coordination
    std::vector<std::string> phase_names = {"Phase A (Baseline)", "Phase B (Dynamic Injection)", "Phase C (Post-Injection Read-Dominant)"};

    for (int p = 0; p < 3; ++p) {
        std::cout << "\n>>> Starting " << phase_names[p] << " (100,000 ops)...\n";
        auto p_start = std::chrono::steady_clock::now();
        sync_barrier.arrive_and_wait(); // Release workers
        sync_barrier.arrive_and_wait(); // Wait for workers to finish phase
        auto p_end = std::chrono::steady_clock::now();
        phase_elapsed_sec[p] = std::chrono::duration<double>(p_end - p_start).count();
        double iops = 100000.0 / phase_elapsed_sec[p];
        std::cout << "  Finished " << phase_names[p] << " in " << std::fixed << std::setprecision(4)
                  << phase_elapsed_sec[p] << " s (" << std::fixed << std::setprecision(1) << iops << " IOPS)\n";
    }

    for (auto& t : workers) t.join();
    BoundarySnapshot b1 = capture_boundary();

    // 6. Fixed 10-Second Cooldown Window (W2)
    std::cout << "\n>>> Starting Fixed 10-Second Cooldown Window (W2)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(10));
    BoundarySnapshot b2 = capture_boundary();

    // 7. Settled Drain Window (W3)
    std::cout << ">>> Starting Settled Drain Window (W3)...\n";
    for (int retry = 0; retry < 500; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0, pending_bytes = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
        bool amtv_stable = true;
        if (options.enable_amtv && cfd->mem() && cfd->mem()->GetAMTVState()) {
            amtv_stable = cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (running_flushes == 0 && running_compactions == 0 && pending_bytes == 0 && amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    BoundarySnapshot b3 = capture_boundary();

    // 8. 500,000 Key Verification against ExternalStateModel
    std::cout << "\n>>> Starting 500,000 Key Verification against ExternalStateModel...\n";
    listener->StartVerificationStage();
    ExternalStateModel model(cfg.total_keys);
    for (int w = 0; w < cfg.num_workers; ++w) {
        for (const auto& rec : worker_traces[w]) {
            if (rec.op_type == 2) {
                std::string val = GeneratePutValue(rec.key1, rec.phase_id, rec.worker_local_op_index, 256);
                model.ApplyPut(rec.key1, val);
            } else if (rec.op_type == 3) {
                model.ApplyDeleteRange(rec.key1, rec.key2);
            }
        }
    }

    auto [expected_live_count, expected_model_sha] = model.ComputeStateDigest();
    CHECK_INVARIANT(expected_live_count == 300000, "Model live count mismatch: expected 300000, got %lu", expected_live_count);

    uint64_t verified_live = 0, verified_deleted = 0;
    rocksdb::ReadOptions v_ropts;
    SHA256_CTX v_ctx;
    SHA256_Init(&v_ctx);

    for (uint64_t k = 0; k < cfg.total_keys; ++k) {
        std::string key_str = FormatKey(k);
        std::string val;
        rocksdb::Status s_get = db->Get(v_ropts, key_str, &val);
        const std::string& exp_val = model.Get(k);
        if (!exp_val.empty()) {
            CHECK_INVARIANT(s_get.ok(), "Key %lu expected LIVE but returned %s", k, s_get.ToString().c_str());
            CHECK_INVARIANT(val == exp_val, "Key %lu value mismatch!", k);
            verified_live++;
            SHA256_Update(&v_ctx, key_str.data(), key_str.size());
            SHA256_Update(&v_ctx, val.data(), val.size());
        } else {
            CHECK_INVARIANT(s_get.IsNotFound(), "Key %lu expected DELETED but returned %s", k, s_get.ToString().c_str());
            verified_deleted++;
        }
    }

    unsigned char v_hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(v_hash, &v_ctx);
    std::ostringstream v_oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        v_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(v_hash[i]);
    }
    std::string db_state_sha = v_oss.str();
    CHECK_INVARIANT(db_state_sha == expected_model_sha, "State SHA mismatch! DB: %s, Model: %s", db_state_sha.c_str(), expected_model_sha.c_str());
    std::cout << "  [PASS] 500,000 Keys verified (300,000 Live, 200,000 Deleted).\n";
    std::cout << "  [PASS] State SHA-256: " << db_state_sha << "\n";

    // 9. Iterator Full Scan Key Count Check
    uint64_t iter_scan_count = 0;
    {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(v_ropts));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            iter_scan_count++;
        }
    }
    CHECK_INVARIANT(iter_scan_count == 300000, "Iterator visible count mismatch: expected 300000, got %lu", iter_scan_count);
    std::cout << "  [PASS] Full DB Iterator Scan visible keys: " << iter_scan_count << "\n";

    // 10. Check fallback events & T0 flush constraints
    uint64_t fallback_events = 0;
    if (options.enable_amtv && cfd->mem() && cfd->mem()->GetAMTVState()) {
        fallback_events = cfd->mem()->GetAMTVState()->fallback_event_count();
        CHECK_INVARIANT(fallback_events == 0, "AMTV Fallback occurred! (%lu events)", fallback_events);
        std::cout << "  [PASS] AMTV Fallback events strictly ZERO.\n";
    }

    // 11. Parse LOG for T512 generation metrics
    uint64_t flushed_tombstones_total = 0;
    uint32_t flush_count = 0;
    struct GenerationFlushRecord {
        uint32_t generation = 0;
        uint64_t range_deletions = 0;
        std::string flush_reason;
    };
    std::vector<GenerationFlushRecord> gen_flush_info;

    {
        std::ifstream logf(cfg.db_path + "/LOG");
        std::string line;
        while (std::getline(logf, line)) {
            if (line.find("EVENT_LOG_v1") != std::string::npos &&
                line.find("\"event\": \"flush_started\"") != std::string::npos) {
                flush_count++;
                uint64_t rds = 0;
                auto rd_pos = line.find("\"num_range_deletes\": ");
                if (rd_pos != std::string::npos) {
                    rds = std::stoull(line.substr(rd_pos + 21));
                }
                std::string reason = "Unknown";
                auto r_pos = line.find("\"flush_reason\": \"");
                if (r_pos != std::string::npos) {
                    auto end_r = line.find("\"", r_pos + 17);
                    if (end_r != std::string::npos) {
                        reason = line.substr(r_pos + 17, end_r - (r_pos + 17));
                    }
                }
                flushed_tombstones_total += rds;
                gen_flush_info.push_back({flush_count, rds, reason});
            }
        }
    }

    uint64_t active_mem_tombstones = cfd->mem() ? cfd->mem()->NumRangeDeletion() : 0;
    if (cfg.config_name == "Native-T512" || cfg.config_name == "AMTV-T512") {
        uint64_t sum_check = flushed_tombstones_total + active_mem_tombstones;
        CHECK_INVARIANT(sum_check == 20000, "Tombstone conservation mismatch! Flushed: %lu, Active: %lu, Total: %lu != 20000",
                        flushed_tombstones_total, active_mem_tombstones, sum_check);
        std::cout << "  [PASS] T512 Tombstone Conservation Verified: Flushed=" << flushed_tombstones_total
                  << " (" << flush_count << " flushes) + Active=" << active_mem_tombstones << " = 20,000\n";
    } else if (cfg.config_name == "Native-T0" || cfg.config_name == "AMTV-T0") {
        CHECK_INVARIANT(flush_count == 0, "T0 configuration produced unexpected flushes: %u", flush_count);
        CHECK_INVARIANT(active_mem_tombstones == 20000, "T0 active tombstones mismatch: expected 20000, got %lu", active_mem_tombstones);
        std::cout << "  [PASS] T0 Zero Flush & 20,000 MemTable Tombstone Conservation Verified.\n";
    }

    // 12. Aggregate multi-threaded per-op stats across all workers
    struct AggregatedOpMetrics {
        std::string op_name;
        uint64_t count = 0;
        double p50_us = 0.0, p95_us = 0.0, p99_us = 0.0, p999_us = 0.0, max_us = 0.0, avg_us = 0.0;
        uint64_t total_returned_keys = 0;
        double avg_returned_keys = 0.0;
        double scan_cost_per_key_us = 0.0;
        uint64_t view_materialization_count = 0;
        uint64_t view_materialization_nanos = 0;
        uint64_t reader_mutex_attempt_count = 0;
        uint64_t reader_mutex_contended_count = 0;
        uint64_t reader_mutex_contended_wait_nanos = 0;
        uint64_t reader_mutex_race_hit_count = 0;
        uint64_t scan_reseek_count = 0;
        uint64_t scan_boundary_advance_count = 0;
        uint64_t scan_child_next_count = 0;
        uint64_t scan_covered_skip_count = 0;
    };

    std::vector<std::vector<AggregatedOpMetrics>> phase_op_metrics(3);

    for (int p = 0; p < 3; ++p) {
        for (size_t op_idx = 1; op_idx < static_cast<size_t>(rocksdb::AuditOpType::kMax); ++op_idx) {
            rocksdb::AuditOpType op_type = static_cast<rocksdb::AuditOpType>(op_idx);
            LatencyHistogram merged_hist;
#ifdef ROCKSDB_READ_PATH_AUDIT
            rocksdb::ReadPathAuditStats merged_stats;
#endif
            uint64_t ret_keys = 0;
            uint64_t dur_ns = 0;

            for (int w = 0; w < cfg.num_workers; ++w) {
                merged_hist.MergeFrom(hist_ops[w][p][op_idx]);
#ifdef ROCKSDB_READ_PATH_AUDIT
                merged_stats.MergeFrom(worker_snapshots[w][p].stats[op_idx]);
#endif
                ret_keys += worker_snapshots[w][p].returned_keys[op_idx];
                dur_ns += worker_snapshots[w][p].scan_duration_nanos[op_idx];
            }

            if (merged_hist.Count() == 0) continue;

            AggregatedOpMetrics m;
            m.op_name = rocksdb::AuditOpTypeName(op_type);
            m.count = merged_hist.Count();
            m.avg_us = merged_hist.MeanMicros();
            m.p50_us = merged_hist.PercentileMicros(50.0);
            m.p95_us = merged_hist.PercentileMicros(95.0);
            m.p99_us = merged_hist.PercentileMicros(99.0);
            m.p999_us = merged_hist.PercentileMicros(99.9);
            m.max_us = merged_hist.MaxMicros();
            m.total_returned_keys = ret_keys;
            m.avg_returned_keys = m.count ? (static_cast<double>(ret_keys) / m.count) : 0.0;
            m.scan_cost_per_key_us = ret_keys ? (dur_ns / 1000.0 / ret_keys) : 0.0;

#ifdef ROCKSDB_READ_PATH_AUDIT
            m.view_materialization_count = merged_stats.range_tombstone_view_materialization_count;
            m.view_materialization_nanos = merged_stats.range_tombstone_view_materialization_nanos;
            m.reader_mutex_attempt_count = merged_stats.fragment_build_lock_attempt_count;
            m.reader_mutex_contended_count = merged_stats.fragment_build_lock_contended_count;
            m.reader_mutex_contended_wait_nanos = merged_stats.fragment_build_lock_contended_wait_nanos;
            m.reader_mutex_race_hit_count = merged_stats.fragment_build_cache_race_hit_count;
            m.scan_reseek_count = merged_stats.scan_range_del_reseek_count;
            m.scan_boundary_advance_count = merged_stats.scan_boundary_advance_count;
            m.scan_child_next_count = merged_stats.scan_range_del_child_next_count;
            m.scan_covered_skip_count = merged_stats.scan_covered_skip_count;
#endif

            phase_op_metrics[p].push_back(m);
        }
    }

    // Print Formatted Summary Tables
    std::cout << "\n======================================================================\n";
    std::cout << "M3a Audit Phase & Operation-Type Metrics Summary (" << cfg.config_name << ")\n";
    std::cout << "======================================================================\n";

    for (int p = 0; p < 3; ++p) {
        std::cout << "\n--- " << phase_names[p] << " ---\n";
        std::cout << std::left << std::setw(24) << "Operation"
                  << std::setw(8)  << "Count"
                  << std::setw(10) << "P50(us)"
                  << std::setw(10) << "P99(us)"
                  << std::setw(10) << "Max(us)"
                  << std::setw(12) << "RetKeys"
                  << std::setw(14) << "MatCount"
                  << std::setw(18) << "MatTime(ns)"
                  << std::setw(14) << "LockContended"
                  << std::setw(18) << "LockWait(ns)"
                  << std::setw(10) << "Reseeks"
                  << "\n";
        for (const auto& m : phase_op_metrics[p]) {
            std::cout << std::left << std::setw(24) << m.op_name
                      << std::setw(8)  << m.count
                      << std::setw(10) << std::fixed << std::setprecision(2) << m.p50_us
                      << std::setw(10) << std::fixed << std::setprecision(2) << m.p99_us
                      << std::setw(10) << std::fixed << std::setprecision(2) << m.max_us
                      << std::setw(12) << m.total_returned_keys
                      << std::setw(14) << m.view_materialization_count
                      << std::setw(18) << m.view_materialization_nanos
                      << std::setw(14) << m.reader_mutex_contended_count
                      << std::setw(18) << m.reader_mutex_contended_wait_nanos
                      << std::setw(10) << m.scan_reseek_count
                      << "\n";
        }
    }

    // 13. Serialize complete JSON report
    std::string out_json_path = cfg.output_dir + "/" + cfg.exp_id + ".json";
    std::string mkdir_cmd = "mkdir -p " + cfg.output_dir;
    ret = system(mkdir_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to create output dir: %s", cfg.output_dir.c_str());

    std::ofstream jf(out_json_path);
    CHECK_INVARIANT(jf.is_open(), "Failed to open json output path: %s", out_json_path.c_str());

    jf << "{\n";
    jf << "  \"exp_id\": \"" << cfg.exp_id << "\",\n";
    jf << "  \"config_name\": \"" << cfg.config_name << "\",\n";
    jf << "  \"mode\": \"" << cfg.mode << "\",\n";
    jf << "  \"rep\": " << cfg.rep << ",\n";
    jf << "  \"trace_dir\": \"" << cfg.trace_dir << "\",\n";
    jf << "  \"db_state_sha256\": \"" << db_state_sha << "\",\n";
    jf << "  \"total_keys\": " << cfg.total_keys << ",\n";
    jf << "  \"verified_live_keys\": " << verified_live << ",\n";
    jf << "  \"verified_deleted_keys\": " << verified_deleted << ",\n";
    jf << "  \"iterator_scan_visible_keys\": " << iter_scan_count << ",\n";
    jf << "  \"fallback_events\": " << fallback_events << ",\n";
    jf << "  \"flush_count\": " << flush_count << ",\n";
    jf << "  \"flushed_tombstones_total\": " << flushed_tombstones_total << ",\n";
    jf << "  \"active_mem_tombstones\": " << active_mem_tombstones << ",\n";
    jf << "  \"final_active_generation_tombstones\": " << active_mem_tombstones << ",\n";
    jf << "  \"tombstone_conservation_verified\": " << ((flushed_tombstones_total + active_mem_tombstones == 20000) ? "true" : "false") << ",\n";
    jf << "  \"t512_generations\": [\n";
    for (size_t g = 0; g < gen_flush_info.size(); ++g) {
        jf << "    {\"generation\": " << gen_flush_info[g].generation
           << ", \"range_deletions\": " << gen_flush_info[g].range_deletions
           << ", \"flush_reason\": \"" << gen_flush_info[g].flush_reason << "\"}"
           << (g + 1 < gen_flush_info.size() ? ",\n" : "\n");
    }
    jf << "  ],\n";
    jf << "  \"three_window_io\": {\n";
    jf << "    \"w1_fg_flush_bytes\": " << (b1.flush_bytes - b0.flush_bytes) << ",\n";
    jf << "    \"w1_fg_compaction_write_bytes\": " << (b1.compaction_write_bytes - b0.compaction_write_bytes) << ",\n";
    jf << "    \"w2_cooldown_flush_bytes\": " << (b2.flush_bytes - b1.flush_bytes) << ",\n";
    jf << "    \"w2_cooldown_compaction_write_bytes\": " << (b2.compaction_write_bytes - b1.compaction_write_bytes) << ",\n";
    jf << "    \"w3_drain_flush_bytes\": " << (b3.flush_bytes - b2.flush_bytes) << ",\n";
    jf << "    \"w3_drain_compaction_write_bytes\": " << (b3.compaction_write_bytes - b2.compaction_write_bytes) << ",\n";
    jf << "    \"three_window_total_flush_bytes\": " << (b3.flush_bytes - b0.flush_bytes) << ",\n";
    jf << "    \"three_window_total_compaction_write_bytes\": " << (b3.compaction_write_bytes - b0.compaction_write_bytes) << "\n";
    jf << "  },\n";
    jf << "  \"phases\": [\n";
    for (int p = 0; p < 3; ++p) {
        jf << "    {\n";
        jf << "      \"phase_id\": " << p << ",\n";
        jf << "      \"phase_name\": \"" << phase_names[p] << "\",\n";
        jf << "      \"elapsed_seconds\": " << phase_elapsed_sec[p] << ",\n";
        jf << "      \"iops\": " << (100000.0 / phase_elapsed_sec[p]) << ",\n";
        jf << "      \"operations\": [\n";
        for (size_t i = 0; i < phase_op_metrics[p].size(); ++i) {
            const auto& m = phase_op_metrics[p][i];
            jf << "        {\n";
            jf << "          \"op_name\": \"" << m.op_name << "\",\n";
            jf << "          \"count\": " << m.count << ",\n";
            jf << "          \"avg_us\": " << m.avg_us << ",\n";
            jf << "          \"p50_us\": " << m.p50_us << ",\n";
            jf << "          \"p95_us\": " << m.p95_us << ",\n";
            jf << "          \"p99_us\": " << m.p99_us << ",\n";
            jf << "          \"p999_us\": " << m.p999_us << ",\n";
            jf << "          \"max_us\": " << m.max_us << ",\n";
            jf << "          \"total_returned_keys\": " << m.total_returned_keys << ",\n";
            jf << "          \"avg_returned_keys\": " << m.avg_returned_keys << ",\n";
            jf << "          \"scan_cost_per_key_us\": " << m.scan_cost_per_key_us << ",\n";
            jf << "          \"cumulative_thread_side_materialization_count\": " << m.view_materialization_count << ",\n";
            jf << "          \"cumulative_thread_side_materialization_nanos\": " << m.view_materialization_nanos << ",\n";
            jf << "          \"reader_mutex_attempt_count\": " << m.reader_mutex_attempt_count << ",\n";
            jf << "          \"reader_mutex_contended_count\": " << m.reader_mutex_contended_count << ",\n";
            jf << "          \"cumulative_thread_side_reader_mutex_wait_nanos\": " << m.reader_mutex_contended_wait_nanos << ",\n";
            jf << "          \"reader_mutex_race_hit_count\": " << m.reader_mutex_race_hit_count << ",\n";
            jf << "          \"scan_reseek_count\": " << m.scan_reseek_count << ",\n";
            jf << "          \"scan_boundary_advance_count\": " << m.scan_boundary_advance_count << ",\n";
            jf << "          \"scan_child_next_count\": " << m.scan_child_next_count << ",\n";
            jf << "          \"scan_covered_skip_count\": " << m.scan_covered_skip_count << "\n";
            jf << "        }" << (i + 1 < phase_op_metrics[p].size() ? "," : "") << "\n";
        }
        jf << "      ]\n";
        jf << "    }" << (p + 1 < 3 ? "," : "") << "\n";
    }
    jf << "  ]\n";
    jf << "}\n";
    jf.close();

    std::cout << "\nResults successfully written to: " << out_json_path << "\n";
    db.reset();
    return 0;
}
