#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <barrier>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <openssl/sha.h>

#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/listener.h"

#include "db/column_family.h"
#include "db/memtable.h"
#include "db/amtv.h"

#ifdef ROCKSDB_READ_PATH_AUDIT
#include "db/read_path_audit.h"
#endif

#include "tools/formal_v2/thread_local_histogram.h"
#include "tools/formal_v2/formal_event_listener.h"

#define CHECK_INVARIANT(cond, fmt, ...)                                      \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "\n======================================================================\n"); \
      fprintf(stderr, "[FATAL FAIL-FAST INVARIANT VIOLATION]\n");            \
      fprintf(stderr, "File: %s, Line: %d, Function: %s\n", __FILE__, __LINE__, __func__); \
      fprintf(stderr, "Condition failed: %s\n", #cond);                      \
      fprintf(stderr, "Details: " fmt "\n", ##__VA_ARGS__);                  \
      fprintf(stderr, "======================================================================\n"); \
      fflush(stderr);                                                        \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

#pragma pack(push, 1)
struct FormalTraceRecord {
    uint8_t  phase_id;      // 0=A, 1=B, 2=C
    uint8_t  op_type;       // 0=Get, 2=Put, 3=DeleteRange
    uint8_t  scan_mode;     // 0
    uint8_t  flags;         // 0
    uint32_t op_id;
    uint64_t key1;
    uint64_t key2;
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

// ---------------------------------------------------------------------------
// External State Model for Absolute Ground-Truth State Reconciliation
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Benchmark Configuration & Driver Options
// ---------------------------------------------------------------------------
struct M2dRunConfig {
    std::string exp_id = "diagnostic_b64_h16";
    std::string config_name = "B64-H16";
    std::string db_path = "./run-db/m2d/test_db";
    std::string seed_db_path = "./run-db/m2d_canonical_seed_db";
    std::string trace_dir = "./traces/m2d_getonly_dynamic_500k";
    std::string output_dir = "./results/amtv_m2d/raw/diagnostic";
    std::string mode = "diagnostic"; // "diagnostic", "audit", or "release"
    int rep = 1;
    uint64_t total_keys = 500000;
    size_t value_size = 256;
    int num_workers = 8;
    uint64_t warmup_ops = 50000;
    int amtv_delta_tombstones = 0;
    int amtv_hard_layer_limit = 0;
};

static void BuildCanonicalSeedDb(const std::string& seed_path, uint64_t total_keys) {
    std::cout << "\n======================================================================\n";
    std::cout << "Building Canonical Seed DB at: " << seed_path << "\n";
    std::cout << "Total keys: " << total_keys << ", Value size: 256 bytes\n";
    std::cout << "======================================================================\n";

    std::string cmd = "rm -rf " + seed_path + " && mkdir -p " + seed_path;
    int ret = system(cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to clean and create seed dir: %s", seed_path.c_str());

    rocksdb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.target_file_size_base = 64 * 1024 * 1024;
    options.max_background_jobs = 8;

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(options, seed_path, &db);
    CHECK_INVARIANT(s.ok(), "Failed to open seed DB: %s", s.ToString().c_str());

    rocksdb::WriteOptions wopts;
    wopts.disableWAL = false;

    for (uint64_t i = 0; i < total_keys; ++i) {
        std::string key = FormatKey(i);
        std::string val = GenerateInitialValue(i, 256);
        s = db->Put(wopts, key, val);
        CHECK_INVARIANT(s.ok(), "Put failed at key %lu: %s", i, s.ToString().c_str());
    }

    std::cout << "  Preload of 500,000 keys complete. Triggering Flush...\n";
    rocksdb::FlushOptions fopts;
    fopts.wait = true;
    s = db->Flush(fopts);
    CHECK_INVARIANT(s.ok(), "Flush failed: %s", s.ToString().c_str());

    // Wait for background compactions/work to finish
    std::cout << "  Waiting for background work to completely settle...\n";
    for (int retry = 0; retry < 200; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        if (running_flushes == 0 && running_compactions == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify 500,000 keys
    uint64_t count = 0;
    {
        rocksdb::ReadOptions ropts;
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ropts));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            count++;
        }
    }
    CHECK_INVARIANT(count == total_keys, "Seed verification mismatch: expected %lu, got %lu", total_keys, count);

    db.reset();
    std::cout << "  Canonical Seed DB closed successfully (" << count << " keys verified).\n";
    std::cout << "======================================================================\n";
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

    M2dRunConfig cfg;
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
        else if (arg == "--amtv-delta-tombstones" && i + 1 < argc) cfg.amtv_delta_tombstones = std::stoi(argv[++i]);
        else if (arg == "--amtv-hard-layer-limit" && i + 1 < argc) cfg.amtv_hard_layer_limit = std::stoi(argv[++i]);
    }

    std::cout << "\n======================================================================\n";
    std::cout << "Starting AMTV M2d Run: " << cfg.exp_id << " (Config: " << cfg.config_name 
              << ", Mode: " << cfg.mode << ", Rep: " << cfg.rep << ")\n";
    std::cout << "======================================================================\n";

    // Reset AMTV timeline logger and configure probe stats
    rocksdb::AMTVTimelineLogger::Get().Reset();
#ifdef ROCKSDB_READ_PATH_AUDIT
    rocksdb::g_amtv_get_probe_stats_enabled.store(true, std::memory_order_relaxed);
#endif

    // 1. Verify CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    std::string affinity_str = "";
    int core_count = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
            if (!affinity_str.empty()) affinity_str += ",";
            affinity_str += std::to_string(i);
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

    if (cfg.config_name == "Native-T0") {
        options.enable_amtv = false;
        options.memtable_max_range_deletions = 0;
    } else if (cfg.config_name == "Native-T512") {
        options.enable_amtv = false;
        options.memtable_max_range_deletions = 512;
    } else if (cfg.config_name == "B64-H16" || cfg.config_name == "AMTV-M2c-T0") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 0;
        options.amtv_delta_tombstones = 64;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 16;
    } else if (cfg.config_name == "B64-H32" || cfg.config_name == "AMTV-T0") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 0;
        options.amtv_delta_tombstones = 64;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 32;
    } else if (cfg.config_name == "B128-H16") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 0;
        options.amtv_delta_tombstones = 128;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 16;
    } else if (cfg.config_name == "B256-H16") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 0;
        options.amtv_delta_tombstones = 256;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 16;
    } else if (cfg.config_name == "AMTV-M2c-T512" || cfg.config_name == "AMTV-T512") {
        options.enable_amtv = true;
        options.memtable_max_range_deletions = 512;
        options.amtv_delta_tombstones = 64;
        options.amtv_merge_soft_limit = 2;
        options.amtv_hard_layer_limit = 32;
    } else {
        CHECK_INVARIANT(false, "Unknown config name: %s", cfg.config_name.c_str());
    }

    if (cfg.amtv_delta_tombstones > 0) {
        options.amtv_delta_tombstones = cfg.amtv_delta_tombstones;
    }
    if (cfg.amtv_hard_layer_limit > 0) {
        options.amtv_hard_layer_limit = cfg.amtv_hard_layer_limit;
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
    rocksdb::AMTVTimelineLogger::Get().SetPhase("WARMUP");
    {
        std::vector<std::thread> warmup_workers;
        warmup_workers.reserve(cfg.num_workers);
        for (int w = 0; w < cfg.num_workers; ++w) {
            warmup_workers.emplace_back([&, w]() {
                rocksdb::ReadOptions ropts;
                std::string val;
                uint64_t base_k = w * 62500;
                for (uint64_t i = 0; i < 1250; ++i) {
                    uint64_t k = base_k + ((i * 17) % 37500);
                    std::string key = FormatKey(k);
                    rocksdb::Status ws = db->Get(ropts, key, &val);
                    CHECK_INVARIANT(ws.ok(), "Warmup Get failed at key %lu: %s", k, ws.ToString().c_str());
                }
            });
        }
        for (auto& t : warmup_workers) t.join();
    }

    // Wait for any background activity to settle after warmup
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

    // 6. Reset Statistics and Establish Baseline
    options.statistics->Reset();
#ifdef ROCKSDB_READ_PATH_AUDIT
    if (cfg.mode == "audit" || cfg.mode == "smoke") {
        rocksdb::SetReadPathAuditEnabled(true);
        rocksdb::GetReadPathAuditStats()->Reset();
    }
#endif

    uint64_t init_merge_completed = 0;
    uint64_t init_merge_requested = 0;
    if (options.enable_amtv && cfd->mem() && cfd->mem()->GetAMTVState()) {
        init_merge_completed = cfd->mem()->GetAMTVState()->merge_completed();
        init_merge_requested = cfd->mem()->GetAMTVState()->merge_requested();
    }

    listener->StartForegroundExperiment();

    // Histograms per worker and per phase
    // 8 workers, 3 phases, 3 op types (0: GetLive, 1: Put, 2: DeleteRange)
    std::vector<std::vector<study::formal::ThreadLocalHistogram>> hist_get(cfg.num_workers, std::vector<study::formal::ThreadLocalHistogram>(3));
    std::vector<std::vector<study::formal::ThreadLocalHistogram>> hist_put(cfg.num_workers, std::vector<study::formal::ThreadLocalHistogram>(3));
    std::vector<std::vector<study::formal::ThreadLocalHistogram>> hist_del(cfg.num_workers, std::vector<study::formal::ThreadLocalHistogram>(3));
    std::vector<study::formal::ThreadLocalHistogram> hist_get_post_fallback(cfg.num_workers);

#ifdef ROCKSDB_READ_PATH_AUDIT
    // Worker phase audit snapshot collector
    struct WorkerPhaseAuditSnapshot {
        rocksdb::ReadPathAuditStats audit_stats;
        rocksdb::AMTVGetProbeStats probe_stats;
        uint64_t get_count = 0;
        uint64_t put_count = 0;
        uint64_t del_count = 0;
    };
    std::vector<std::vector<WorkerPhaseAuditSnapshot>> worker_phase_stats(
        cfg.num_workers, std::vector<WorkerPhaseAuditSnapshot>(3));
#endif

    std::barrier sync_barrier(cfg.num_workers + 1);

    std::atomic<uint64_t> completed_ops_total{0};
    std::vector<double> phase_elapsed_sec(3, 0.0);

    auto run_phase = [&](int phase_idx, const std::string& phase_name) {
        rocksdb::AMTVTimelineLogger::Get().SetPhase(phase_name);
        auto p_start = std::chrono::steady_clock::now();
        sync_barrier.arrive_and_wait(); // Release workers
        sync_barrier.arrive_and_wait(); // Wait for workers to finish phase
        auto p_end = std::chrono::steady_clock::now();
        phase_elapsed_sec[phase_idx] = std::chrono::duration<double>(p_end - p_start).count();
    };

    // Track Phase B End Snapshot
    uint32_t phase_b_end_sealed_runs = 0;
    uint32_t phase_b_end_open_delta_len = 0;
    std::string phase_b_end_level_hist = "{}";
    bool phase_b_end_mergeable_pairs_exist = false;
    uint64_t phase_b_end_fallback_events = 0;
    int phase_b_end_task_state = 0;
    int phase_b_end_diagnostic_phase = 0;
    uint64_t phase_b_end_merge_computed = 0;
    uint64_t phase_b_end_merge_published = 0;
    uint64_t phase_b_end_ts_us = 0;

    // Spawn 8 Worker Threads
    std::vector<std::thread> workers;
    workers.reserve(cfg.num_workers);

    for (int w = 0; w < cfg.num_workers; ++w) {
        workers.emplace_back([&, w]() {
#ifdef ROCKSDB_READ_PATH_AUDIT
            if (cfg.mode == "audit" || cfg.mode == "smoke") {
                rocksdb::GetReadPathAuditStats()->Reset();
                rocksdb::tl_amtv_get_probe_stats.Reset();
            }
#endif
            const auto& trace = worker_traces[w];
            rocksdb::ReadOptions ropts;
            rocksdb::WriteOptions wopts;
            wopts.disableWAL = false;

            size_t trace_idx = 0;

            for (int phase = 0; phase < 3; ++phase) {
                sync_barrier.arrive_and_wait(); // Wait for phase release

                size_t phase_records = 12500;
                uint64_t phase_gets = 0, phase_puts = 0, phase_dels = 0;
                for (size_t i = 0; i < phase_records; ++i) {
                    const auto& rec = trace[trace_idx++];
                    CHECK_INVARIANT(rec.phase_id == phase, "Trace phase mismatch: expected %d, got %u", phase, rec.phase_id);

                    auto op_start = std::chrono::steady_clock::now();

                    if (rec.op_type == 0) { // GetLive
                        phase_gets++;
                        std::string val;
                        std::string key = FormatKey(rec.key1);
                        rocksdb::Status s_get = db->Get(ropts, key, &val);
                        CHECK_INVARIANT(s_get.ok(), "Worker %d GetLive key %lu failed: %s", w, rec.key1, s_get.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_get[w][phase].Record(lat_ns);

                        // Check if fallback has occurred and record post-fallback Get latency
                        rocksdb::ColumnFamilyData* cur_cfd =
                            static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
                        if (cur_cfd && cur_cfd->mem()) {
                            rocksdb::AMTVState* state = cur_cfd->mem()->GetAMTVState();
                            if (state && state->is_fallback_required()) {
                                hist_get_post_fallback[w].Record(lat_ns);
                            }
                        }

                        rocksdb::AMTVTimelineLogger::Get().RecordForegroundOp(1);
                    } else if (rec.op_type == 2) { // Put
                        phase_puts++;
                        std::string key = FormatKey(rec.key1);
                        std::string val = GeneratePutValue(rec.key1, rec.phase_id, rec.op_id, 256);
                        rocksdb::Status s_put = db->Put(wopts, key, val);
                        CHECK_INVARIANT(s_put.ok(), "Worker %d Put key %lu failed: %s", w, rec.key1, s_put.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_put[w][phase].Record(lat_ns);
                        rocksdb::AMTVTimelineLogger::Get().RecordForegroundOp(1);
                    } else if (rec.op_type == 3) { // DeleteRange
                        phase_dels++;
                        std::string k1 = FormatKey(rec.key1);
                        std::string k2 = FormatKey(rec.key2);
                        rocksdb::Status s_del = db->DeleteRange(wopts, k1, k2);
                        CHECK_INVARIANT(s_del.ok(), "Worker %d DeleteRange %lu..%lu failed: %s", w, rec.key1, rec.key2, s_del.ToString().c_str());
                        auto op_end = std::chrono::steady_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
                        hist_del[w][phase].Record(lat_ns);
                        rocksdb::AMTVTimelineLogger::Get().RecordForegroundOp(1);
                        rocksdb::AMTVTimelineLogger::Get().RecordDeleteRange(1);
                    } else {
                        CHECK_INVARIANT(false, "Unknown op_type: %u", rec.op_type);
                    }
                    completed_ops_total.fetch_add(1, std::memory_order_relaxed);
                }

#ifdef ROCKSDB_READ_PATH_AUDIT
                if (cfg.mode == "audit" || cfg.mode == "smoke") {
                    auto* astats = rocksdb::GetReadPathAuditStats();
                    worker_phase_stats[w][phase].audit_stats = *astats;
                    astats->Reset();

                    worker_phase_stats[w][phase].probe_stats = rocksdb::tl_amtv_get_probe_stats;
                    rocksdb::tl_amtv_get_probe_stats.Reset();

                    worker_phase_stats[w][phase].get_count = phase_gets;
                    worker_phase_stats[w][phase].put_count = phase_puts;
                    worker_phase_stats[w][phase].del_count = phase_dels;
                }
#endif

                sync_barrier.arrive_and_wait(); // Signal phase complete
            }
        });
    }

    // -----------------------------------------------------------------------
    // Window 1: Foreground Execution Window
    // -----------------------------------------------------------------------
    std::cout << "  [Window 1] Running Phase A...\n";
    auto fg_start = std::chrono::steady_clock::now();
    run_phase(0, "PHASE_A");
    std::cout << "  [Window 1] Phase A completed in " << phase_elapsed_sec[0] << " s\n";

    std::cout << "  [Window 1] Running Phase B...\n";
    run_phase(1, "PHASE_B");
    std::cout << "  [Window 1] Phase B completed in " << phase_elapsed_sec[1] << " s\n";

    // Capture Phase B End Snapshot immediately upon Phase B completion
    {
        phase_b_end_ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        rocksdb::ColumnFamilyData* cur_cfd =
            static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
        if (cur_cfd && cur_cfd->mem()) {
            rocksdb::AMTVState* state = cur_cfd->mem()->GetAMTVState();
            if (state) {
                auto snap = state->GetSnapshot();
                if (snap) {
                    phase_b_end_sealed_runs = snap->sealed_run_count();
                    phase_b_end_open_delta_len = snap->open_delta ? static_cast<uint32_t>(snap->open_delta->size()) : 0;
                    phase_b_end_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);
                    phase_b_end_mergeable_pairs_exist = rocksdb::HasMergeablePair(snap->sealed_runs);
                }
                phase_b_end_fallback_events = state->fallback_event_count();
                phase_b_end_task_state = static_cast<int>(state->task_state());
                phase_b_end_diagnostic_phase = static_cast<int>(state->diagnostic_phase());
                phase_b_end_merge_computed = state->merge_computed_count();
                phase_b_end_merge_published = state->merge_published_count();
            }
        }
        std::cout << "  [Phase B End Snapshot] sealed_runs=" << phase_b_end_sealed_runs
                  << ", open_delta=" << phase_b_end_open_delta_len
                  << ", hist=" << phase_b_end_level_hist
                  << ", mergeable_pairs_exist=" << (phase_b_end_mergeable_pairs_exist ? "true" : "false")
                  << ", task_state=" << phase_b_end_task_state
                  << ", diag_phase=" << phase_b_end_diagnostic_phase
                  << ", fallback_events=" << phase_b_end_fallback_events << "\n";
    }

    std::cout << "  [Window 1] Running Phase C...\n";
    run_phase(2, "PHASE_C");
    std::cout << "  [Window 1] Phase C completed in " << phase_elapsed_sec[2] << " s\n";

    for (auto& t : workers) t.join();
    auto fg_end = std::chrono::steady_clock::now();
    uint64_t fg_end_ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
        fg_end.time_since_epoch()).count();
    double fg_elapsed_sec = std::chrono::duration<double>(fg_end - fg_start).count();
    double fg_iops = 300000.0 / fg_elapsed_sec;

    std::cout << "  [Window 1] Foreground finished in " << fg_elapsed_sec << " s (IOPS: " << fg_iops << ")\n";

    // -----------------------------------------------------------------------
    // Window 2: Fixed Cooldown Window (Strict 10 Seconds)
    // -----------------------------------------------------------------------
    std::cout << "  [Window 2] Entering Fixed Cooldown Window (10 seconds)...\n";
    rocksdb::AMTVTimelineLogger::Get().SetPhase("COOLDOWN");
    listener->StartCooldownObservation();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "  [Window 2] Fixed Cooldown Window completed.\n";

    // -----------------------------------------------------------------------
    // Window 3: Drain-to-Stable Window (Up to 120s)
    // -----------------------------------------------------------------------
    std::cout << "  [Window 3] Entering Drain-to-Stable Window (timeout 120s)...\n";
    rocksdb::AMTVTimelineLogger::Get().SetPhase("DRAIN");
    listener->StartVerificationStage();
    auto drain_start = std::chrono::steady_clock::now();
    bool drain_converged = false;
    double drain_elapsed_sec = 0.0;

    while (true) {
        uint64_t running_flushes = 0;
        uint64_t running_compactions = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);

        bool amtv_stable = true;
        if (options.enable_amtv) {
            rocksdb::ColumnFamilyData* cur_cfd =
                static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
            if (cur_cfd && cur_cfd->mem()) {
                rocksdb::AMTVState* state = cur_cfd->mem()->GetAMTVState();
                if (state) {
                    bool is_idle = state->IsMergeStable();
                    bool no_queued_running = (state->diagnostic_phase() == rocksdb::AMTVDiagnosticPhase::kIdle);
                    auto snap = state->GetSnapshot();

                    if (options.memtable_max_range_deletions == 0) {
                        uint32_t chunk_tombstones = (options.amtv_delta_tombstones > 0) ? options.amtv_delta_tombstones : 64;
                        uint32_t expected_stable_runs = std::popcount(static_cast<uint64_t>(20000 / chunk_tombstones));
                        uint32_t cur_runs = snap ? snap->sealed_run_count() : 0;
                        int32_t signed_backlog = static_cast<int32_t>(cur_runs) - static_cast<int32_t>(expected_stable_runs);

                        CHECK_INVARIANT(signed_backlog >= 0,
                            "Fatal: signed_backlog is negative (%d)! cur_runs=%u, expected_stable_runs=%u",
                            signed_backlog, cur_runs, expected_stable_runs);

                        amtv_stable = (is_idle && no_queued_running && (signed_backlog == 0));
                    } else {
                        // For AMTV-T512 (memtable_max_range_deletions > 0), memtables flush at threshold,
                        // so sealed_run count reflects only the current active memtable generation.
                        // Stability is reached when background merge is idle and no merge is queued/running.
                        amtv_stable = (is_idle && no_queued_running);
                    }
                }
            }
        }

        if (running_flushes == 0 && running_compactions == 0 && amtv_stable) {
            drain_converged = true;
            drain_elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - drain_start).count();
            std::cout << "  [Window 3] System completely settled in " << drain_elapsed_sec << " s.\n";
            break;
        }

        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - drain_start).count();
        if (elapsed > 120.0) {
            drain_elapsed_sec = elapsed;
            std::cout << "  [Window 3 WARNING] Drain timeout (120s). Remaining state: flushes=" << running_flushes 
                      << ", compactions=" << running_compactions << ", amtv_stable=" << amtv_stable << "\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Capture Drained Final State
    uint32_t drained_sealed_runs = 0;
    uint32_t drained_open_delta_len = 0;
    std::string drained_level_hist = "{}";
    bool theoretical_distribution_matched = false;

    if (options.enable_amtv) {
        rocksdb::ColumnFamilyData* cur_cfd =
            static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
        if (cur_cfd && cur_cfd->mem()) {
            rocksdb::AMTVState* state = cur_cfd->mem()->GetAMTVState();
            if (state) {
                auto snap = state->GetSnapshot();
                if (snap) {
                    drained_sealed_runs = snap->sealed_run_count();
                    drained_open_delta_len = snap->open_delta ? static_cast<uint32_t>(snap->open_delta->size()) : 0;
                    drained_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);

                    // Verify theoretical distribution under 20,000 tombstones:
                    // B64:  312 chunks = L8:1 (256), L5:1 (32), L4:1 (16), L3:1 (8) + 32 delta
                    // B128: 156 chunks = L7:1 (128), L4:1 (16), L3:1 (8),  L2:1 (4) + 32 delta
                    // B256:  78 chunks = L6:1 (64),  L3:1 (8),  L2:1 (4),  L1:1 (2) + 32 delta
                    if (options.amtv_delta_tombstones == 64) {
                        if (drained_sealed_runs == 4 && drained_open_delta_len == 32 &&
                            drained_level_hist == "{L3:1, L4:1, L5:1, L8:1}") {
                            theoretical_distribution_matched = true;
                        }
                    } else if (options.amtv_delta_tombstones == 128) {
                        if (drained_sealed_runs == 4 && drained_open_delta_len == 32 &&
                            drained_level_hist == "{L2:1, L3:1, L4:1, L7:1}") {
                            theoretical_distribution_matched = true;
                        }
                    } else if (options.amtv_delta_tombstones == 256) {
                        if (drained_sealed_runs == 4 && drained_open_delta_len == 32 &&
                            drained_level_hist == "{L1:1, L2:1, L3:1, L6:1}") {
                            theoretical_distribution_matched = true;
                        }
                    }
                }
            }
        }
    }

    // Verify strict final stability and conservation: signed_backlog == 0
    // Note: only applicable to AMTV-T0 where all 20,000 tombstones reside in the single active memtable.
    if (options.enable_amtv && options.memtable_max_range_deletions == 0) {
        uint32_t chunk_tombstones = (options.amtv_delta_tombstones > 0) ? options.amtv_delta_tombstones : 64;
        uint32_t expected_final_stable_runs = std::popcount(static_cast<uint64_t>(20000 / chunk_tombstones));
        int32_t final_signed_backlog = static_cast<int32_t>(drained_sealed_runs) - static_cast<int32_t>(expected_final_stable_runs);
        CHECK_INVARIANT(final_signed_backlog >= 0,
            "Fatal: final signed_backlog is negative (%d)! drained_sealed_runs=%u, expected=%u",
            final_signed_backlog, drained_sealed_runs, expected_final_stable_runs);
        CHECK_INVARIANT(final_signed_backlog == 0,
            "Fatal: final signed_backlog must be exactly 0, got %d (drained_sealed_runs=%u, expected=%u)",
            final_signed_backlog, drained_sealed_runs, expected_final_stable_runs);
    }

    // -----------------------------------------------------------------------
    // Step 4: External State Model & Full 500,000 Key DB Verification
    // -----------------------------------------------------------------------
    std::cout << "  [Verification] Executing External State Model & Full 500,000 Key Get() Reconcile...\n";
    ExternalStateModel model(cfg.total_keys);
    for (int w = 0; w < cfg.num_workers; ++w) {
        for (const auto& rec : worker_traces[w]) {
            if (rec.op_type == 2) {
                std::string val = GeneratePutValue(rec.key1, rec.phase_id, rec.op_id, 256);
                model.ApplyPut(rec.key1, val);
            } else if (rec.op_type == 3) {
                model.ApplyDeleteRange(rec.key1, rec.key2);
            }
        }
    }

    auto [expected_live_count, expected_model_sha] = model.ComputeStateDigest();
    CHECK_INVARIANT(expected_live_count == 300000, "Model live count mismatch: expected 300000, got %lu", expected_live_count);

    // 1. Full 500,000 Key Point Get() Reconciliation
    uint64_t verified_live_count = 0;
    uint64_t verified_deleted_count = 0;
    rocksdb::ReadOptions v_ropts;
    SHA256_CTX db_ctx;
    SHA256_Init(&db_ctx);

    for (uint64_t k = 0; k < cfg.total_keys; ++k) {
        std::string key_str = FormatKey(k);
        std::string val;
        rocksdb::Status s = db->Get(v_ropts, key_str, &val);
        const std::string& exp_val = model.Get(k);
        if (!exp_val.empty()) {
            CHECK_INVARIANT(s.ok(), "Key %lu expected LIVE but Get returned %s", k, s.ToString().c_str());
            CHECK_INVARIANT(val == exp_val, "Key %lu value mismatch! DB: %s, Model: %s", k, val.c_str(), exp_val.c_str());
            verified_live_count++;
            SHA256_Update(&db_ctx, key_str.data(), key_str.size());
            SHA256_Update(&db_ctx, val.data(), val.size());
        } else {
            CHECK_INVARIANT(s.IsNotFound(), "Key %lu expected DELETED but Get returned %s", k, s.ToString().c_str());
            verified_deleted_count++;
        }
    }

    CHECK_INVARIANT(verified_live_count == 300000, "Live count mismatch: expected 300000, got %lu", verified_live_count);
    CHECK_INVARIANT(verified_deleted_count == 200000, "Deleted count mismatch: expected 200000, got %lu", verified_deleted_count);

    unsigned char db_hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(db_hash, &db_ctx);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(db_hash[i]);
    }
    std::string db_sha256 = oss.str();

    CHECK_INVARIANT(db_sha256 == expected_model_sha, "DB SHA-256 mismatch! DB: %s, Model: %s", db_sha256.c_str(), expected_model_sha.c_str());
    std::cout << "  [PASS] All 500,000 keys verified (300,000 live, 200,000 deleted) bit-for-bit against State Model (SHA-256: " << db_sha256 << ")\n";

    // 2. Scan Iterator Visibility Verification
    uint64_t db_visible_count = 0;
    {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(v_ropts));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            db_visible_count++;
        }
    }
    CHECK_INVARIANT(db_visible_count == 300000, "DB visible iterator count mismatch: expected 300000, got %lu", db_visible_count);

    // -----------------------------------------------------------------------
    // Step 5: Gather Metrics & Enforce Fail-Fast Gates
    // -----------------------------------------------------------------------
    uint64_t fg_flush_bytes = listener->GetForegroundFlushBytes();
    uint64_t fg_compaction_read_bytes = listener->GetForegroundCompactionReadBytes();
    uint64_t fg_compaction_write_bytes = listener->GetForegroundCompactionWriteBytes();
    uint64_t fg_capacity_flushes = listener->GetForegroundFlushCountByReason(rocksdb::FlushReason::kWriteBufferFull);
    uint64_t fg_threshold_flushes = listener->GetForegroundFlushCountByReason(rocksdb::FlushReason::kMemtableMaxRangeDeletions);

    // Gate 1: T0 configs must have 0 capacity flushes
    if (cfg.config_name.find("T0") != std::string::npos || cfg.config_name.find("B") == 0) {
        CHECK_INVARIANT(fg_capacity_flushes == 0, "T0 config %s experienced %lu natural capacity flushes!", cfg.config_name.c_str(), fg_capacity_flushes);
    }

    // Gather AMTV metrics
    uint64_t amtv_merge_completed = 0;
    uint64_t amtv_merge_requested = 0;
    uint64_t amtv_merge_computed = 0;
    uint64_t amtv_merge_published = 0;
    uint64_t amtv_merge_discarded = 0;
    uint64_t amtv_merge_unscheduled = 0;
    uint64_t amtv_merge_input_runs = 0;
    uint64_t amtv_merge_input_tombstones = 0;
    uint64_t amtv_fallback_events = 0;
    uint64_t amtv_fallback_gets = 0;
    uint64_t amtv_runs_at_fallback = 0;
    uint64_t amtv_tombstones_at_fallback = 0;
    uint64_t amtv_merge_wall_time_us = 0;
    uint64_t amtv_merge_cpu_time_us = 0;
    uint64_t amtv_task_queue_wait_time_us = 0;
    uint64_t amtv_raw_entries_struct_bytes_peak = 0;
    uint64_t amtv_in_flight_merge_struct_bytes_peak = 0;
    uint64_t amtv_open_delta_len = 0;
    uint64_t amtv_sealed_runs = 0;
    std::string amtv_level_hist = "{}";

    uint64_t max_single_merge_wall_time_us = 0;
    uint64_t max_single_merge_cpu_time_us = 0;
    uint32_t max_single_merge_level = 0;

    uint64_t max_computed_merge_wall_us = 0;
    uint64_t max_computed_merge_cpu_us = 0;
    uint32_t max_computed_merge_level = 0;
    uint64_t max_published_merge_wall_us = 0;
    uint64_t max_published_merge_cpu_us = 0;
    uint32_t max_published_merge_level = 0;
    uint64_t total_computed_merge_wall_us = 0;
    uint64_t total_computed_merge_cpu_us = 0;
    uint64_t total_published_merge_wall_us = 0;
    uint64_t total_published_merge_cpu_us = 0;

    uint64_t raw_entry_payload_bytes_peak = 0;
    uint64_t raw_entry_capacity_proxy_bytes_peak = 0;
    uint64_t fragment_payload_proxy_bytes_peak = 0;
    uint64_t inflight_payload_proxy_bytes_peak = 0;

    std::map<uint32_t, uint64_t> merge_count_per_lvl;
    std::map<uint32_t, uint64_t> merge_tombstones_per_lvl;
    std::map<uint32_t, uint64_t> merge_wall_us_per_lvl;
    std::map<uint32_t, uint64_t> merge_cpu_us_per_lvl;
    std::map<uint32_t, uint64_t> merge_wait_us_per_lvl;

    if (options.enable_amtv) {
        rocksdb::ColumnFamilyData* cur_cfd =
            static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
        if (cur_cfd && cur_cfd->mem()) {
            rocksdb::AMTVState* state = cur_cfd->mem()->GetAMTVState();
            if (state) {
                amtv_merge_completed = state->merge_completed() - init_merge_completed;
                amtv_merge_requested = state->merge_requested() - init_merge_requested;
                amtv_merge_computed = state->merge_computed_count();
                amtv_merge_published = state->merge_published_count();
                amtv_merge_discarded = state->merge_discarded();
                amtv_merge_unscheduled = state->merge_unscheduled();
                amtv_merge_input_runs = state->merge_input_run_count();
                amtv_merge_input_tombstones = state->merge_input_tombstones();
                amtv_fallback_events = state->fallback_event_count();
                amtv_fallback_gets = state->get_fallback_to_native_count();
                amtv_runs_at_fallback = state->runs_at_fallback();
                amtv_tombstones_at_fallback = state->tombstones_at_fallback();
                amtv_merge_wall_time_us = state->merge_wall_time_nanos() / 1000;
                amtv_merge_cpu_time_us = state->merge_cpu_time_nanos() / 1000;
                amtv_task_queue_wait_time_us = state->task_queue_wait_time_nanos() / 1000;
                amtv_raw_entries_struct_bytes_peak = state->raw_entries_struct_bytes_peak();
                amtv_in_flight_merge_struct_bytes_peak = state->in_flight_merge_struct_bytes_peak();

                max_single_merge_wall_time_us = state->max_single_merge_wall_time_nanos() / 1000;
                max_single_merge_cpu_time_us = state->max_single_merge_cpu_time_nanos() / 1000;
                max_single_merge_level = state->max_single_merge_level();

                max_computed_merge_wall_us = state->max_computed_merge_wall_time_nanos() / 1000;
                max_computed_merge_cpu_us = state->max_computed_merge_cpu_time_nanos() / 1000;
                max_computed_merge_level = state->max_computed_merge_level();

                max_published_merge_wall_us = state->max_published_merge_wall_time_nanos() / 1000;
                max_published_merge_cpu_us = state->max_published_merge_cpu_time_nanos() / 1000;
                max_published_merge_level = state->max_published_merge_level();

                total_computed_merge_wall_us = state->total_computed_merge_wall_time_nanos() / 1000;
                total_computed_merge_cpu_us = state->total_computed_merge_cpu_time_nanos() / 1000;
                total_published_merge_wall_us = state->total_published_merge_wall_time_nanos() / 1000;
                total_published_merge_cpu_us = state->total_published_merge_cpu_time_nanos() / 1000;

                raw_entry_payload_bytes_peak = state->raw_entry_payload_bytes_peak();
                raw_entry_capacity_proxy_bytes_peak = state->raw_entry_capacity_proxy_bytes_peak();
                fragment_payload_proxy_bytes_peak = state->fragment_payload_proxy_bytes_peak();
                inflight_payload_proxy_bytes_peak = state->inflight_payload_proxy_bytes_peak();

                merge_count_per_lvl = state->merge_count_per_level();
                merge_tombstones_per_lvl = state->merge_input_tombstones_per_level();
                for (const auto& p : state->merge_wall_time_nanos_per_level()) {
                    merge_wall_us_per_lvl[p.first] = p.second / 1000;
                }
                for (const auto& p : state->merge_cpu_time_nanos_per_level()) {
                    merge_cpu_us_per_lvl[p.first] = p.second / 1000;
                }
                for (const auto& p : state->merge_queue_wait_nanos_per_level()) {
                    merge_wait_us_per_lvl[p.first] = p.second / 1000;
                }

                auto snap = state->GetSnapshot();
                if (snap) {
                    amtv_open_delta_len = snap->open_delta ? snap->open_delta->size() : 0;
                    amtv_sealed_runs = snap->sealed_runs.size();
                    amtv_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);
                }
            }
            std::cout << "  [AMTV Diagnostics] hard_layer_limit: " << state->hard_layer_limit()
                      << ", sealed_runs: " << amtv_sealed_runs
                      << ", fallback_events: " << amtv_fallback_events
                      << ", computed: " << amtv_merge_computed
                      << ", published: " << amtv_merge_published
                      << ", discarded: " << amtv_merge_discarded
                      << ", max_computed_wall_us: " << max_computed_merge_wall_us << " (L" << max_computed_merge_level << ")"
                      << ", max_published_wall_us: " << max_published_merge_wall_us << " (L" << max_published_merge_level << ")\n";
        }
    }

    // Gate 2: In formal non-diagnostic mode, fallback must be 0
    if (cfg.mode != "diagnostic") {
        CHECK_INVARIANT(amtv_fallback_events == 0, "Config %s had %lu fallback events!", cfg.config_name.c_str(), amtv_fallback_events);
        CHECK_INVARIANT(amtv_fallback_gets == 0, "Config %s had %lu fallback gets!", cfg.config_name.c_str(), amtv_fallback_gets);
    }

    // Rusage metrics
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double user_cpu_sec = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
    double sys_cpu_sec = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    uint64_t peak_rss_kb = usage.ru_maxrss;

    // Aggregate Histograms
    study::formal::ThreadLocalHistogram agg_get_all, agg_put_all, agg_del_all;
    std::vector<study::formal::ThreadLocalHistogram> agg_get_phase(3);
    std::vector<study::formal::ThreadLocalHistogram> agg_put_phase(3);
    std::vector<study::formal::ThreadLocalHistogram> agg_del_phase(3);
    study::formal::ThreadLocalHistogram agg_get_post_fallback;

    for (int w = 0; w < cfg.num_workers; ++w) {
        agg_get_post_fallback.MergeFrom(hist_get_post_fallback[w]);
        for (int p = 0; p < 3; ++p) {
            agg_get_all.MergeFrom(hist_get[w][p]);
            agg_get_phase[p].MergeFrom(hist_get[w][p]);
            agg_put_all.MergeFrom(hist_put[w][p]);
            agg_put_phase[p].MergeFrom(hist_put[w][p]);
            agg_del_all.MergeFrom(hist_del[w][p]);
            agg_del_phase[p].MergeFrom(hist_del[w][p]);
        }
    }

    double get_p50 = 0, get_p90 = 0, get_p95 = 0, get_p99 = 0, get_p999 = 0, get_mean = 0, get_max = 0;
    agg_get_all.ComputeQuantiles(get_p50, get_p90, get_p95, get_p99, get_p999, get_mean, get_max);

    double put_p50 = 0, put_p90 = 0, put_p95 = 0, put_p99 = 0, put_p999 = 0, put_mean = 0, put_max = 0;
    agg_put_all.ComputeQuantiles(put_p50, put_p90, put_p95, put_p99, put_p999, put_mean, put_max);

    double del_p50 = 0, del_p90 = 0, del_p95 = 0, del_p99 = 0, del_p999 = 0, del_mean = 0, del_max = 0;
    agg_del_all.ComputeQuantiles(del_p50, del_p90, del_p95, del_p99, del_p999, del_mean, del_max);

    double post_fallback_p50 = 0, post_fallback_p90 = 0, post_fallback_p95 = 0, post_fallback_p99 = 0, post_fallback_p999 = 0, post_fallback_mean = 0, post_fallback_max = 0;
    agg_get_post_fallback.ComputeQuantiles(post_fallback_p50, post_fallback_p90, post_fallback_p95, post_fallback_p99, post_fallback_p999, post_fallback_mean, post_fallback_max);
    uint64_t post_fallback_count = agg_get_post_fallback.GetCount();

    // Gather Audit metrics if in audit mode
    uint64_t audit_mat_count = 0;
    uint64_t audit_mat_nanos = 0;
    uint64_t audit_cache_inv_count = 0;
    uint64_t audit_lock_attempt_count = 0;
    uint64_t audit_lock_contended_count = 0;
    uint64_t audit_lock_wait_nanos = 0;

    double get_probe_avg_sealed_runs = 0.0;
    double get_probe_avg_open_delta = 0.0;
    uint32_t get_probe_max_sealed_runs = 0;
    uint32_t get_probe_max_open_delta = 0;
    uint32_t get_probe_p50_sealed_runs = 0;
    uint32_t get_probe_p90_sealed_runs = 0;
    uint32_t get_probe_p99_sealed_runs = 0;

    struct PhaseAggStats {
        uint64_t mat_count = 0;
        uint64_t mat_nanos = 0;
        uint64_t cache_inv_count = 0;
        uint64_t lock_attempt_count = 0;
        uint64_t lock_contended_count = 0;
        uint64_t lock_wait_nanos = 0;
        uint64_t gets = 0;
        uint64_t puts = 0;
        uint64_t dels = 0;
        uint64_t probe_gets = 0;
        double avg_sealed_runs = 0.0;
        uint32_t max_sealed_runs = 0;
        double avg_open_delta = 0.0;
        uint32_t max_open_delta = 0;
    };
    std::vector<PhaseAggStats> phase_audit_aggs(3);
    PhaseAggStats overall_audit_agg;

#ifdef ROCKSDB_READ_PATH_AUDIT
    if (cfg.mode == "audit" || cfg.mode == "smoke") {
        std::string wps_csv_path = cfg.output_dir + "/" + cfg.exp_id + "_worker_phase_stats.csv";
        std::ofstream wps_out(wps_csv_path);
        if (wps_out.is_open()) {
            wps_out << "worker_id,phase_id,phase_name,get_count,put_count,del_count,"
                    << "mat_count,mat_nanos,lock_attempt_count,lock_contended_count,lock_wait_nanos,cache_inv_count,"
                    << "probe_gets,probed_runs_sum,probed_runs_max,open_delta_sum,open_delta_max\n";
        }
        static const char* kPhaseNames[] = {"PHASE_A", "PHASE_B", "PHASE_C"};

        rocksdb::AMTVGetProbeStats overall_probe_stats;
        for (int p = 0; p < 3; ++p) {
            rocksdb::AMTVGetProbeStats phase_probe;
            for (int w = 0; w < cfg.num_workers; ++w) {
                const auto& snap = worker_phase_stats[w][p];
                if (wps_out.is_open()) {
                    wps_out << w << "," << p << "," << kPhaseNames[p] << ","
                            << snap.get_count << "," << snap.put_count << "," << snap.del_count << ","
                            << snap.audit_stats.range_tombstone_view_materialization_count << ","
                            << snap.audit_stats.range_tombstone_view_materialization_nanos << ","
                            << snap.audit_stats.fragment_build_lock_attempt_count << ","
                            << snap.audit_stats.fragment_build_lock_contended_count << ","
                            << snap.audit_stats.fragment_build_lock_contended_wait_nanos << ","
                            << snap.audit_stats.memtable_cache_invalidation_count << ","
                            << snap.probe_stats.get_count << ","
                            << snap.probe_stats.sealed_runs_sum << ","
                            << snap.probe_stats.sealed_runs_max << ","
                            << snap.probe_stats.open_delta_entries_sum << ","
                            << snap.probe_stats.open_delta_entries_max << "\n";
                }
                phase_audit_aggs[p].mat_count += snap.audit_stats.range_tombstone_view_materialization_count;
                phase_audit_aggs[p].mat_nanos += snap.audit_stats.range_tombstone_view_materialization_nanos;
                phase_audit_aggs[p].cache_inv_count += snap.audit_stats.memtable_cache_invalidation_count;
                phase_audit_aggs[p].lock_attempt_count += snap.audit_stats.fragment_build_lock_attempt_count;
                phase_audit_aggs[p].lock_contended_count += snap.audit_stats.fragment_build_lock_contended_count;
                phase_audit_aggs[p].lock_wait_nanos += snap.audit_stats.fragment_build_lock_contended_wait_nanos;
                phase_audit_aggs[p].gets += snap.get_count;
                phase_audit_aggs[p].puts += snap.put_count;
                phase_audit_aggs[p].dels += snap.del_count;
                phase_probe.MergeFrom(snap.probe_stats);
            }
            phase_audit_aggs[p].probe_gets = phase_probe.get_count;
            phase_audit_aggs[p].avg_sealed_runs = phase_probe.get_count > 0 ? static_cast<double>(phase_probe.sealed_runs_sum) / phase_probe.get_count : 0.0;
            phase_audit_aggs[p].max_sealed_runs = phase_probe.sealed_runs_max;
            phase_audit_aggs[p].avg_open_delta = phase_probe.get_count > 0 ? static_cast<double>(phase_probe.open_delta_entries_sum) / phase_probe.get_count : 0.0;
            phase_audit_aggs[p].max_open_delta = phase_probe.open_delta_entries_max;

            overall_audit_agg.mat_count += phase_audit_aggs[p].mat_count;
            overall_audit_agg.mat_nanos += phase_audit_aggs[p].mat_nanos;
            overall_audit_agg.cache_inv_count += phase_audit_aggs[p].cache_inv_count;
            overall_audit_agg.lock_attempt_count += phase_audit_aggs[p].lock_attempt_count;
            overall_audit_agg.lock_contended_count += phase_audit_aggs[p].lock_contended_count;
            overall_audit_agg.lock_wait_nanos += phase_audit_aggs[p].lock_wait_nanos;
            overall_audit_agg.gets += phase_audit_aggs[p].gets;
            overall_audit_agg.puts += phase_audit_aggs[p].puts;
            overall_audit_agg.dels += phase_audit_aggs[p].dels;
            overall_probe_stats.MergeFrom(phase_probe);
        }
        if (wps_out.is_open()) wps_out.close();

        overall_audit_agg.probe_gets = overall_probe_stats.get_count;
        overall_audit_agg.avg_sealed_runs = overall_probe_stats.get_count > 0 ? static_cast<double>(overall_probe_stats.sealed_runs_sum) / overall_probe_stats.get_count : 0.0;
        overall_audit_agg.max_sealed_runs = overall_probe_stats.sealed_runs_max;
        overall_audit_agg.avg_open_delta = overall_probe_stats.get_count > 0 ? static_cast<double>(overall_probe_stats.open_delta_entries_sum) / overall_probe_stats.get_count : 0.0;
        overall_audit_agg.max_open_delta = overall_probe_stats.open_delta_entries_max;

        audit_mat_count = overall_audit_agg.mat_count;
        audit_mat_nanos = overall_audit_agg.mat_nanos;
        audit_cache_inv_count = overall_audit_agg.cache_inv_count;
        audit_lock_attempt_count = overall_audit_agg.lock_attempt_count;
        audit_lock_contended_count = overall_audit_agg.lock_contended_count;
        audit_lock_wait_nanos = overall_audit_agg.lock_wait_nanos;

        get_probe_avg_sealed_runs = overall_audit_agg.avg_sealed_runs;
        get_probe_max_sealed_runs = overall_audit_agg.max_sealed_runs;
        get_probe_avg_open_delta = overall_audit_agg.avg_open_delta;
        get_probe_max_open_delta = overall_audit_agg.max_open_delta;

        if (overall_probe_stats.get_count > 0) {
            uint64_t running = 0;
            uint64_t target_p50 = static_cast<uint64_t>(overall_probe_stats.get_count * 0.50);
            uint64_t target_p90 = static_cast<uint64_t>(overall_probe_stats.get_count * 0.90);
            uint64_t target_p99 = static_cast<uint64_t>(overall_probe_stats.get_count * 0.99);
            bool found_p50 = false, found_p90 = false, found_p99 = false;
            for (uint32_t i = 0; i < 64; ++i) {
                running += overall_probe_stats.sealed_runs_hist[i];
                if (!found_p50 && running >= target_p50) {
                    get_probe_p50_sealed_runs = i;
                    found_p50 = true;
                }
                if (!found_p90 && running >= target_p90) {
                    get_probe_p90_sealed_runs = i;
                    found_p90 = true;
                }
                if (!found_p99 && running >= target_p99) {
                    get_probe_p99_sealed_runs = i;
                    found_p99 = true;
                }
            }
        }

        if (cfg.config_name == "AMTV-M2c-T0" || cfg.config_name == "AMTV-T0") {
            CHECK_INVARIANT(audit_mat_count == 0, "AMTV-T0 had %lu active MemTable materializations on GetOnly path!", audit_mat_count);
        }
    }
#endif

    // Read write-side audit timings and discarded merge metrics from AMTVState
    uint64_t amtv_write_state_lock_wait_nanos = 0;
    uint64_t amtv_write_append_nanos = 0;
    uint64_t amtv_write_snapshot_clone_nanos = 0;
    uint64_t amtv_write_seal_build_nanos = 0;
    uint64_t amtv_write_publish_nanos = 0;
    uint64_t amtv_merge_discarded_wall_time_us = 0;
    uint64_t amtv_merge_discarded_cpu_time_us = 0;

    rocksdb::ColumnFamilyData* amtv_cfd =
        static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();
    if (amtv_cfd && amtv_cfd->mem() && amtv_cfd->mem()->GetAMTVState()) {
        auto* astate = amtv_cfd->mem()->GetAMTVState();
#ifdef ROCKSDB_READ_PATH_AUDIT
        amtv_write_state_lock_wait_nanos = astate->amtv_write_state_lock_wait_nanos();
        amtv_write_append_nanos = astate->amtv_write_append_nanos();
        amtv_write_snapshot_clone_nanos = astate->amtv_write_snapshot_clone_nanos();
        amtv_write_seal_build_nanos = astate->amtv_write_seal_build_nanos();
        amtv_write_publish_nanos = astate->amtv_write_publish_nanos();
#endif
        amtv_merge_discarded_wall_time_us = astate->total_discarded_merge_wall_time_nanos() / 1000;
        amtv_merge_discarded_cpu_time_us = astate->total_discarded_merge_cpu_time_nanos() / 1000;
    }

    // Compute Engine Output Write Amplification
    uint64_t put_value_bytes = 60000ULL * 256ULL;
    double engine_output_wa = static_cast<double>(fg_flush_bytes + fg_compaction_write_bytes) / put_value_bytes;

    uint64_t l0_files_peak = 0;
    db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files_peak);
    uint64_t pending_compaction_bytes = 0;
    db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_compaction_bytes);

    // -----------------------------------------------------------------------
    // Step 6: Process Timeline Records & Compute Monotonic Clock Delta t
    // -----------------------------------------------------------------------
    std::string out_dir = cfg.output_dir;
    int sys_ret = system(("mkdir -p " + out_dir).c_str());
    (void)sys_ret;

    std::string timeline_csv_path = cfg.output_dir + "/" + cfg.exp_id + "_timeline.csv";
    std::string amtv_merge_timeline_csv = cfg.output_dir + "/amtv_merge_timeline.csv";
    rocksdb::AMTVTimelineLogger::Get().DumpToCsv(timeline_csv_path);
    rocksdb::AMTVTimelineLogger::Get().DumpToCsv(amtv_merge_timeline_csv);

    auto timeline_records = rocksdb::AMTVTimelineLogger::Get().GetRecords();

    // 1. Calculate actual SEAL Delta t distribution and Backlog Metrics
    std::vector<uint64_t> seal_timestamps_us;
    std::vector<uint64_t> seal_delta_t_us;
    std::vector<std::pair<double, double>> phase_b_backlog_points; // (time_sec, sealed_runs)
    uint64_t phase_b_first_ts_us = 0;
    uint64_t last_merge_publish_timestamp_us = 0;

    uint32_t chunk_tombstones = (options.amtv_delta_tombstones > 0) ? options.amtv_delta_tombstones : 64;
    int32_t peak_signed_backlog = 0;
    uint32_t peak_backlog_excess = 0;
    uint32_t peak_actual_sealed_runs = 0;
    uint64_t backlog_excess_max_duration_us = 0;
    uint64_t current_excess_start_ts_us = 0;
    uint32_t peak_claimed_input_runs = 0;
    uint32_t peak_scheduling_backlog = 0;

    for (size_t i = 0; i < timeline_records.size(); ++i) {
        const auto& rec = timeline_records[i];
        if (rec.sealed_run_count > peak_actual_sealed_runs) {
            peak_actual_sealed_runs = rec.sealed_run_count;
        }
        if (rec.event_type == "SEAL") {
            seal_timestamps_us.push_back(rec.monotonic_timestamp_us);
            if (seal_timestamps_us.size() > 1) {
                uint64_t dt = rec.monotonic_timestamp_us - seal_timestamps_us[seal_timestamps_us.size() - 2];
                seal_delta_t_us.push_back(dt);
            }
        }
        if (rec.event_type == "MERGE_PUBLISH") {
            last_merge_publish_timestamp_us = rec.monotonic_timestamp_us;
        }

        uint64_t del_count = rec.delete_ranges_issued;
        uint32_t stable_run_count = std::popcount(static_cast<uint64_t>(del_count / chunk_tombstones));
        int32_t signed_backlog = static_cast<int32_t>(rec.sealed_run_count) - static_cast<int32_t>(stable_run_count);
        uint32_t backlog_excess = static_cast<uint32_t>(std::max(0, signed_backlog));
        if (signed_backlog > peak_signed_backlog) peak_signed_backlog = signed_backlog;
        if (backlog_excess > peak_backlog_excess) peak_backlog_excess = backlog_excess;

        uint32_t cur_claimed = (rec.event_type == "MERGE_START" || rec.event_type == "MERGE_DONE") ? 2 : 0;
        if (cur_claimed > peak_claimed_input_runs) peak_claimed_input_runs = cur_claimed;

        int32_t cur_sched = static_cast<int32_t>(rec.sealed_run_count) - static_cast<int32_t>(cur_claimed) - static_cast<int32_t>(stable_run_count);
        uint32_t sched_backlog = static_cast<uint32_t>(std::max(0, cur_sched));
        if (sched_backlog > peak_scheduling_backlog) peak_scheduling_backlog = sched_backlog;

        if (backlog_excess > 0) {
            if (current_excess_start_ts_us == 0) {
                current_excess_start_ts_us = rec.monotonic_timestamp_us;
            }
            uint64_t cur_dur = rec.monotonic_timestamp_us - current_excess_start_ts_us;
            if (cur_dur > backlog_excess_max_duration_us) {
                backlog_excess_max_duration_us = cur_dur;
            }
        } else {
            if (current_excess_start_ts_us != 0) {
                uint64_t cur_dur = rec.monotonic_timestamp_us - current_excess_start_ts_us;
                if (cur_dur > backlog_excess_max_duration_us) {
                    backlog_excess_max_duration_us = cur_dur;
                }
                current_excess_start_ts_us = 0;
            }
        }

        if (rec.phase == "PHASE_B") {
            if (phase_b_first_ts_us == 0) {
                phase_b_first_ts_us = rec.monotonic_timestamp_us;
            }
            double rel_sec = (rec.monotonic_timestamp_us - phase_b_first_ts_us) / 1e6;
            phase_b_backlog_points.push_back({rel_sec, static_cast<double>(rec.sealed_run_count)});
        }
    }

    // Drain metric calculations
    uint64_t phase_b_end_to_merge_stable_us = 0;
    if (last_merge_publish_timestamp_us > phase_b_end_ts_us) {
        phase_b_end_to_merge_stable_us = last_merge_publish_timestamp_us - phase_b_end_ts_us;
    }
    uint64_t foreground_end_to_merge_stable_us = 0;
    if (last_merge_publish_timestamp_us > fg_end_ts_us) {
        foreground_end_to_merge_stable_us = last_merge_publish_timestamp_us - fg_end_ts_us;
    }
    uint64_t phase_b_end_merges_computed_after = (amtv_merge_computed >= phase_b_end_merge_computed)
        ? (amtv_merge_computed - phase_b_end_merge_computed) : 0;
    uint64_t phase_b_end_merges_published_after = (amtv_merge_published >= phase_b_end_merge_published)
        ? (amtv_merge_published - phase_b_end_merge_published) : 0;
    uint32_t phase_b_end_merges_publishing_only = (phase_b_end_diagnostic_phase == 4 /* kPublishing */) ? 1 : 0;

    double chunk_interval_min_us = 0;
    double chunk_interval_p50_us = 0;
    double chunk_interval_p95_us = 0;
    double chunk_interval_max_us = 0;

    if (!seal_delta_t_us.empty()) {
        std::vector<uint64_t> sorted_dt = seal_delta_t_us;
        std::sort(sorted_dt.begin(), sorted_dt.end());
        chunk_interval_min_us = static_cast<double>(sorted_dt.front());
        chunk_interval_max_us = static_cast<double>(sorted_dt.back());
        size_t idx_p50 = static_cast<size_t>(sorted_dt.size() * 0.50);
        size_t idx_p95 = static_cast<size_t>(sorted_dt.size() * 0.95);
        if (idx_p50 >= sorted_dt.size()) idx_p50 = sorted_dt.size() - 1;
        if (idx_p95 >= sorted_dt.size()) idx_p95 = sorted_dt.size() - 1;
        chunk_interval_p50_us = static_cast<double>(sorted_dt[idx_p50]);
        chunk_interval_p95_us = static_cast<double>(sorted_dt[idx_p95]);
    }

    // 2. Phase B DeleteRange arrival rate
    double phase_b_del_range_arrival_rate = (phase_elapsed_sec[1] > 0.0) ? (20000.0 / phase_elapsed_sec[1]) : 0.0;
    double phase_b_chunk_arrival_rate = phase_b_del_range_arrival_rate / chunk_tombstones;

    // 3. Phase B Backlog Regression Slope (runs / sec)
    double backlog_regression_slope = 0.0;
    if (phase_b_backlog_points.size() >= 2) {
        double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
        size_t n = phase_b_backlog_points.size();
        for (const auto& pt : phase_b_backlog_points) {
            sum_x += pt.first;
            sum_y += pt.second;
            sum_xx += pt.first * pt.first;
            sum_xy += pt.first * pt.second;
        }
        double denom = (n * sum_xx - sum_x * sum_x);
        if (std::abs(denom) > 1e-9) {
            backlog_regression_slope = (n * sum_xy - sum_x * sum_y) / denom;
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: Output JSON and Append to Summary CSV
    // -----------------------------------------------------------------------
    std::string out_json_path = cfg.output_dir + "/" + cfg.exp_id + ".json";
    std::ofstream jf(out_json_path);
    CHECK_INVARIANT(jf.is_open(), "Failed to open output JSON: %s", out_json_path.c_str());

    auto map_to_json = [](const auto& m) {
        std::ostringstream s;
        s << "{";
        bool first = true;
        for (const auto& p : m) {
            if (!first) s << ", ";
            s << "\"L" << p.first << "\": " << p.second;
            first = false;
        }
        s << "}";
        return s.str();
    };

    jf << "{\n"
       << "  \"exp_id\": \"" << cfg.exp_id << "\",\n"
       << "  \"config_name\": \"" << cfg.config_name << "\",\n"
       << "  \"mode\": \"" << cfg.mode << "\",\n"
       << "  \"rep\": " << cfg.rep << ",\n"
       << "  \"delta_tombstones\": " << options.amtv_delta_tombstones << ",\n"
       << "  \"hard_layer_limit\": " << options.amtv_hard_layer_limit << ",\n"
       << "  \"fg_elapsed_sec\": " << fg_elapsed_sec << ",\n"
       << "  \"fg_iops\": " << fg_iops << ",\n"
       << "  \"phase_a_sec\": " << phase_elapsed_sec[0] << ",\n"
       << "  \"phase_b_sec\": " << phase_elapsed_sec[1] << ",\n"
       << "  \"phase_c_sec\": " << phase_elapsed_sec[2] << ",\n"
       << "  \"get_live_p50_us\": " << get_p50 << ",\n"
       << "  \"get_live_p95_us\": " << get_p95 << ",\n"
       << "  \"get_live_p99_us\": " << get_p99 << ",\n"
       << "  \"get_live_p999_us\": " << get_p999 << ",\n"
       << "  \"get_live_max_us\": " << get_max << ",\n"
       << "  \"get_post_fallback_p50_us\": " << post_fallback_p50 << ",\n"
       << "  \"get_post_fallback_p95_us\": " << post_fallback_p95 << ",\n"
       << "  \"get_post_fallback_p99_us\": " << post_fallback_p99 << ",\n"
       << "  \"get_post_fallback_p999_us\": " << post_fallback_p999 << ",\n"
       << "  \"get_post_fallback_max_us\": " << post_fallback_max << ",\n"
       << "  \"get_post_fallback_count\": " << post_fallback_count << ",\n"
       << "  \"put_p95_us\": " << put_p95 << ",\n"
       << "  \"put_p99_us\": " << put_p99 << ",\n"
       << "  \"put_p999_us\": " << put_p999 << ",\n"
       << "  \"put_max_us\": " << put_max << ",\n"
       << "  \"delete_range_p95_us\": " << del_p95 << ",\n"
       << "  \"delete_range_p99_us\": " << del_p99 << ",\n"
       << "  \"delete_range_p999_us\": " << del_p999 << ",\n"
       << "  \"delete_range_max_us\": " << del_max << ",\n"
       << "  \"fg_capacity_flushes\": " << fg_capacity_flushes << ",\n"
       << "  \"fg_threshold_flushes\": " << fg_threshold_flushes << ",\n"
       << "  \"fg_flush_bytes\": " << fg_flush_bytes << ",\n"
       << "  \"fg_compaction_read_bytes\": " << fg_compaction_read_bytes << ",\n"
       << "  \"fg_compaction_write_bytes\": " << fg_compaction_write_bytes << ",\n"
       << "  \"engine_output_wa\": " << engine_output_wa << ",\n"
       << "  \"engine_output_wa_description\": \"前台及观察窗口内 Flush/Compaction 引擎输出写放大为 0 (不含 WAL、AMTV 内存重建和设备层写放大)\",\n"
       << "  \"l0_files_peak\": " << l0_files_peak << ",\n"
       << "  \"pending_compaction_bytes\": " << pending_compaction_bytes << ",\n"
       << "  \"user_cpu_sec\": " << user_cpu_sec << ",\n"
       << "  \"sys_cpu_sec\": " << sys_cpu_sec << ",\n"
       << "  \"peak_rss_kb\": " << peak_rss_kb << ",\n"
       << "  \"drain_converged\": " << (drain_converged ? "true" : "false") << ",\n"
       << "  \"drain_elapsed_sec\": " << drain_elapsed_sec << ",\n"
       << "  \"phase_b_end_to_merge_stable_us\": " << phase_b_end_to_merge_stable_us << ",\n"
       << "  \"foreground_end_to_merge_stable_us\": " << foreground_end_to_merge_stable_us << ",\n"
       << "  \"last_merge_publish_timestamp_us\": " << last_merge_publish_timestamp_us << ",\n"
       << "  \"phase_b_end_task_state\": " << phase_b_end_task_state << ",\n"
       << "  \"phase_b_end_diagnostic_phase\": " << phase_b_end_diagnostic_phase << ",\n"
       << "  \"phase_b_end_merges_computed_after\": " << phase_b_end_merges_computed_after << ",\n"
       << "  \"phase_b_end_merges_published_after\": " << phase_b_end_merges_published_after << ",\n"
       << "  \"phase_b_end_merges_publishing_only\": " << phase_b_end_merges_publishing_only << ",\n"
       << "  \"peak_signed_backlog\": " << peak_signed_backlog << ",\n"
       << "  \"peak_backlog_excess\": " << peak_backlog_excess << ",\n"
       << "  \"peak_actual_sealed_runs\": " << peak_actual_sealed_runs << ",\n"
       << "  \"backlog_excess_max_duration_us\": " << backlog_excess_max_duration_us << ",\n"
       << "  \"peak_claimed_input_runs\": " << peak_claimed_input_runs << ",\n"
       << "  \"peak_scheduling_backlog\": " << peak_scheduling_backlog << ",\n"
       << "  \"db_sha256\": \"" << db_sha256 << "\",\n"
       << "  \"expected_model_sha\": \"" << expected_model_sha << "\",\n"
       << "  \"chunk_interval_min_us\": " << chunk_interval_min_us << ",\n"
       << "  \"chunk_interval_p50_us\": " << chunk_interval_p50_us << ",\n"
       << "  \"chunk_interval_p95_us\": " << chunk_interval_p95_us << ",\n"
       << "  \"chunk_interval_max_us\": " << chunk_interval_max_us << ",\n"
       << "  \"phase_b_del_range_arrival_rate_ops_per_sec\": " << phase_b_del_range_arrival_rate << ",\n"
       << "  \"phase_b_del_range_chunk_arrival_rate_chunks_per_sec\": " << phase_b_chunk_arrival_rate << ",\n"
       << "  \"phase_b_end_sealed_runs\": " << phase_b_end_sealed_runs << ",\n"
       << "  \"phase_b_end_open_delta_len\": " << phase_b_end_open_delta_len << ",\n"
       << "  \"phase_b_end_level_hist\": \"" << phase_b_end_level_hist << "\",\n"
       << "  \"phase_b_end_mergeable_pairs_exist\": " << (phase_b_end_mergeable_pairs_exist ? "true" : "false") << ",\n"
       << "  \"drained_sealed_runs\": " << drained_sealed_runs << ",\n"
       << "  \"drained_open_delta_len\": " << drained_open_delta_len << ",\n"
       << "  \"drained_level_hist\": \"" << drained_level_hist << "\",\n"
       << "  \"theoretical_distribution_matched\": " << (theoretical_distribution_matched ? "true" : "false") << ",\n"
       << "  \"max_single_merge_wall_time_us\": " << max_single_merge_wall_time_us << ",\n"
       << "  \"max_single_merge_cpu_time_us\": " << max_single_merge_cpu_time_us << ",\n"
       << "  \"max_single_merge_level\": " << max_single_merge_level << ",\n"
       << "  \"max_computed_merge_wall_us\": " << max_computed_merge_wall_us << ",\n"
       << "  \"max_computed_merge_cpu_us\": " << max_computed_merge_cpu_us << ",\n"
       << "  \"max_computed_merge_level\": " << max_computed_merge_level << ",\n"
       << "  \"max_published_merge_wall_us\": " << max_published_merge_wall_us << ",\n"
       << "  \"max_published_merge_cpu_us\": " << max_published_merge_cpu_us << ",\n"
       << "  \"max_published_merge_level\": " << max_published_merge_level << ",\n"
       << "  \"total_computed_merge_wall_us\": " << total_computed_merge_wall_us << ",\n"
       << "  \"total_computed_merge_cpu_us\": " << total_computed_merge_cpu_us << ",\n"
       << "  \"total_published_merge_wall_us\": " << total_published_merge_wall_us << ",\n"
       << "  \"total_published_merge_cpu_us\": " << total_published_merge_cpu_us << ",\n"
       << "  \"raw_entry_payload_bytes_peak\": " << raw_entry_payload_bytes_peak << ",\n"
       << "  \"raw_entry_capacity_proxy_bytes_peak\": " << raw_entry_capacity_proxy_bytes_peak << ",\n"
       << "  \"fragment_payload_proxy_bytes_peak\": " << fragment_payload_proxy_bytes_peak << ",\n"
       << "  \"inflight_payload_proxy_bytes_peak\": " << inflight_payload_proxy_bytes_peak << ",\n"
       << "  \"merge_count_per_level\": " << map_to_json(merge_count_per_lvl) << ",\n"
       << "  \"merge_input_tombstones_per_level\": " << map_to_json(merge_tombstones_per_lvl) << ",\n"
       << "  \"merge_wall_time_us_per_level\": " << map_to_json(merge_wall_us_per_lvl) << ",\n"
       << "  \"merge_cpu_time_us_per_level\": " << map_to_json(merge_cpu_us_per_lvl) << ",\n"
       << "  \"merge_queue_wait_us_per_level\": " << map_to_json(merge_wait_us_per_lvl) << ",\n"
       << "  \"backlog_regression_slope\": " << backlog_regression_slope << ",\n"
       << "  \"get_probe_avg_sealed_runs\": " << get_probe_avg_sealed_runs << ",\n"
       << "  \"get_probe_p50_sealed_runs\": " << get_probe_p50_sealed_runs << ",\n"
       << "  \"get_probe_p90_sealed_runs\": " << get_probe_p90_sealed_runs << ",\n"
       << "  \"get_probe_p99_sealed_runs\": " << get_probe_p99_sealed_runs << ",\n"
       << "  \"get_probe_max_sealed_runs\": " << get_probe_max_sealed_runs << ",\n"
       << "  \"get_probe_avg_open_delta\": " << get_probe_avg_open_delta << ",\n"
       << "  \"get_probe_max_open_delta\": " << get_probe_max_open_delta << ",\n"
       << "  \"amtv_merge_completed\": " << amtv_merge_completed << ",\n"
       << "  \"amtv_merge_requested\": " << amtv_merge_requested << ",\n"
       << "  \"amtv_merge_computed\": " << amtv_merge_computed << ",\n"
       << "  \"amtv_merge_published\": " << amtv_merge_published << ",\n"
       << "  \"amtv_merge_discarded\": " << amtv_merge_discarded << ",\n"
       << "  \"amtv_merge_input_runs\": " << amtv_merge_input_runs << ",\n"
       << "  \"amtv_merge_input_tombstones\": " << amtv_merge_input_tombstones << ",\n"
       << "  \"amtv_reconstruction_amplification\": " << (options.enable_amtv ? (double)amtv_merge_input_tombstones / 20000.0 : 0.0) << ",\n"
       << "  \"amtv_fallback_events\": " << amtv_fallback_events << ",\n"
       << "  \"amtv_fallback_gets\": " << amtv_fallback_gets << ",\n"
       << "  \"runs_at_fallback\": " << amtv_runs_at_fallback << ",\n"
       << "  \"tombstones_at_fallback\": " << amtv_tombstones_at_fallback << ",\n"
       << "  \"amtv_merge_wall_time_us\": " << amtv_merge_wall_time_us << ",\n"
       << "  \"amtv_merge_cpu_time_us\": " << amtv_merge_cpu_time_us << ",\n"
       << "  \"amtv_task_queue_wait_time_us\": " << amtv_task_queue_wait_time_us << ",\n"
       << "  \"amtv_raw_entries_struct_bytes_peak\": " << amtv_raw_entries_struct_bytes_peak << ",\n"
       << "  \"amtv_in_flight_merge_struct_bytes_peak\": " << amtv_in_flight_merge_struct_bytes_peak << ",\n"
       << "  \"amtv_open_delta_len\": " << amtv_open_delta_len << ",\n"
       << "  \"amtv_sealed_runs\": " << amtv_sealed_runs << ",\n"
       << "  \"amtv_level_hist\": \"" << amtv_level_hist << "\",\n"
       << "  \"audit_mat_count\": " << audit_mat_count << ",\n"
       << "  \"audit_mat_nanos\": " << audit_mat_nanos << ",\n"
       << "  \"audit_cache_inv_count\": " << audit_cache_inv_count << ",\n"
       << "  \"audit_lock_attempt_count\": " << audit_lock_attempt_count << ",\n"
       << "  \"audit_lock_contended_count\": " << audit_lock_contended_count << ",\n"
       << "  \"audit_lock_wait_nanos\": " << audit_lock_wait_nanos << ",\n"
       << "  \"amtv_write_state_lock_wait_nanos\": " << amtv_write_state_lock_wait_nanos << ",\n"
       << "  \"amtv_write_append_nanos\": " << amtv_write_append_nanos << ",\n"
       << "  \"amtv_write_snapshot_clone_nanos\": " << amtv_write_snapshot_clone_nanos << ",\n"
       << "  \"amtv_write_seal_build_nanos\": " << amtv_write_seal_build_nanos << ",\n"
       << "  \"amtv_write_publish_nanos\": " << amtv_write_publish_nanos << ",\n"
       << "  \"amtv_merge_discarded_wall_time_us\": " << amtv_merge_discarded_wall_time_us << ",\n"
       << "  \"amtv_merge_discarded_cpu_time_us\": " << amtv_merge_discarded_cpu_time_us << ",\n"
       << "  \"audit_phase_a\": {\n"
       << "    \"mat_count\": " << phase_audit_aggs[0].mat_count << ",\n"
       << "    \"mat_nanos\": " << phase_audit_aggs[0].mat_nanos << ",\n"
       << "    \"lock_attempt_count\": " << phase_audit_aggs[0].lock_attempt_count << ",\n"
       << "    \"lock_contended_count\": " << phase_audit_aggs[0].lock_contended_count << ",\n"
       << "    \"lock_wait_nanos\": " << phase_audit_aggs[0].lock_wait_nanos << ",\n"
       << "    \"cache_inv_count\": " << phase_audit_aggs[0].cache_inv_count << ",\n"
       << "    \"gets\": " << phase_audit_aggs[0].gets << ",\n"
       << "    \"puts\": " << phase_audit_aggs[0].puts << ",\n"
       << "    \"dels\": " << phase_audit_aggs[0].dels << ",\n"
       << "    \"avg_sealed_runs\": " << phase_audit_aggs[0].avg_sealed_runs << ",\n"
       << "    \"max_sealed_runs\": " << phase_audit_aggs[0].max_sealed_runs << ",\n"
       << "    \"avg_open_delta\": " << phase_audit_aggs[0].avg_open_delta << ",\n"
       << "    \"max_open_delta\": " << phase_audit_aggs[0].max_open_delta << "\n"
       << "  },\n"
       << "  \"audit_phase_b\": {\n"
       << "    \"mat_count\": " << phase_audit_aggs[1].mat_count << ",\n"
       << "    \"mat_nanos\": " << phase_audit_aggs[1].mat_nanos << ",\n"
       << "    \"lock_attempt_count\": " << phase_audit_aggs[1].lock_attempt_count << ",\n"
       << "    \"lock_contended_count\": " << phase_audit_aggs[1].lock_contended_count << ",\n"
       << "    \"lock_wait_nanos\": " << phase_audit_aggs[1].lock_wait_nanos << ",\n"
       << "    \"cache_inv_count\": " << phase_audit_aggs[1].cache_inv_count << ",\n"
       << "    \"gets\": " << phase_audit_aggs[1].gets << ",\n"
       << "    \"puts\": " << phase_audit_aggs[1].puts << ",\n"
       << "    \"dels\": " << phase_audit_aggs[1].dels << ",\n"
       << "    \"avg_sealed_runs\": " << phase_audit_aggs[1].avg_sealed_runs << ",\n"
       << "    \"max_sealed_runs\": " << phase_audit_aggs[1].max_sealed_runs << ",\n"
       << "    \"avg_open_delta\": " << phase_audit_aggs[1].avg_open_delta << ",\n"
       << "    \"max_open_delta\": " << phase_audit_aggs[1].max_open_delta << "\n"
       << "  },\n"
       << "  \"audit_phase_c\": {\n"
       << "    \"mat_count\": " << phase_audit_aggs[2].mat_count << ",\n"
       << "    \"mat_nanos\": " << phase_audit_aggs[2].mat_nanos << ",\n"
       << "    \"lock_attempt_count\": " << phase_audit_aggs[2].lock_attempt_count << ",\n"
       << "    \"lock_contended_count\": " << phase_audit_aggs[2].lock_contended_count << ",\n"
       << "    \"lock_wait_nanos\": " << phase_audit_aggs[2].lock_wait_nanos << ",\n"
       << "    \"cache_inv_count\": " << phase_audit_aggs[2].cache_inv_count << ",\n"
       << "    \"gets\": " << phase_audit_aggs[2].gets << ",\n"
       << "    \"puts\": " << phase_audit_aggs[2].puts << ",\n"
       << "    \"dels\": " << phase_audit_aggs[2].dels << ",\n"
       << "    \"avg_sealed_runs\": " << phase_audit_aggs[2].avg_sealed_runs << ",\n"
       << "    \"max_sealed_runs\": " << phase_audit_aggs[2].max_sealed_runs << ",\n"
       << "    \"avg_open_delta\": " << phase_audit_aggs[2].avg_open_delta << ",\n"
       << "    \"max_open_delta\": " << phase_audit_aggs[2].max_open_delta << "\n"
       << "  }\n"
       << "}\n";
    jf.close();

    db.reset();

    std::cout << "\n======================================================================\n";
    std::cout << "AMTV M2d Run " << cfg.exp_id << " COMPLETED SUCCESSFULLY!\n";
    std::cout << "  IOPS: " << fg_iops << ", GetLive P99: " << get_p99 << " us, Engine WA: " << engine_output_wa << "x\n";
    std::cout << "  Fallback Events: " << amtv_fallback_events << ", Fallback Gets: " << amtv_fallback_gets << "\n";
    std::cout << "  SEAL dt Min/P50/P95: " << chunk_interval_min_us << " / " << chunk_interval_p50_us << " / " << chunk_interval_p95_us << " us\n";
    std::cout << "  Phase B Chunk Arrival Rate: " << phase_b_chunk_arrival_rate << " chunks/sec\n";
    std::cout << "  Backlog Slope: " << backlog_regression_slope << " runs/sec\n";
    std::cout << "  Theoretical Distribution Matched: " << (theoretical_distribution_matched ? "YES" : "NO") << "\n";
    std::cout << "  Output JSON: " << out_json_path << "\n";
    std::cout << "  Timeline CSV: " << timeline_csv_path << "\n";
    std::cout << "======================================================================\n";

    return 0;
}
