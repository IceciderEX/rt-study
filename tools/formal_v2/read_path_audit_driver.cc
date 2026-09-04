//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iomanip>
#include <cassert>
#include <sched.h>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "db/read_path_audit.h"

#pragma pack(push, 1)
struct FormalTraceRecord {
    uint8_t  phase_id;      // 0=A, 1=B, 2=C
    uint8_t  op_type;       // 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=No-op
    uint8_t  scan_mode;     // 0=Range, 1=Limit
    uint8_t  flags;         // bit 0: affected/intersect, bit 1: inject, bit 2: postburst
    uint32_t op_id;
    uint64_t key1;
    uint64_t key2;
};
#pragma pack(pop)

static_assert(sizeof(FormalTraceRecord) == 24, "FormalTraceRecord must be exactly 24 bytes");

static std::string FormatKey(uint64_t k) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lu", k);
    return std::string(buf);
}

struct WriteSideSnapshot {
    uint64_t delete_range_count = 0;
    uint64_t cache_invalidation_count = 0;
};

struct AuditRunConfig {
    std::string condition;       // CLEAN, T0-MEM, POSTFLUSH-SST
    std::string workload;        // get_live, scan_intersect
    std::string db_path;
    std::string trace_dir;
    std::string csv_path;
    int rep = 1;
    uint64_t total_keys = 500000;
    size_t value_size = 256;
    uint64_t write_buffer_size = 64 * 1024 * 1024;
    size_t warm_ops = 5000;
};

class AuditHarness {
public:
    explicit AuditHarness(const AuditRunConfig& cfg) : cfg_(cfg) {}
    ~AuditHarness() = default;

    bool Run() {
        LogEnvironmentMetadata();
        if (!OpenDB()) return false;
        if (!PreloadData()) return false;

        std::vector<FormalTraceRecord> filtered_records;
        if (!LoadAndFilterTrace(filtered_records)) return false;

        if (cfg_.condition == "CLEAN") {
            return RunClean(filtered_records);
        } else if (cfg_.condition == "T0-MEM") {
            return RunT0Mem(filtered_records);
        } else if (cfg_.condition == "POSTFLUSH-SST") {
            return RunPostFlushSST(filtered_records);
        } else {
            std::cerr << "[AUDIT ERROR] Unknown condition: " << cfg_.condition << "\n";
            return false;
        }
    }

private:
    AuditRunConfig cfg_;
    std::unique_ptr<rocksdb::DB> db_;

    void LogEnvironmentMetadata() {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
        std::string cpu_list;
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                if (!cpu_list.empty()) cpu_list += ",";
                cpu_list += std::to_string(i);
            }
        }
        std::cout << "[AUDIT METADATA] Condition: " << cfg_.condition
                  << " | Workload: " << cfg_.workload
                  << " | Rep: " << cfg_.rep
                  << " | Bound CPUs: [" << cpu_list << "]"
                  << " | Macro: ROCKSDB_READ_PATH_AUDIT=1\n";
    }

    bool OpenDB() {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = false;
        options.write_buffer_size = cfg_.write_buffer_size;
        options.max_write_buffer_number = 6;
        options.min_write_buffer_number_to_merge = 1;
        options.memtable_max_range_deletions = 0;
        options.disable_auto_compactions = true;
        options.compression = rocksdb::kNoCompression;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_size = 4 * 1024;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        std::filesystem::remove_all(cfg_.db_path);
        std::filesystem::create_directories(cfg_.db_path);

        rocksdb::Status s = rocksdb::DB::Open(options, cfg_.db_path, &db_);
        if (!s.ok()) {
            std::cerr << "[AUDIT ERROR] RocksDB Open failed: " << s.ToString() << "\n";
            return false;
        }
        return true;
    }

    bool PreloadData() {
        std::cout << "[AUDIT] Preloading " << cfg_.total_keys << " keys...\n";
        std::string val_payload(cfg_.value_size, 'V');
        const size_t kBatchSize = 1000;
        for (uint64_t i = 0; i < cfg_.total_keys; i += kBatchSize) {
            rocksdb::WriteBatch batch;
            for (uint64_t j = 0; j < kBatchSize && (i + j) < cfg_.total_keys; ++j) {
                batch.Put(FormatKey(i + j), val_payload);
            }
            rocksdb::WriteOptions wopt;
            wopt.disableWAL = true;
            rocksdb::Status s = db_->Write(wopt, &batch);
            if (!s.ok()) {
                std::cerr << "[AUDIT ERROR] Preload write failed: " << s.ToString() << "\n";
                return false;
            }
        }
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        rocksdb::Status s = db_->Flush(fopt);
        if (!s.ok()) {
            std::cerr << "[AUDIT ERROR] Preload Flush failed: " << s.ToString() << "\n";
            return false;
        }
        return true;
    }

    bool InjectDeleteRanges(WriteSideSnapshot& write_snap) {
        std::cout << "[AUDIT] Injecting 20,000 DeleteRanges from Phase B traces...\n";
        rocksdb::WriteOptions wopt;
        wopt.disableWAL = true;
        uint64_t del_count = 0;

        // Capture write-side cache invalidations during injection
        rocksdb::SetReadPathAuditEnabled(true);
        rocksdb::GetReadPathAuditStats()->Reset();

        for (int w = 0; w < 8; ++w) {
            char fname_buf[64];
            snprintf(fname_buf, sizeof(fname_buf), "phase_b-worker-%02d.bin", w);
            std::string fname = cfg_.trace_dir + "/" + fname_buf;
            if (!std::filesystem::exists(fname)) {
                if (w == 0) {
                    std::cerr << "[AUDIT ERROR] Trace missing: " << fname << "\n";
                    return false;
                }
                break;
            }
            auto fsize = std::filesystem::file_size(fname);
            size_t num_records = fsize / sizeof(FormalTraceRecord);
            std::vector<FormalTraceRecord> records(num_records);
            std::ifstream fin(fname, std::ios::binary);
            if (!fin.read(reinterpret_cast<char*>(records.data()), fsize)) {
                std::cerr << "[AUDIT ERROR] Reading " << fname << " failed.\n";
                return false;
            }
            for (const auto& rec : records) {
                if (rec.op_type == 3) {
                    std::string k1 = FormatKey(rec.key1);
                    std::string k2 = FormatKey(rec.key2);
                    rocksdb::Status s = db_->DeleteRange(wopt, db_->DefaultColumnFamily(), k1, k2);
                    if (!s.ok()) {
                        std::cerr << "[AUDIT ERROR] DeleteRange failed: " << s.ToString() << "\n";
                        return false;
                    }
                    del_count++;
                }
            }
        }

        // Record write snapshot BEFORE resetting for read window
        write_snap.delete_range_count = del_count;
        write_snap.cache_invalidation_count =
            rocksdb::GetReadPathAuditStats()->memtable_cache_invalidation_count;

        std::cout << "[AUDIT WRITE SNAPSHOT] Injected DeleteRanges=" << write_snap.delete_range_count
                  << " | CacheInvalidations=" << write_snap.cache_invalidation_count << "\n";

        rocksdb::GetReadPathAuditStats()->Reset();
        rocksdb::SetReadPathAuditEnabled(false);
        return true;
    }

    bool LoadAndFilterTrace(std::vector<FormalTraceRecord>& filtered_records) {
        std::string fname = cfg_.trace_dir + "/phase_c-worker-00.bin";
        if (!std::filesystem::exists(fname)) {
            std::cerr << "[AUDIT ERROR] Phase C trace missing: " << fname << "\n";
            return false;
        }
        auto fsize = std::filesystem::file_size(fname);
        size_t num_records = fsize / sizeof(FormalTraceRecord);
        std::vector<FormalTraceRecord> raw_records(num_records);
        std::ifstream fin(fname, std::ios::binary);
        if (!fin.read(reinterpret_cast<char*>(raw_records.data()), fsize)) {
            std::cerr << "[AUDIT ERROR] Reading Phase C trace failed.\n";
            return false;
        }

        size_t count_get = 0, count_scan = 0, count_other = 0;
        for (const auto& rec : raw_records) {
            if (cfg_.workload == "get_live") {
                if (rec.op_type == 0) {
                    filtered_records.push_back(rec);
                    count_get++;
                } else {
                    count_other++;
                }
            } else if (cfg_.workload == "scan_intersect") {
                if (rec.op_type == 1 && (rec.flags & 1)) {
                    filtered_records.push_back(rec);
                    count_scan++;
                } else {
                    count_other++;
                }
            }
        }

        std::cout << "[AUDIT TRACE FILTER] Workload: " << cfg_.workload
                  << " | RawRecords=" << num_records
                  << " | FilteredSelected=" << filtered_records.size()
                  << " | GetOps=" << count_get
                  << " | ScanOps=" << count_scan
                  << " | OtherOps=" << count_other << "\n";

        if (count_other != 0) {
            std::cerr << "[AUDIT ERROR] Found " << count_other << " non-matching records in filtered workload!\n";
            return false;
        }
        if (filtered_records.empty()) {
            std::cerr << "[AUDIT ERROR] No matching records found for workload: " << cfg_.workload << "\n";
            return false;
        }
        return true;
    }

    void ExecuteSingleOp(const FormalTraceRecord& rec, uint64_t& visible_keys) {
        rocksdb::ReadOptions ropt;
        ropt.total_order_seek = true;

        if (cfg_.workload == "get_live") {
            assert(rec.op_type == 0);
            std::string k = FormatKey(rec.key1);
            std::string val;
            rocksdb::Status s = db_->Get(ropt, k, &val);
            if (!s.ok()) {
                std::cerr << "[AUDIT ERROR] Get failed unexpectedly: " << s.ToString() << " on key " << k << "\n";
                exit(1);
            }
            visible_keys++;
        } else if (cfg_.workload == "scan_intersect") {
            assert(rec.op_type == 1);
            std::string start_k = FormatKey(rec.key1);
            std::string end_k = FormatKey(rec.key2);
            auto iter = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ropt));
            iter->Seek(start_k);
            uint64_t keys_in_this_scan = 0;
            while (iter->Valid()) {
                if (iter->key().ToString() >= end_k) {
                    break;
                }
                keys_in_this_scan++;
                iter->Next();
            }
            if (!iter->status().ok()) {
                std::cerr << "[AUDIT ERROR] Scan iterator error: " << iter->status().ToString() << "\n";
                exit(1);
            }
            visible_keys += keys_in_this_scan;
        }
    }

    void RecordRow(const std::string& sub_state, size_t ops, double elapsed_ms, uint64_t visible_keys,
                   const WriteSideSnapshot& write_snap, const rocksdb::ReadPathAuditStats& st) {
        double latency_us_per_op = (elapsed_ms * 1000.0) / (ops > 0 ? ops : 1);

        // Verification of correctness
        if (cfg_.workload == "get_live") {
            assert(visible_keys == ops && "GetLive must return exactly 1 visible key per op!");
        } else if (cfg_.workload == "scan_intersect") {
            if (cfg_.condition == "CLEAN") {
                assert(visible_keys == ops * 50 && "CLEAN scan must return exactly 50 keys per scan!");
            } else {
                assert(visible_keys == ops * 40 && "Intersect scan must return exactly 40 live keys (10 deleted)!");
            }
        }

        std::ofstream csv(cfg_.csv_path, std::ios::app);
        csv << cfg_.condition << ","
            << cfg_.workload << ","
            << sub_state << ","
            << cfg_.rep << ","
            << ops << ","
            << std::fixed << std::setprecision(3) << elapsed_ms << ","
            << std::fixed << std::setprecision(3) << latency_us_per_op << ","
            << visible_keys << ","
            << write_snap.delete_range_count << ","
            << write_snap.cache_invalidation_count << ","
            << st.range_tombstone_view_materialization_count << ","
            << std::fixed << std::setprecision(4) << (st.range_tombstone_view_materialization_nanos / 1e6) << ","
            << st.fragment_build_lock_attempt_count << ","
            << st.fragment_build_lock_contended_count << ","
            << std::fixed << std::setprecision(4) << (st.fragment_build_lock_contended_wait_nanos / 1e6) << ","
            << st.fragment_build_cache_race_hit_count << ","
            << st.active_mem_tombstone_iter_prepare_count << ","
            << std::fixed << std::setprecision(4) << (st.active_mem_tombstone_iter_prepare_nanos / 1e6) << ","
            << st.active_mem_tombstone_cover_lookup_count << ","
            << std::fixed << std::setprecision(4) << (st.active_mem_tombstone_cover_lookup_nanos / 1e6) << ","
            << st.imm_mem_tombstone_iter_prepare_count << ","
            << std::fixed << std::setprecision(4) << (st.imm_mem_tombstone_iter_prepare_nanos / 1e6) << ","
            << st.imm_mem_tombstone_cover_lookup_count << ","
            << std::fixed << std::setprecision(4) << (st.imm_mem_tombstone_cover_lookup_nanos / 1e6) << ","
            << st.active_mem_iter_construct_count << ","
            << std::fixed << std::setprecision(4) << (st.active_mem_iter_construct_nanos / 1e6) << ","
            << st.imm_mem_iter_construct_count << ","
            << std::fixed << std::setprecision(4) << (st.imm_mem_iter_construct_nanos / 1e6) << ","
            << st.sst_iter_construct_count << ","
            << std::fixed << std::setprecision(4) << (st.sst_iter_construct_nanos / 1e6) << ","
            << st.scan_range_del_reseek_count << ","
            << st.scan_boundary_advance_count << ","
            << st.scan_range_del_child_next_count << ","
            << st.scan_covered_skip_count << "\n";
        csv.flush();

        std::cout << "[AUDIT RESULT] " << cfg_.condition << " | " << cfg_.workload << " | " << sub_state
                  << " | Ops=" << ops << " | Latency=" << latency_us_per_op << " us/op"
                  << " | VisibleKeys=" << visible_keys
                  << " | WriteInvalidations=" << write_snap.cache_invalidation_count
                  << " | ViewMatCount=" << st.range_tombstone_view_materialization_count
                  << " | ViewMatTime=" << (st.range_tombstone_view_materialization_nanos / 1e6) << " ms"
                  << " | LockAttempts=" << st.fragment_build_lock_attempt_count
                  << " | LockContended=" << st.fragment_build_lock_contended_count
                  << " | IterPrepTime=" << (st.active_mem_tombstone_iter_prepare_nanos / 1e6) << " ms"
                  << " | CoverLookupTime=" << (st.active_mem_tombstone_cover_lookup_nanos / 1e6) << " ms"
                  << " | Reseeks=" << st.scan_range_del_reseek_count
                  << " | BoundaryAdv=" << st.scan_boundary_advance_count
                  << "\n";
    }

    bool RunClean(const std::vector<FormalTraceRecord>& records) {
        std::cout << "[AUDIT] Running CLEAN condition...\n";
        WriteSideSnapshot clean_write;
        clean_write.delete_range_count = 0;
        clean_write.cache_invalidation_count = 0;

        rocksdb::SetReadPathAuditEnabled(true);
        auto* stats = rocksdb::GetReadPathAuditStats();
        stats->Reset();

        uint64_t vis_keys = 0;
        size_t total_ops = std::min(cfg_.warm_ops, records.size());
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < total_ops; ++i) {
            ExecuteSingleOp(records[i], vis_keys);
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        RecordRow("Warm-steady", total_ops, elapsed_ms, vis_keys, clean_write, *stats);
        rocksdb::SetReadPathAuditEnabled(false);
        return true;
    }

    bool RunT0Mem(const std::vector<FormalTraceRecord>& records) {
        std::cout << "[AUDIT] Running T0-MEM condition...\n";
        WriteSideSnapshot inj_write;
        if (!InjectDeleteRanges(inj_write)) return false;

        auto* stats = rocksdb::GetReadPathAuditStats();

        // 1. Cold-build: First read immediately after injection
        std::cout << "[AUDIT] Measuring Cold-build (1st op)...\n";
        stats->Reset();
        rocksdb::SetReadPathAuditEnabled(true);
        uint64_t cold_vis_keys = 0;
        auto t0 = std::chrono::steady_clock::now();
        ExecuteSingleOp(records[0], cold_vis_keys);
        auto t1 = std::chrono::steady_clock::now();
        double cold_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        RecordRow("Cold-build", 1, cold_ms, cold_vis_keys, inj_write, *stats);

        // 2. Warm-steady: Next N ops with cached fragmented list
        std::cout << "[AUDIT] Measuring Warm-steady (" << cfg_.warm_ops << " ops)...\n";
        stats->Reset();
        uint64_t warm_vis_keys = 0;
        size_t total_ops = std::min(cfg_.warm_ops, records.size() - 1);
        t0 = std::chrono::steady_clock::now();
        for (size_t i = 1; i <= total_ops; ++i) {
            ExecuteSingleOp(records[i], warm_vis_keys);
        }
        t1 = std::chrono::steady_clock::now();
        double warm_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        RecordRow("Warm-steady", total_ops, warm_ms, warm_vis_keys, inj_write, *stats);

        // 3. Re-invalidated: Inject 1 new range tombstone to invalidate cache, then measure next read
        std::cout << "[AUDIT] Measuring Re-invalidated (1 DeleteRange injected + 1st op)...\n";
        stats->Reset();
        rocksdb::WriteOptions wopt;
        wopt.disableWAL = true;
        rocksdb::Status s = db_->DeleteRange(wopt, db_->DefaultColumnFamily(),
                                             FormatKey(499900), FormatKey(499910));
        if (!s.ok()) {
            std::cerr << "[AUDIT ERROR] Invalidation DeleteRange failed: " << s.ToString() << "\n";
            return false;
        }

        WriteSideSnapshot reinval_write;
        reinval_write.delete_range_count = 1;
        reinval_write.cache_invalidation_count = stats->memtable_cache_invalidation_count;

        stats->Reset();
        uint64_t reinval_vis_keys = 0;
        t0 = std::chrono::steady_clock::now();
        ExecuteSingleOp(records[0], reinval_vis_keys);
        t1 = std::chrono::steady_clock::now();
        double reinval_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        RecordRow("Re-invalidated", 1, reinval_ms, reinval_vis_keys, reinval_write, *stats);

        rocksdb::SetReadPathAuditEnabled(false);
        return true;
    }

    bool RunPostFlushSST(const std::vector<FormalTraceRecord>& records) {
        std::cout << "[AUDIT] Running POSTFLUSH-SST condition...\n";
        WriteSideSnapshot inj_write;
        if (!InjectDeleteRanges(inj_write)) return false;

        std::cout << "[AUDIT] Flushing DeleteRanges to L0 SST...\n";
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        rocksdb::Status s = db_->Flush(fopt);
        if (!s.ok()) {
            std::cerr << "[AUDIT ERROR] Post-Inject Flush failed: " << s.ToString() << "\n";
            return false;
        }

        rocksdb::SetReadPathAuditEnabled(true);
        auto* stats = rocksdb::GetReadPathAuditStats();
        stats->Reset();

        uint64_t vis_keys = 0;
        size_t total_ops = std::min(cfg_.warm_ops, records.size());
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < total_ops; ++i) {
            ExecuteSingleOp(records[i], vis_keys);
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        RecordRow("Warm-steady", total_ops, elapsed_ms, vis_keys, inj_write, *stats);
        rocksdb::SetReadPathAuditEnabled(false);
        return true;
    }
};

int main(int argc, char** argv) {
    AuditRunConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--condition" && i + 1 < argc) cfg.condition = argv[++i];
        else if (arg == "--workload" && i + 1 < argc) cfg.workload = argv[++i];
        else if (arg == "--db_path" && i + 1 < argc) cfg.db_path = argv[++i];
        else if (arg == "--trace_dir" && i + 1 < argc) cfg.trace_dir = argv[++i];
        else if (arg == "--csv_path" && i + 1 < argc) cfg.csv_path = argv[++i];
        else if (arg == "--rep" && i + 1 < argc) cfg.rep = std::stoi(argv[++i]);
        else if (arg == "--warm_ops" && i + 1 < argc) cfg.warm_ops = std::stoull(argv[++i]);
    }

    if (cfg.condition.empty() || cfg.workload.empty() || cfg.db_path.empty() ||
        cfg.trace_dir.empty() || cfg.csv_path.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --condition <CLEAN|T0-MEM|POSTFLUSH-SST>"
                  << " --workload <get_live|scan_intersect>"
                  << " --db_path <path>"
                  << " --trace_dir <path>"
                  << " --csv_path <path>"
                  << " [--rep <N>] [--warm_ops <N>]\n";
        return 1;
    }

    if (!std::filesystem::exists(cfg.csv_path)) {
        std::ofstream csv(cfg.csv_path);
        csv << "condition,workload,sub_state,rep,ops,total_time_ms,latency_us_per_op,visible_keys,"
            << "write_del_ranges,write_cache_invalidations,"
            << "view_mat_count,view_mat_ms,"
            << "lock_attempt_count,lock_contended_count,lock_contended_wait_ms,lock_race_hit_count,"
            << "active_mem_iter_prep_count,active_mem_iter_prep_ms,"
            << "active_mem_cover_lookup_count,active_mem_cover_lookup_ms,"
            << "imm_mem_iter_prep_count,imm_mem_iter_prep_ms,"
            << "imm_mem_cover_lookup_count,imm_mem_cover_lookup_ms,"
            << "active_mem_iter_construct_count,active_mem_iter_construct_ms,"
            << "imm_mem_iter_construct_count,imm_mem_iter_construct_ms,"
            << "sst_iter_construct_count,sst_iter_construct_ms,"
            << "scan_range_del_reseek_count,scan_boundary_advance_count,"
            << "scan_range_del_child_next_count,scan_covered_skip_count\n";
    }

    AuditHarness harness(cfg);
    if (!harness.Run()) {
        std::cerr << "[AUDIT ERROR] Execution failed.\n";
        return 2;
    }

    return 0;
}
