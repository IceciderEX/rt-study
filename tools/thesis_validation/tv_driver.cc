#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cassert>
#include <fstream>
#include <iomanip>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/write_batch.h"

#include "tv_config.h"
#include "tv_reference_model.h"
#include "tv_stats_collector.h"

using namespace study::tv;

struct TVOp {
    uint8_t op_type; // 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=No-op
    uint64_t key1;
    uint64_t key2;
};

class TVDriver {
public:
    TVDriver(const TVConfig& config)
        : config_(config),
          ref_model_(config.total_keys, config.value_size),
          stop_requested_(false),
          experiment_failed_(false),
          completed_ops_(0),
          cumulative_del_ranges_(0),
          next_progress_milestone_(2000) {}

    bool LoadTrace() {
        if (!config_.trace_path.empty() && std::filesystem::exists(config_.trace_path)) {
            // Strict Trace Size & Format Verification
            auto file_size = std::filesystem::file_size(config_.trace_path);
            const size_t kRecordSize = 17; // 1 byte op_type + 8 bytes key1 + 8 bytes key2
            if (file_size % kRecordSize != 0) {
                std::cerr << "[TVDriver ERROR] Trace file size (" << file_size 
                          << " bytes) is not a multiple of " << kRecordSize << " bytes: " 
                          << config_.trace_path << std::endl;
                return false;
            }

            std::ifstream file(config_.trace_path, std::ios::binary);
            if (!file.is_open()) {
                std::cerr << "[TVDriver ERROR] Failed to open TV trace: " << config_.trace_path << std::endl;
                return false;
            }

            trace_ops_.clear();
            trace_ops_.reserve(file_size / kRecordSize);
            uint8_t op;
            uint64_t k1, k2;
            while (file.read(reinterpret_cast<char*>(&op), sizeof(op)) &&
                   file.read(reinterpret_cast<char*>(&k1), sizeof(k1)) &&
                   file.read(reinterpret_cast<char*>(&k2), sizeof(k2))) {
                if (op > 4) {
                    std::cerr << "[TVDriver ERROR] Invalid opcode " << static_cast<int>(op) 
                              << " encountered in trace: " << config_.trace_path << std::endl;
                    return false;
                }
                trace_ops_.push_back({op, k1, k2});
            }

            if (config_.total_ops > 0 && trace_ops_.size() != config_.total_ops) {
                std::cerr << "[TVDriver ERROR] Loaded ops count (" << trace_ops_.size() 
                          << ") does not match configured total_ops (" << config_.total_ops 
                          << "): " << config_.trace_path << std::endl;
                return false;
            }

            std::cout << "[TVDriver] Loaded and verified " << trace_ops_.size() 
                      << " deterministic ops from " << config_.trace_path << "\n";
            for (const auto& op : trace_ops_) {
                if (op.op_type == 2) ref_model_.ApplyPut(op.key1);
                else if (op.op_type == 3) ref_model_.ApplyDeleteRange(op.key1, op.key2);
            }
            return true;
        } else {
            std::cout << "[TVDriver] Generating " << config_.total_ops << " ops on the fly with seed " << config_.random_seed << "...\n";
            trace_ops_.clear();
            trace_ops_.reserve(config_.total_ops);

            std::mt19937_64 rng(config_.random_seed);
            std::uniform_real_distribution<double> dist_op(0.0, 1.0);
            std::uniform_int_distribution<uint64_t> dist_k(0, config_.total_keys - 1);

            double p_del = config_.del_range_ratio;
            double p_put = p_del + config_.put_ratio;
            double p_scan = p_put + config_.scan_ratio;

            for (uint64_t i = 0; i < config_.total_ops; ++i) {
                double r = dist_op(rng);
                if (r < p_del) {
                    uint64_t b = dist_k(rng);
                    uint64_t e = std::min(b + config_.del_range_len, config_.total_keys);
                    trace_ops_.push_back({3, b, e});
                    ref_model_.ApplyDeleteRange(b, e);
                } else if (r < p_put) {
                    uint64_t k = dist_k(rng);
                    trace_ops_.push_back({2, k, 0});
                    ref_model_.ApplyPut(k);
                } else if (r < p_scan) {
                    uint64_t b = dist_k(rng);
                    trace_ops_.push_back({1, b, config_.scan_span});
                } else {
                    uint64_t k = dist_k(rng);
                    trace_ops_.push_back({0, k, 0});
                }
            }
            std::cout << "[TVDriver] Generated " << trace_ops_.size() << " ops.\n";
            return true;
        }
    }

    bool InitializeDB() {
        std::filesystem::create_directories(config_.db_path);
        std::filesystem::create_directories(config_.result_dir);

        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = false;
        options.compression = rocksdb::kNoCompression;

        options.write_buffer_size = config_.write_buffer_size;
        options.max_write_buffer_number = config_.max_write_buffer_number;
        options.level0_file_num_compaction_trigger = config_.level0_file_num_compaction_trigger;
        options.level0_slowdown_writes_trigger = config_.level0_slowdown_writes_trigger;
        options.level0_stop_writes_trigger = config_.level0_stop_writes_trigger;
        options.target_file_size_base = config_.target_file_size_base;
        options.max_bytes_for_level_base = config_.max_bytes_for_level_base;
        options.max_background_jobs = config_.max_background_jobs;

        options.memtable_max_range_deletions = config_.memtable_max_range_deletions;
        options.memtable_op_scan_flush_trigger = config_.memtable_op_scan_flush_trigger;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_size = 4 * 1024;
        table_options.block_cache = rocksdb::NewLRUCache(config_.block_cache_size);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        db_stats_ = rocksdb::CreateDBStatistics();
        options.statistics = db_stats_;

        options_ = options;

        rocksdb::Status status = rocksdb::DB::Open(options_, config_.db_path, &db_);
        if (!status.ok()) {
            std::cerr << "[TVDriver ERROR] Failed to open RocksDB at " << config_.db_path << ": " << status.ToString() << std::endl;
            return false;
        }
        return true;
    }

    bool PreloadDatabase() {
        std::cout << "[Preload] Preloading " << config_.total_keys << " keys (Value size=" << config_.value_size << " B) into DB...\n";
        auto t0 = std::chrono::high_resolution_clock::now();

        int num_threads = config_.num_threads > 0 ? config_.num_threads : 8;
        uint64_t total_keys = config_.total_keys;
        uint64_t keys_per_thread = (total_keys + num_threads - 1) / num_threads;

        std::vector<std::thread> workers;
        std::atomic<bool> preload_failed(false);

        for (int t = 0; t < num_threads; ++t) {
            uint64_t start_k = t * keys_per_thread;
            uint64_t end_k = std::min(start_k + keys_per_thread, total_keys);
            if (start_k >= total_keys) break;

            workers.emplace_back([&, start_k, end_k]() {
                rocksdb::WriteOptions write_opts;
                write_opts.disableWAL = false;
                const size_t kBatchSize = 4096;

                for (uint64_t k = start_k; k < end_k && !preload_failed.load(); k += kBatchSize) {
                    rocksdb::WriteBatch batch;
                    uint64_t batch_end = std::min(k + kBatchSize, end_k);
                    for (uint64_t i = k; i < batch_end; ++i) {
                        std::string key = TVReferenceModel::FormatKey(i);
                        std::string val = TVReferenceModel::GenerateValue(i, 1, config_.value_size);
                        batch.Put(key, val);
                    }
                    rocksdb::Status s = db_->Write(write_opts, &batch);
                    if (!s.ok()) {
                        std::cerr << "[Preload ERROR] WriteBatch failed: " << s.ToString() << std::endl;
                        preload_failed.store(true);
                        break;
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        if (preload_failed.load()) return false;

        std::cout << "[Preload] Flushing preloaded data to SST...\n";
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;
        rocksdb::Status fs = db_->Flush(flush_opts);
        if (!fs.ok()) {
            std::cerr << "[Preload ERROR] Flush failed: " << fs.ToString() << std::endl;
            return false;
        }

        std::cout << "[Preload] Waiting for background compaction to settle...\n";
        rocksdb::WaitForCompactOptions wait_opts;
        db_->WaitForCompact(wait_opts);

        auto t1 = std::chrono::high_resolution_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Completed in " << std::fixed << std::setprecision(2) << preload_sec << " s.\n";
        return true;
    }

    bool RunExperiment() {
        std::cout << "\n=========================================================\n";
        std::cout << "Starting TV Validation Experiment: " << config_.exp_id << " (" << config_.group_name << ")\n";
        std::cout << "Value Size: " << config_.value_size << " B, Native memtable_max_range_deletions = "
                  << config_.memtable_max_range_deletions << " ("
                  << (config_.memtable_max_range_deletions == 0 ? "DISABLED" : "ENABLED") << ")\n";
        std::cout << "DeleteRange Ratio: " << (config_.del_range_ratio * 100.0) << "%, Threads: " << config_.num_threads << "\n";
        std::cout << "=========================================================\n";

        db_stats_->Reset();
        stop_requested_.store(false);
        experiment_failed_.store(false);
        completed_ops_.store(0);
        cumulative_del_ranges_.store(0);
        next_progress_milestone_.store(config_.total_ops / 100);

        auto exp_start_time = std::chrono::high_resolution_clock::now();

        // 1-second wall-clock monitor thread
        std::thread wallclock_thread([&]() {
            int sec = 1;
            while (!stop_requested_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (stop_requested_.load()) break;

                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time).count();
                uint64_t cur_ops = completed_ops_.load();
                double prog_pct = (config_.total_ops > 0) ? (100.0 * cur_ops / config_.total_ops) : 0.0;
                uint64_t cur_del = cumulative_del_ranges_.load();

                stats_.CaptureWallClockSecond(sec++, elapsed, cur_ops, prog_pct, cur_del, db_.get(), db_stats_, config_.db_path);
            }
        });

        // Worker Threads (Deterministic Trace Replay across Concurrent Workers)
        std::vector<std::thread> workers;
        workers.reserve(config_.num_threads);

        std::atomic<uint64_t> op_cursor(0);
        size_t total_trace_ops = trace_ops_.size();
        uint64_t step_size = config_.total_ops / 100; // 1%

        for (int t = 0; t < config_.num_threads; ++t) {
            workers.emplace_back([&, t]() {
                rocksdb::ReadOptions read_opts;
                rocksdb::WriteOptions write_opts;

                while (!experiment_failed_.load()) {
                    uint64_t idx = op_cursor.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= total_trace_ops) break;

                    const auto& op = trace_ops_[idx];
                    auto t_start = std::chrono::high_resolution_clock::now();

                    if (op.op_type == 0) { // Get
                        std::string key = TVReferenceModel::FormatKey(op.key1);
                        std::string val;
                        rocksdb::Status s = db_->Get(read_opts, key, &val);
                        if (!s.ok() && !s.IsNotFound()) {
                            std::cerr << "[TVDriver ERROR] Get failed with unexpected status: " << s.ToString() << std::endl;
                            experiment_failed_.store(true);
                            break;
                        }
                        auto t_end = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();

                        TVGetCategory cat = ref_model_.ClassifyGet(op.key1);
                        stats_.RecordGet(static_cast<int>(cat), lat_ns);
                    } else if (op.op_type == 1) { // Scan (LIMIT k Valid Keys Scan)
                        std::string start_key = TVReferenceModel::FormatKey(op.key1);
                        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
                        it->Seek(start_key);
                        uint64_t keys_found = 0;
                        while (it->Valid() && keys_found < op.key2) {
                            keys_found++;
                            it->Next();
                        }
                        if (!it->status().ok()) {
                            std::cerr << "[TVDriver ERROR] Scan iterator error: " << it->status().ToString() << std::endl;
                            experiment_failed_.store(true);
                            break;
                        }
                        auto t_end = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                        stats_.RecordScan(lat_ns, op.key2, keys_found);
                    } else if (op.op_type == 2) { // Put
                        std::string key = TVReferenceModel::FormatKey(op.key1);
                        std::string val = TVReferenceModel::GenerateValue(op.key1, 2, config_.value_size);
                        rocksdb::Status s = db_->Put(write_opts, key, val);
                        if (!s.ok()) {
                            std::cerr << "[TVDriver ERROR] Put failed: " << s.ToString() << std::endl;
                            experiment_failed_.store(true);
                            break;
                        }
                        auto t_end = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                        stats_.RecordPut(lat_ns);
                    } else if (op.op_type == 3) { // DeleteRange
                        std::string start_key = TVReferenceModel::FormatKey(op.key1);
                        std::string end_key = TVReferenceModel::FormatKey(op.key2);
                        rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), start_key, end_key);
                        if (!s.ok()) {
                            std::cerr << "[TVDriver ERROR] DeleteRange failed: " << s.ToString() << std::endl;
                            experiment_failed_.store(true);
                            break;
                        }
                        cumulative_del_ranges_.fetch_add(1, std::memory_order_relaxed);
                        auto t_end = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                        stats_.RecordDeleteRange(lat_ns);
                    } else if (op.op_type == 4) { // No-op
                        // Functional No-DeleteRange Clean Baseline: do nothing
                    }

                    uint64_t cur_done = completed_ops_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (step_size > 0 && cur_done >= next_progress_milestone_.load(std::memory_order_relaxed)) {
                        uint64_t old_val = next_progress_milestone_.load(std::memory_order_relaxed);
                        if (cur_done >= old_val && next_progress_milestone_.compare_exchange_strong(old_val, old_val + step_size)) {
                            double prog_pct = (100.0 * cur_done) / config_.total_ops;
                            auto now = std::chrono::high_resolution_clock::now();
                            double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time).count();
                            uint64_t cur_del = cumulative_del_ranges_.load();
                            stats_.CaptureProgressSnapshot(prog_pct, elapsed, cur_done, cur_del, db_.get(), db_stats_, config_.db_path);
                        }
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        auto exp_end_time = std::chrono::high_resolution_clock::now();
        double total_elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(exp_end_time - exp_start_time).count();

        stop_requested_.store(true);
        if (wallclock_thread.joinable()) wallclock_thread.join();

        if (experiment_failed_.load()) {
            std::cerr << "\n[TVDriver CRITICAL ERROR] Experiment failed during workload execution!\n";
            return false;
        }

        // 10-second post-run cooldown observation
        std::cout << "[Phase D] Entering 10-second post-run cooldown observation...\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Post-run validation & SHA-256 computation
        std::cout << "\n[Validation] Running Post-Experiment Verification & Computing DB State SHA-256...\n";
        size_t ver_ok = 0, ver_nf = 0;
        std::string err_msg;
        bool sample_pass = ref_model_.SampleVerification(db_.get(), 10000, ver_ok, ver_nf, err_msg);

        uint64_t db_live = 0, db_bytes = 0;
        std::string sha256_hex;
        bool scan_pass = ref_model_.FullScanAndComputeSha256(db_.get(), db_live, db_bytes, sha256_hex, err_msg);

        uint64_t union_del_keys = ref_model_.GetUnionDeletedKeys();
        double union_cov_pct = ref_model_.GetUnionCoverageRatio() * 100.0;
        uint64_t tombstones_cnt = ref_model_.GetTombstonesCount();

        std::cout << "[Validation Result] Live Keys=" << db_live << " (Deleted Keys=" << union_del_keys
                  << ", Coverage=" << union_cov_pct << "%), Payload=" << (db_bytes / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "[State Checksum] SHA-256 = " << sha256_hex << "\n";
        std::cout << "[Verification] Sample: " << (sample_pass ? "PASS" : "FAIL")
                  << ", Full Scan: " << (scan_pass ? "PASS" : "FAIL") << "\n";

        if (!sample_pass || !scan_pass) {
            std::cerr << "[TVDriver CRITICAL ERROR] Validation check failed!\n";
            return false;
        }

        stats_.AppendSummaryCsv(config_.summary_csv, config_.exp_id, config_.group_name, config_.desc,
                                total_elapsed_sec, config_.value_size, config_.memtable_max_range_deletions,
                                db_.get(), db_stats_,
                                union_del_keys, union_cov_pct, tombstones_cnt, sha256_hex,
                                ver_ok, ver_nf, (sample_pass && scan_pass));

        stats_.DumpWallClockCsv(config_.ts_wallclock_csv, config_.exp_id, config_.group_name);
        stats_.DumpProgressCsv(config_.ts_progress_csv, config_.exp_id, config_.group_name);

        return true;
    }

private:
    TVConfig config_;
    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::unique_ptr<rocksdb::DB> db_;
    TVReferenceModel ref_model_;
    TVStatsCollector stats_;
    std::vector<TVOp> trace_ops_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> experiment_failed_;
    std::atomic<uint64_t> completed_ops_;
    std::atomic<uint64_t> cumulative_del_ranges_;
    std::atomic<uint64_t> next_progress_milestone_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    TVConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
        else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
        else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
        else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
        else if (arg == "--ts_wallclock_csv" && i + 1 < argc) config.ts_wallclock_csv = argv[++i];
        else if (arg == "--ts_progress_csv" && i + 1 < argc) config.ts_progress_csv = argv[++i];
        else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoul(argv[++i]);
        else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
    }

    if (!config_file.empty()) {
        if (!config.ParseIni(config_file)) {
            std::cerr << "Failed to parse config file: " << config_file << std::endl;
            return 1;
        }
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
            else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
            else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
            else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
            else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
            else if (arg == "--ts_wallclock_csv" && i + 1 < argc) config.ts_wallclock_csv = argv[++i];
            else if (arg == "--ts_progress_csv" && i + 1 < argc) config.ts_progress_csv = argv[++i];
            else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoul(argv[++i]);
            else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }

    TVDriver driver(config);
    if (!driver.LoadTrace()) return 1;
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    if (!driver.RunExperiment()) return 1;

    return 0;
}
