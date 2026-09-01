#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cassert>
#include <fstream>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"

#include "p7_config.h"
#include "p7_reference_model.h"
#include "p7_stats_collector.h"

using namespace study::p7;

struct P7Op {
    uint8_t op_type; // 0=Get, 1=Scan, 2=Put, 3=DeleteRange
    uint64_t key1;
    uint64_t key2;
};

class P7Driver {
public:
    P7Driver(const P7Config& config)
        : config_(config),
          ref_model_(config.total_keys, config.value_size),
          stop_requested_(false),
          completed_ops_(0),
          next_progress_milestone_(2000) {} // 1% of 200,000 = 2,000

    bool LoadTrace() {
        std::ifstream file(config_.trace_path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open P7 trace: " << config_.trace_path << std::endl;
            return false;
        }

        trace_ops_.clear();
        uint8_t op;
        uint64_t k1, k2;
        while (file.read(reinterpret_cast<char*>(&op), sizeof(op)) &&
               file.read(reinterpret_cast<char*>(&k1), sizeof(k1)) &&
               file.read(reinterpret_cast<char*>(&k2), sizeof(k2))) {
            trace_ops_.push_back({op, k1, k2});
        }
        std::cout << "[P7Driver] Loaded " << trace_ops_.size() << " deterministic ops from " << config_.trace_path << "\n";
        if (trace_ops_.size() != config_.total_ops) {
            std::cerr << "Warning: Loaded ops " << trace_ops_.size() << " != total_ops " << config_.total_ops << "\n";
        }
        return true;
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
            std::cerr << "Failed to open RocksDB at " << config_.db_path << ": " << status.ToString() << std::endl;
            return false;
        }
        return true;
    }

    bool PreloadDatabase() {
        std::cout << "[Preload] Preloading " << config_.total_keys << " keys (256B value) into DB...\n";
        auto t0 = std::chrono::high_resolution_clock::now();

        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = false;

        for (uint64_t k = 0; k < config_.total_keys; ++k) {
            std::string key = P7ReferenceModel::FormatKey(k);
            std::string val = P7ReferenceModel::GenerateValue(k, 1, config_.value_size);
            rocksdb::Status s = db_->Put(write_opts, key, val);
            if (!s.ok()) {
                std::cerr << "Preload Put failed on key " << key << ": " << s.ToString() << std::endl;
                return false;
            }
        }

        std::cout << "[Preload] Flushing preloaded data to SST...\n";
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;
        rocksdb::Status fs = db_->Flush(flush_opts);
        if (!fs.ok()) return false;

        auto t1 = std::chrono::high_resolution_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Completed in " << std::fixed << std::setprecision(2) << preload_sec << " s.\n";
        return true;
    }

    void RunExperiment() {
        std::cout << "\n=========================================================\n";
        std::cout << "Starting P7 P1-Replay Experiment: " << config_.exp_id << " (" << config_.group_name << ")\n";
        std::cout << "Total Requests: " << config_.total_ops << ", Threads: " << config_.num_threads
                  << ", DeleteRange Ratio: " << (config_.del_range_ratio * 100.0) << "%\n";
        std::cout << "=========================================================\n";

        db_stats_->Reset();
        stop_requested_.store(false);
        completed_ops_.store(0);
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

                stats_.CaptureWallClockSecond(sec++, elapsed, cur_ops, prog_pct, db_.get(), db_stats_, config_.db_path);
            }
        });

        // Worker Threads
        std::vector<std::thread> workers;
        workers.reserve(config_.num_threads);

        std::atomic<uint64_t> op_cursor(0);
        size_t total_trace_ops = trace_ops_.size();
        uint64_t step_size = config_.total_ops / 100; // 1%

        for (int t = 0; t < config_.num_threads; ++t) {
            workers.emplace_back([&, t]() {
                rocksdb::ReadOptions read_opts;
                rocksdb::WriteOptions write_opts;

                while (true) {
                    uint64_t idx = op_cursor.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= total_trace_ops) break;

                    const auto& op = trace_ops_[idx];

                    if (op.op_type == 0) {
                        // Get
                        int cat = ref_model_.ClassifyGet(op.key1);
                        std::string key_str = P7ReferenceModel::FormatKey(op.key1);
                        std::string val_str;
                        auto t0 = std::chrono::high_resolution_clock::now();
                        db_->Get(read_opts, key_str, &val_str);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        stats_.RecordGet(cat, lat_ns);

                    } else if (op.op_type == 1) {
                        // Scan
                        uint64_t begin_k = op.key1;
                        uint64_t end_k = std::min(begin_k + op.key2, config_.total_keys);
                        uint64_t span_req = end_k - begin_k;

                        std::string begin_key = P7ReferenceModel::FormatKey(begin_k);
                        std::string end_key = P7ReferenceModel::FormatKey(end_k);

                        size_t returned_keys = 0;
                        auto t0 = std::chrono::high_resolution_clock::now();
                        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
                        it->Seek(begin_key);
                        while (it->Valid()) {
                            std::string k_str = it->key().ToString();
                            if (k_str >= end_key) break;
                            returned_keys++;
                            it->Next();
                        }
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        stats_.RecordScan(lat_ns, span_req, returned_keys);

                    } else if (op.op_type == 2) {
                        // Put
                        std::string key_str = P7ReferenceModel::FormatKey(op.key1);
                        std::string val_str = P7ReferenceModel::GenerateValue(op.key1, 2, config_.value_size);
                        auto t0 = std::chrono::high_resolution_clock::now();
                        rocksdb::Status s = db_->Put(write_opts, key_str, val_str);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        if (s.ok()) stats_.RecordPut(lat_ns);

                    } else if (op.op_type == 3) {
                        // DeleteRange
                        std::string b_k = P7ReferenceModel::FormatKey(op.key1);
                        std::string e_k = P7ReferenceModel::FormatKey(op.key2);
                        auto t0 = std::chrono::high_resolution_clock::now();
                        rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), b_k, e_k);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        if (s.ok()) {
                            ref_model_.ApplyDeleteRange(op.key1, op.key2);
                            stats_.RecordDeleteRange(lat_ns);
                        }
                    }

                    uint64_t cur_done = completed_ops_.fetch_add(1, std::memory_order_relaxed) + 1;

                    // Check progress milestone (1% = every step_size)
                    uint64_t cur_milestone = next_progress_milestone_.load(std::memory_order_relaxed);
                    if (cur_done >= cur_milestone) {
                        if (next_progress_milestone_.compare_exchange_strong(cur_milestone, cur_milestone + step_size)) {
                            int prog_pct = static_cast<int>(cur_milestone / step_size);
                            auto now = std::chrono::high_resolution_clock::now();
                            double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time).count();
                            stats_.CaptureProgressSnapshot(prog_pct, elapsed, cur_done, db_.get(), db_stats_, config_.db_path);
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

        stats_.AppendSummaryCsv(config_.summary_csv, config_.exp_id, config_.group_name, config_.desc,
                                total_elapsed_sec, db_.get(), db_stats_,
                                union_del_keys, union_cov_pct, tombstones_cnt, sha256_hex,
                                ver_ok, ver_nf, (sample_pass && scan_pass));

        stats_.DumpWallClockCsv(config_.ts_wallclock_csv, config_.exp_id, config_.group_name);
        stats_.DumpProgressCsv(config_.ts_progress_csv, config_.exp_id, config_.group_name);
    }

private:
    P7Config config_;
    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::unique_ptr<rocksdb::DB> db_;
    P7ReferenceModel ref_model_;
    P7StatsCollector stats_;
    std::vector<P7Op> trace_ops_;
    std::atomic<bool> stop_requested_;
    std::atomic<uint64_t> completed_ops_;
    std::atomic<uint64_t> next_progress_milestone_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    P7Config config;

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
        }
    }

    P7Driver driver(config);
    if (!driver.LoadTrace()) return 1;
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    driver.RunExperiment();

    return 0;
}
