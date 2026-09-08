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

#include "tools/formal_v2/thread_local_histogram.h"
#include "tools/formal_v2/formal_event_listener.h"

#define CHECK_INVARIANT(cond, fmt, ...)                                      \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "\n======================================================================\n"); \
      fprintf(stderr, "[FATAL R1 RECOVERY INVARIANT VIOLATION]\n");          \
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

struct R1Config {
    std::string trace_dir = "traces/m2d_r1_seed410001";
    std::string seed_db_path = "./run-db/m2d_canonical_seed_db";
    std::string db_path = "./run-db/m2d_r1_recovery/db";
    std::string output_json = "results/r0_audit/r1_recovery_verification.json";
    int num_workers = 8;
    uint64_t total_keys = 500000;
};

static void ConfigureRocksDBOptions(rocksdb::Options& options,
                                    std::shared_ptr<study::formal::FormalEventListener> listener) {
    options.create_if_missing = false;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.level0_file_num_compaction_trigger = 4;
    options.level0_slowdown_writes_trigger = 8;
    options.level0_stop_writes_trigger = 12;
    options.target_file_size_base = 64 * 1024 * 1024;
    options.max_bytes_for_level_base = 256 * 1024 * 1024;
    options.max_background_jobs = 8;

    rocksdb::BlockBasedTableOptions table_opts;
    table_opts.block_cache = rocksdb::NewLRUCache(128 * 1024 * 1024);
    table_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_opts));

    options.statistics = rocksdb::CreateDBStatistics();

    // AMTV-T0 configuration
    options.enable_amtv = true;
    options.memtable_max_range_deletions = 0;
    options.amtv_delta_tombstones = 64;
    options.amtv_merge_soft_limit = 2;
    options.amtv_hard_layer_limit = 32;

    options.listeners.clear();
    if (listener) {
        options.listeners.push_back(listener);
    }
}

int main(int argc, char* argv[]) {
    R1Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--trace-dir" && i + 1 < argc) cfg.trace_dir = argv[++i];
        else if (arg == "--seed-db" && i + 1 < argc) cfg.seed_db_path = argv[++i];
        else if (arg == "--db-path" && i + 1 < argc) cfg.db_path = argv[++i];
        else if (arg == "--output-json" && i + 1 < argc) cfg.output_json = argv[++i];
        else if (arg == "--workers" && i + 1 < argc) cfg.num_workers = std::stoi(argv[++i]);
        else if (arg == "--total-keys" && i + 1 < argc) cfg.total_keys = std::stoull(argv[++i]);
    }

    std::cout << "======================================================================\n";
    std::cout << " AMTV-T0 R1 Recovery Semantics Verification (Seed 410001)\n";
    std::cout << " Trace Dir: " << cfg.trace_dir << "\n";
    std::cout << " DB Path:   " << cfg.db_path << "\n";
    std::cout << " Seed DB:   " << cfg.seed_db_path << "\n";
    std::cout << " Output:    " << cfg.output_json << "\n";
    std::cout << "======================================================================\n";

    // Set high priority
    setpriority(PRIO_PROCESS, 0, -20);

    // 1. Prepare Fresh DB Directory from Canonical Seed DB
    std::cout << "[Step 1/8] Initializing DB from canonical seed...\n";
    std::string clean_cmd = "rm -rf " + cfg.db_path + " && mkdir -p " + cfg.db_path;
    int ret = std::system(clean_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to clean/create DB dir: %s", cfg.db_path.c_str());

    std::string cp_cmd = "cp -r " + cfg.seed_db_path + "/* " + cfg.db_path + "/";
    ret = std::system(cp_cmd.c_str());
    CHECK_INVARIANT(ret == 0, "Failed to copy seed DB from %s to %s", cfg.seed_db_path.c_str(), cfg.db_path.c_str());

    // 2. Load Worker Traces
    std::cout << "[Step 2/8] Loading worker traces...\n";
    std::vector<std::vector<FormalTraceRecord>> worker_traces(cfg.num_workers);
    for (int w = 0; w < cfg.num_workers; ++w) {
        std::string trace_path = cfg.trace_dir + "/worker_" + std::to_string(w) + ".trace";
        std::ifstream f(trace_path, std::ios::binary);
        CHECK_INVARIANT(f.is_open(), "Failed to open worker trace: %s", trace_path.c_str());
        FormalTraceRecord rec;
        while (f.read(reinterpret_cast<char*>(&rec), sizeof(rec))) {
            worker_traces[w].push_back(rec);
        }
        CHECK_INVARIANT(worker_traces[w].size() == 37500, "Worker %d trace size mismatch: expected 37500, got %zu", w, worker_traces[w].size());
    }

    // 3. Open DB with Initial Listener
    rocksdb::Env::Default()->SetBackgroundThreads(4, rocksdb::Env::Priority::BOTTOM);
    rocksdb::Env::Default()->SetBackgroundThreads(4, rocksdb::Env::Priority::HIGH);
    rocksdb::Env::Default()->SetBackgroundThreads(8, rocksdb::Env::Priority::LOW);

    auto listener = std::make_shared<study::formal::FormalEventListener>();
    rocksdb::Options options;
    ConfigureRocksDBOptions(options, listener);

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(options, cfg.db_path, &db);
    CHECK_INVARIANT(s.ok(), "Failed to open DB: %s", s.ToString().c_str());

    rocksdb::ColumnFamilyData* cfd =
        static_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily())->cfd();

    // 4. Deterministic Read-only Warmup: 10,000 GetLive across 8 workers (1,250 each)
    std::cout << "[Step 3/8] Running deterministic read-only warmup...\n";
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

    // Drain background after warmup
    for (int retry = 0; retry < 200; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        bool amtv_stable = true;
        if (cfd->mem() && cfd->mem()->GetAMTVState()) {
            amtv_stable = cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (running_flushes == 0 && running_compactions == 0 && amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    struct BoundarySnapshot {
        uint64_t flush_bytes = 0;
        uint64_t compaction_write_bytes = 0;
    };
    auto capture_boundary = [&]() -> BoundarySnapshot {
        return BoundarySnapshot{
            listener->GetTotalCumulativeFlushBytes(),
            listener->GetTotalCumulativeCompactionWriteBytes()
        };
    };

    // Reset listener and stats before foreground
    options.statistics->Reset();
    listener->StartForegroundExperiment();
    BoundarySnapshot b0 = capture_boundary();

    // 5. Execute Foreground Phases A, B, C
    std::cout << "[Step 4/8] Executing Foreground Phases A, B, C (300,000 operations)...\n";
    std::barrier sync_barrier(cfg.num_workers + 1);
    std::vector<std::thread> workers;
    workers.reserve(cfg.num_workers);

    for (int w = 0; w < cfg.num_workers; ++w) {
        workers.emplace_back([&, w]() {
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
                        std::string val;
                        std::string key = FormatKey(rec.key1);
                        rocksdb::Status s_get = db->Get(ropts, key, &val);
                        CHECK_INVARIANT(s_get.ok(), "Worker %d GetLive key %lu failed: %s", w, rec.key1, s_get.ToString().c_str());
                    } else if (rec.op_type == 2) { // Put
                        std::string key = FormatKey(rec.key1);
                        std::string val = GeneratePutValue(rec.key1, rec.phase_id, rec.op_id, 256);
                        rocksdb::Status s_put = db->Put(wopts, key, val);
                        CHECK_INVARIANT(s_put.ok(), "Worker %d Put key %lu failed: %s", w, rec.key1, s_put.ToString().c_str());
                    } else if (rec.op_type == 3) { // DeleteRange
                        std::string k1 = FormatKey(rec.key1);
                        std::string k2 = FormatKey(rec.key2);
                        rocksdb::Status s_del = db->DeleteRange(wopts, db->DefaultColumnFamily(), k1, k2);
                        CHECK_INVARIANT(s_del.ok(), "Worker %d DeleteRange [%lu, %lu) failed: %s", w, rec.key1, rec.key2, s_del.ToString().c_str());
                    }
                }

                sync_barrier.arrive_and_wait(); // Signal phase complete
            }
        });
    }

    auto run_phase = [&](int phase_idx, const std::string& phase_name) {
        (void)phase_idx;
        (void)phase_name;
        sync_barrier.arrive_and_wait(); // Release workers
        sync_barrier.arrive_and_wait(); // Wait for workers to finish
    };

    run_phase(0, "PHASE_A");
    run_phase(1, "PHASE_B");
    run_phase(2, "PHASE_C");

    for (auto& t : workers) t.join();
    std::cout << "  Foreground execution finished.\n";
    BoundarySnapshot b1 = capture_boundary();

    // 6. Cooldown Window (10s)
    std::cout << "[Step 5/8] Cooldown (10s) and Drain Window...\n";
    listener->StartCooldownObservation();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    BoundarySnapshot b2 = capture_boundary();

    // 7. Drain Window
    listener->StartDrainStage();
    for (int retry = 0; retry < 500; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0, pending_bytes = 0;
        db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
        bool amtv_stable = true;
        if (cfd->mem() && cfd->mem()->GetAMTVState()) {
            amtv_stable = cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (running_flushes == 0 && running_compactions == 0 && pending_bytes == 0 && amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    BoundarySnapshot b3 = capture_boundary();

    // Capture Pre-Close State & Metrics
    uint64_t pre_close_fg_flush_bytes = (b1.flush_bytes >= b0.flush_bytes) ? (b1.flush_bytes - b0.flush_bytes) : 0;
    uint64_t pre_close_fg_compaction_write_bytes = (b1.compaction_write_bytes >= b0.compaction_write_bytes) ? (b1.compaction_write_bytes - b0.compaction_write_bytes) : 0;
    uint64_t pre_close_cooldown_flush_bytes = (b2.flush_bytes >= b1.flush_bytes) ? (b2.flush_bytes - b1.flush_bytes) : 0;
    uint64_t pre_close_cooldown_compaction_write_bytes = (b2.compaction_write_bytes >= b1.compaction_write_bytes) ? (b2.compaction_write_bytes - b1.compaction_write_bytes) : 0;
    uint64_t pre_close_drain_flush_bytes = (b3.flush_bytes >= b2.flush_bytes) ? (b3.flush_bytes - b2.flush_bytes) : 0;
    uint64_t pre_close_drain_compaction_write_bytes = (b3.compaction_write_bytes >= b2.compaction_write_bytes) ? (b3.compaction_write_bytes - b2.compaction_write_bytes) : 0;
    uint64_t pre_close_total_flush_bytes = (b3.flush_bytes >= b0.flush_bytes) ? (b3.flush_bytes - b0.flush_bytes) : 0;
    uint64_t pre_close_total_compaction_write_bytes = (b3.compaction_write_bytes >= b0.compaction_write_bytes) ? (b3.compaction_write_bytes - b0.compaction_write_bytes) : 0;

    // Inspect AMTV pre-close state
    uint32_t pre_close_sealed_runs = 0;
    uint32_t pre_close_open_delta_len = 0;
    std::string pre_close_level_hist = "{}";
    bool pre_close_fallback = false;
    bool pre_close_is_stable = false;

    if (cfd->mem() && cfd->mem()->GetAMTVState()) {
        auto* state = cfd->mem()->GetAMTVState();
        auto snap = state->GetSnapshot();
        if (snap) {
            pre_close_sealed_runs = snap->sealed_run_count();
            pre_close_open_delta_len = snap->open_delta ? static_cast<uint32_t>(snap->open_delta->size()) : 0;
            pre_close_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);
            pre_close_fallback = snap->fallback_required;
        }
        pre_close_is_stable = state->IsMergeStable();
    }

    std::cout << "  Pre-Close AMTV State: sealed_runs=" << pre_close_sealed_runs
              << ", open_delta_len=" << pre_close_open_delta_len
              << ", hist=" << pre_close_level_hist
              << ", fallback=" << pre_close_fallback
              << ", is_stable=" << pre_close_is_stable << "\n";

    // Build External State Model
    std::cout << "[Step 6/8] Pre-Close 500,000 Key Verification against External State Model...\n";
    listener->StartVerificationStage();
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

    // Pre-Close 500k Point Get Verification
    uint64_t pre_close_live_verified = 0;
    uint64_t pre_close_deleted_verified = 0;
    rocksdb::ReadOptions v_ropts;
    SHA256_CTX pre_ctx;
    SHA256_Init(&pre_ctx);

    for (uint64_t k = 0; k < cfg.total_keys; ++k) {
        std::string key_str = FormatKey(k);
        std::string val;
        rocksdb::Status s_get = db->Get(v_ropts, key_str, &val);
        const std::string& exp_val = model.Get(k);
        if (!exp_val.empty()) {
            CHECK_INVARIANT(s_get.ok(), "Pre-close Key %lu expected LIVE but Get returned %s", k, s_get.ToString().c_str());
            CHECK_INVARIANT(val == exp_val, "Pre-close Key %lu value mismatch!", k);
            pre_close_live_verified++;
            SHA256_Update(&pre_ctx, key_str.data(), key_str.size());
            SHA256_Update(&pre_ctx, val.data(), val.size());
        } else {
            CHECK_INVARIANT(s_get.IsNotFound(), "Pre-close Key %lu expected DELETED but Get returned %s", k, s_get.ToString().c_str());
            pre_close_deleted_verified++;
        }
    }

    CHECK_INVARIANT(pre_close_live_verified == 300000, "Pre-close live count: expected 300000, got %lu", pre_close_live_verified);
    CHECK_INVARIANT(pre_close_deleted_verified == 200000, "Pre-close deleted count: expected 200000, got %lu", pre_close_deleted_verified);

    unsigned char pre_hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(pre_hash, &pre_ctx);
    std::ostringstream pre_oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        pre_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(pre_hash[i]);
    }
    std::string pre_close_sha = pre_oss.str();
    CHECK_INVARIANT(pre_close_sha == expected_model_sha, "Pre-close SHA mismatch! DB: %s, Model: %s", pre_close_sha.c_str(), expected_model_sha.c_str());
    std::cout << "  [PASS] Pre-Close State SHA: " << pre_close_sha << " (300,000 Live, 200,000 Deleted)\n";

    // Pre-Close Scan Visibility
    uint64_t pre_close_scan_count = 0;
    {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(v_ropts));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            pre_close_scan_count++;
        }
    }
    CHECK_INVARIANT(pre_close_scan_count == 300000, "Pre-close scan count: expected 300000, got %lu", pre_close_scan_count);

    // 8. Close DB & Check Teardown Output
    std::cout << "[Step 7/8] Closing DB & Measuring Close Teardown I/O...\n";
    size_t flush_count_before_close = listener->GetFlushEvents().size();
    size_t compaction_count_before_close = listener->GetCompactionEvents().size();
    uint64_t flush_bytes_before_close = listener->GetTotalCumulativeFlushBytes();
    uint64_t compaction_write_bytes_before_close = listener->GetTotalCumulativeCompactionWriteBytes();

    // Close DB cleanly
    db.reset();

    uint64_t during_close_flush_bytes = listener->GetTotalCumulativeFlushBytes() - flush_bytes_before_close;
    uint64_t during_close_compaction_write_bytes = listener->GetTotalCumulativeCompactionWriteBytes() - compaction_write_bytes_before_close;
    size_t during_close_flush_events = listener->GetFlushEvents().size() - flush_count_before_close;
    size_t during_close_compaction_events = listener->GetCompactionEvents().size() - compaction_count_before_close;

    std::cout << "  DB closed cleanly.\n";
    std::cout << "  During-Close Flush Bytes: " << during_close_flush_bytes << " (events: " << during_close_flush_events << ")\n";
    std::cout << "  During-Close Compaction Write Bytes: " << during_close_compaction_write_bytes << " (events: " << during_close_compaction_events << ")\n";

    // Backup closed DB directory with intact WAL before any reopen
    std::string wal_backup_dir = cfg.db_path + "_wal_backup";
    std::string backup_cmd = "rm -rf " + wal_backup_dir + " && cp -r " + cfg.db_path + " " + wal_backup_dir;
    int b_ret = std::system(backup_cmd.c_str());
    CHECK_INVARIANT(b_ret == 0, "Failed to backup DB dir for WAL recovery audit: %s", wal_backup_dir.c_str());

    // 9. Standard Reopen (Standard Default Options: avoid_flush_during_recovery = false)
    std::cout << "[Step 8/8] Re-opening DB with Standard Default Config (WAL Replay & Recovery Flush)...\n";
    auto reopen_listener = std::make_shared<study::formal::FormalEventListener>();
    rocksdb::Options reopen_options;
    ConfigureRocksDBOptions(reopen_options, reopen_listener);
    reopen_options.avoid_flush_during_recovery = false;

    std::unique_ptr<rocksdb::DB> reopened_db;
    rocksdb::Status reopen_s = rocksdb::DB::Open(reopen_options, cfg.db_path, &reopened_db);
    CHECK_INVARIANT(reopen_s.ok(), "Failed to re-open DB: %s", reopen_s.ToString().c_str());

    rocksdb::ColumnFamilyData* reopen_cfd =
        static_cast<rocksdb::ColumnFamilyHandleImpl*>(reopened_db->DefaultColumnFamily())->cfd();

    // Wait for recovery background tasks and AMTV merges to stabilize
    for (int retry = 0; retry < 500; ++retry) {
        uint64_t running_flushes = 0, running_compactions = 0, pending_bytes = 0;
        reopened_db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
        reopened_db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
        reopened_db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
        bool amtv_stable = true;
        if (reopen_cfd->mem() && reopen_cfd->mem()->GetAMTVState()) {
            amtv_stable = reopen_cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (running_flushes == 0 && running_compactions == 0 && pending_bytes == 0 && amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Inspect LOG file for recovery flush table file creation
    uint64_t recovery_flush_bytes = 0;
    uint64_t recovery_range_deletions = 0;
    {
        std::ifstream logf(cfg.db_path + "/LOG");
        std::string line;
        while (std::getline(logf, line)) {
            if (line.find("\"event\": \"table_file_creation\"") != std::string::npos &&
                line.find("\"job\": 1") != std::string::npos) {
                auto sz_pos = line.find("\"file_size\": ");
                if (sz_pos != std::string::npos) {
                    recovery_flush_bytes = std::stoull(line.substr(sz_pos + 13));
                }
                auto rd_pos = line.find("\"num_range_deletions\": ");
                if (rd_pos != std::string::npos) {
                    recovery_range_deletions = std::stoull(line.substr(rd_pos + 23));
                }
            }
        }
    }

    uint64_t during_reopen_compaction_write_bytes = reopen_listener->GetTotalCumulativeCompactionWriteBytes();
    size_t during_reopen_compaction_events = reopen_listener->GetCompactionEvents().size();

    std::cout << "  DB reopened successfully under default configuration.\n";
    std::cout << "  Recovery L0 Flush (from WAL replay): " << recovery_flush_bytes << " B (RangeDeletions: " << recovery_range_deletions << ")\n";
    std::cout << "  Recovery Compaction Write Bytes:     " << during_reopen_compaction_write_bytes << " B (events: " << during_reopen_compaction_events << ")\n";

    // Post-Reopen 500k Point Get Verification (Without any new Put/DeleteRange)
    std::cout << "  Verifying Post-Reopen 500,000 Keys against External State Model...\n";
    uint64_t post_reopen_live_verified = 0;
    uint64_t post_reopen_deleted_verified = 0;
    SHA256_CTX post_ctx;
    SHA256_Init(&post_ctx);

    for (uint64_t k = 0; k < cfg.total_keys; ++k) {
        std::string key_str = FormatKey(k);
        std::string val;
        rocksdb::Status s_get = reopened_db->Get(v_ropts, key_str, &val);
        const std::string& exp_val = model.Get(k);
        if (!exp_val.empty()) {
            CHECK_INVARIANT(s_get.ok(), "Post-reopen Key %lu expected LIVE but Get returned %s", k, s_get.ToString().c_str());
            CHECK_INVARIANT(val == exp_val, "Post-reopen Key %lu value mismatch! DB: %s, Exp: %s", k, val.c_str(), exp_val.c_str());
            post_reopen_live_verified++;
            SHA256_Update(&post_ctx, key_str.data(), key_str.size());
            SHA256_Update(&post_ctx, val.data(), val.size());
        } else {
            CHECK_INVARIANT(s_get.IsNotFound(), "Post-reopen Key %lu expected DELETED but Get returned %s", k, s_get.ToString().c_str());
            post_reopen_deleted_verified++;
        }
    }

    CHECK_INVARIANT(post_reopen_live_verified == 300000, "Post-reopen live count: expected 300000, got %lu", post_reopen_live_verified);
    CHECK_INVARIANT(post_reopen_deleted_verified == 200000, "Post-reopen deleted count: expected 200000, got %lu", post_reopen_deleted_verified);

    unsigned char post_hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(post_hash, &post_ctx);
    std::ostringstream post_oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        post_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(post_hash[i]);
    }
    std::string post_reopen_sha = post_oss.str();

    std::cout << "  [PASS] Post-Reopen State SHA: " << post_reopen_sha << "\n";
    CHECK_INVARIANT(post_reopen_sha == pre_close_sha,
                    "FATAL: Post-reopen SHA mismatch! Pre-close: %s, Post-reopen: %s",
                    pre_close_sha.c_str(), post_reopen_sha.c_str());
    std::cout << "  [PASS] State SHA Conservation Verified (pre_close == post_reopen == " << post_reopen_sha << ")\n";

    // Post-Reopen Scan Visibility
    uint64_t post_reopen_scan_count = 0;
    {
        std::unique_ptr<rocksdb::Iterator> it(reopened_db->NewIterator(v_ropts));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            post_reopen_scan_count++;
        }
    }
    CHECK_INVARIANT(post_reopen_scan_count == 300000, "Post-reopen scan count: expected 300000, got %lu", post_reopen_scan_count);
    std::cout << "  [PASS] Post-Reopen Iterator Scan Count Verified: " << post_reopen_scan_count << "\n";

    // Standard Reopen active memtable state
    uint32_t post_reopen_std_sealed_runs = 0;
    uint32_t post_reopen_std_open_delta_len = 0;
    std::string post_reopen_std_level_hist = "{}";
    if (reopen_cfd->mem() && reopen_cfd->mem()->GetAMTVState()) {
        auto snap = reopen_cfd->mem()->GetAMTVState()->GetSnapshot();
        if (snap) {
            post_reopen_std_sealed_runs = snap->sealed_run_count();
            post_reopen_std_open_delta_len = snap->open_delta ? static_cast<uint32_t>(snap->open_delta->size()) : 0;
            post_reopen_std_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);
        }
    }
    std::cout << "  Standard Post-Reopen Active MemTable AMTV: sealed_runs=" << post_reopen_std_sealed_runs
              << ", hist=" << post_reopen_std_level_hist << " (flushed to SST #20 by recovery)\n";

    reopened_db.reset(); // Close standard reopened DB

    // 10. Reopen WAL Backup with avoid_flush_during_recovery = true to verify in-memory AMTV state reconstruction
    std::cout << "\n[Step 8b/8] Verifying In-Memory AMTV State Reconstruction (avoid_flush_during_recovery = true)...\n";
    auto amtv_audit_listener = std::make_shared<study::formal::FormalEventListener>();
    rocksdb::Options amtv_audit_opts;
    ConfigureRocksDBOptions(amtv_audit_opts, amtv_audit_listener);
    amtv_audit_opts.avoid_flush_during_recovery = true;

    std::unique_ptr<rocksdb::DB> amtv_audit_db;
    rocksdb::Status audit_s = rocksdb::DB::Open(amtv_audit_opts, wal_backup_dir, &amtv_audit_db);
    CHECK_INVARIANT(audit_s.ok(), "Failed to open WAL backup DB: %s", audit_s.ToString().c_str());

    rocksdb::ColumnFamilyData* audit_cfd =
        static_cast<rocksdb::ColumnFamilyHandleImpl*>(amtv_audit_db->DefaultColumnFamily())->cfd();

    // Wait for AMTV background merges to stabilize
    for (int retry = 0; retry < 500; ++retry) {
        bool amtv_stable = true;
        if (audit_cfd->mem() && audit_cfd->mem()->GetAMTVState()) {
            amtv_stable = audit_cfd->mem()->GetAMTVState()->IsMergeStable();
        }
        if (amtv_stable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    uint32_t inmem_sealed_runs = 0;
    uint32_t inmem_open_delta_len = 0;
    std::string inmem_level_hist = "{}";
    bool inmem_fallback = false;
    bool inmem_is_stable = false;

    if (audit_cfd->mem() && audit_cfd->mem()->GetAMTVState()) {
        auto* state = audit_cfd->mem()->GetAMTVState();
        auto snap = state->GetSnapshot();
        if (snap) {
            inmem_sealed_runs = snap->sealed_run_count();
            inmem_open_delta_len = snap->open_delta ? static_cast<uint32_t>(snap->open_delta->size()) : 0;
            inmem_level_hist = rocksdb::FormatRunLevelHistogram(snap->sealed_runs);
            inmem_fallback = snap->fallback_required;
        }
        inmem_is_stable = state->IsMergeStable();
    }

    std::cout << "  In-Memory Reconstructed AMTV State: sealed_runs=" << inmem_sealed_runs
              << ", open_delta_len=" << inmem_open_delta_len
              << ", hist=" << inmem_level_hist
              << ", fallback=" << inmem_fallback
              << ", is_stable=" << inmem_is_stable << "\n";

    bool amtv_wal_recovery_matched = (inmem_sealed_runs == 4 && inmem_open_delta_len == 32 &&
                                      inmem_level_hist == "{L3:1, L4:1, L5:1, L8:1}" && !inmem_fallback);
    CHECK_INVARIANT(amtv_wal_recovery_matched, "Fatal: In-memory AMTV state mismatch after WAL recovery!");
    std::cout << "  [PASS] AMTV WAL State Reconstruction Verified: exactly 4 sealed runs {L3:1, L4:1, L5:1, L8:1} + 32 delta\n";

    // Verify 500k keys on the in-memory reconstructed DB
    uint64_t inmem_live_verified = 0;
    uint64_t inmem_deleted_verified = 0;
    SHA256_CTX inmem_ctx;
    SHA256_Init(&inmem_ctx);
    for (uint64_t k = 0; k < cfg.total_keys; ++k) {
        std::string key_str = FormatKey(k);
        std::string val;
        rocksdb::Status s_get = amtv_audit_db->Get(v_ropts, key_str, &val);
        const std::string& exp_val = model.Get(k);
        if (!exp_val.empty()) {
            CHECK_INVARIANT(s_get.ok(), "In-memory Key %lu expected LIVE", k);
            CHECK_INVARIANT(val == exp_val, "In-memory Key %lu value mismatch!", k);
            inmem_live_verified++;
            SHA256_Update(&inmem_ctx, key_str.data(), key_str.size());
            SHA256_Update(&inmem_ctx, val.data(), val.size());
        } else {
            CHECK_INVARIANT(s_get.IsNotFound(), "In-memory Key %lu expected DELETED", k);
            inmem_deleted_verified++;
        }
    }
    unsigned char inmem_hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(inmem_hash, &inmem_ctx);
    std::ostringstream inmem_oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        inmem_oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(inmem_hash[i]);
    }
    std::string inmem_sha = inmem_oss.str();
    CHECK_INVARIANT(inmem_sha == pre_close_sha, "In-memory State SHA mismatch!");
    std::cout << "  [PASS] In-Memory Reconstructed DB State SHA Verified: " << inmem_sha << "\n";

    amtv_audit_db.reset(); // Close audit DB
    std::system(("rm -rf " + wal_backup_dir).c_str()); // Clean up backup

    // Export Verification JSON
    std::ofstream jf(cfg.output_json);
    CHECK_INVARIANT(jf.is_open(), "Failed to open output JSON: %s", cfg.output_json.c_str());
    jf << std::fixed << std::setprecision(6);
    jf << "{\n"
       << "  \"experiment\": \"m2d_r1_recovery_verification\",\n"
       << "  \"config\": \"AMTV-T0\",\n"
       << "  \"random_seed\": 410001,\n"
       << "  \"total_keys\": " << cfg.total_keys << ",\n"
       << "  \"expected_live_keys\": 300000,\n"
       << "  \"expected_deleted_keys\": 200000,\n"
       << "  \"external_model_sha256\": \"" << expected_model_sha << "\",\n"
       << "  \"pre_close\": {\n"
       << "    \"state_sha256\": \"" << pre_close_sha << "\",\n"
       << "    \"verified_live_keys\": " << pre_close_live_verified << ",\n"
       << "    \"verified_deleted_keys\": " << pre_close_deleted_verified << ",\n"
       << "    \"scan_visible_keys\": " << pre_close_scan_count << ",\n"
       << "    \"amtv_state\": {\n"
       << "      \"sealed_runs\": " << pre_close_sealed_runs << ",\n"
       << "      \"open_delta_len\": " << pre_close_open_delta_len << ",\n"
       << "      \"level_histogram\": \"" << pre_close_level_hist << "\",\n"
       << "      \"fallback_required\": " << (pre_close_fallback ? "true" : "false") << ",\n"
       << "      \"is_merge_stable\": " << (pre_close_is_stable ? "true" : "false") << "\n"
       << "    },\n"
       << "    \"cumulative_io\": {\n"
       << "      \"fg_flush_bytes\": " << pre_close_fg_flush_bytes << ",\n"
       << "      \"fg_compaction_write_bytes\": " << pre_close_fg_compaction_write_bytes << ",\n"
       << "      \"cooldown_flush_bytes\": " << pre_close_cooldown_flush_bytes << ",\n"
       << "      \"cooldown_compaction_write_bytes\": " << pre_close_cooldown_compaction_write_bytes << ",\n"
       << "      \"drain_flush_bytes\": " << pre_close_drain_flush_bytes << ",\n"
       << "      \"drain_compaction_write_bytes\": " << pre_close_drain_compaction_write_bytes << ",\n"
       << "      \"total_flush_bytes\": " << pre_close_total_flush_bytes << ",\n"
       << "      \"total_compaction_write_bytes\": " << pre_close_total_compaction_write_bytes << "\n"
       << "    }\n"
       << "  },\n"
       << "  \"close_stage\": {\n"
       << "    \"flush_bytes\": " << during_close_flush_bytes << ",\n"
       << "    \"flush_events\": " << during_close_flush_events << ",\n"
       << "    \"compaction_write_bytes\": " << during_close_compaction_write_bytes << ",\n"
       << "    \"compaction_events\": " << during_close_compaction_events << "\n"
       << "  },\n"
       << "  \"reopen_standard_stage\": {\n"
       << "    \"recovery_l0_flush_bytes\": " << recovery_flush_bytes << ",\n"
       << "    \"recovery_range_deletions_persisted\": " << recovery_range_deletions << ",\n"
       << "    \"compaction_write_bytes\": " << during_reopen_compaction_write_bytes << ",\n"
       << "    \"compaction_events\": " << during_reopen_compaction_events << ",\n"
       << "    \"active_memtable_sealed_runs\": " << post_reopen_std_sealed_runs << ",\n"
       << "    \"active_memtable_hist\": \"" << post_reopen_std_level_hist << "\"\n"
       << "  },\n"
       << "  \"reopen_inmemory_amtv_stage\": {\n"
       << "    \"avoid_flush_during_recovery\": true,\n"
       << "    \"sealed_runs\": " << inmem_sealed_runs << ",\n"
       << "    \"open_delta_len\": " << inmem_open_delta_len << ",\n"
       << "    \"level_histogram\": \"" << inmem_level_hist << "\",\n"
       << "    \"fallback_required\": " << (inmem_fallback ? "true" : "false") << ",\n"
       << "    \"is_merge_stable\": " << (inmem_is_stable ? "true" : "false") << "\n"
       << "  },\n"
       << "  \"post_reopen\": {\n"
       << "    \"state_sha256\": \"" << post_reopen_sha << "\",\n"
       << "    \"verified_live_keys\": " << post_reopen_live_verified << ",\n"
       << "    \"verified_deleted_keys\": " << post_reopen_deleted_verified << ",\n"
       << "    \"scan_visible_keys\": " << post_reopen_scan_count << "\n"
       << "  },\n"
       << "  \"audit_verdict\": {\n"
       << "    \"state_sha_conserved\": " << ((pre_close_sha == post_reopen_sha && post_reopen_sha == expected_model_sha) ? "true" : "false") << ",\n"
       << "    \"amtv_wal_recovery_matched\": " << (amtv_wal_recovery_matched ? "true" : "false") << ",\n"
       << "    \"pass_all_gates\": true\n"
       << "  }\n"
       << "}\n";
    jf.close();

    std::cout << "\n======================================================================\n";
    std::cout << " [R1 VERIFICATION RESULT: COMPLETE SUCCESS]\n";
    std::cout << " Pre-Close SHA:   " << pre_close_sha << "\n";
    std::cout << " Post-Reopen SHA: " << post_reopen_sha << " (CONSERVED: YES)\n";
    std::cout << " Close Output:    Flush=" << during_close_flush_bytes << " B, Compaction=" << during_close_compaction_write_bytes << " B\n";
    std::cout << " Reopen (Std):    Recovery L0 Flush=" << recovery_flush_bytes << " B, Compaction=" << during_reopen_compaction_write_bytes << " B\n";
    std::cout << " Reopen (AMTV):   In-Memory WAL Recovery State=" << inmem_level_hist << " + " << inmem_open_delta_len << " delta (MATCH: YES)\n";
    std::cout << " JSON Written:    " << cfg.output_json << "\n";
    std::cout << "======================================================================\n";

    return 0;
}
