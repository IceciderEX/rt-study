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

#include "p6_config.h"
#include "p6_reference_model.h"
#include "p6_stats_collector.h"

using namespace study::p6;

struct FrontendOp {
    uint8_t op_type; // 0=Get, 1=Scan, 2=Put
    uint64_t key1;
    uint64_t key2;
};

class P6Driver {
public:
    P6Driver(const P6Config& config)
        : config_(config),
          ref_model_(config.total_keys, config.value_size),
          stop_requested_(false) {}

    bool LoadTraces() {
        if (!ref_model_.LoadDeleteRangeTrace(config_.deleterange_trace_path)) {
            return false;
        }

        // Load Frontend Trace
        std::ifstream fe_file(config_.frontend_trace_path, std::ios::binary);
        if (!fe_file.is_open()) {
            std::cerr << "Failed to open frontend trace: " << config_.frontend_trace_path << std::endl;
            return false;
        }

        frontend_ops_.clear();
        uint8_t op;
        uint64_t k1, k2;
        while (fe_file.read(reinterpret_cast<char*>(&op), sizeof(op)) &&
               fe_file.read(reinterpret_cast<char*>(&k1), sizeof(k1)) &&
               fe_file.read(reinterpret_cast<char*>(&k2), sizeof(k2))) {
            frontend_ops_.push_back({op, k1, k2});
        }
        std::cout << "[P6Driver] Loaded " << frontend_ops_.size() << " deterministic frontend ops from " << config_.frontend_trace_path << "\n";
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
        std::cout << "[Preload] Setting up DB state for mode: " << config_.preload_mode << " ...\n";
        auto t0 = std::chrono::high_resolution_clock::now();

        ref_model_.SetupState(config_.preload_mode);

        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = false;
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;

        if (config_.preload_mode == "d0_clean") {
            // D0: Insert only 600,000 keys in L -> Flush
            uint64_t inserted = 0;
            for (uint64_t k = 0; k < config_.total_keys; ++k) {
                if (ref_model_.IsAlive(k)) {
                    std::string key = P6ReferenceModel::FormatKey(k);
                    std::string val = P6ReferenceModel::GenerateValue(k, 1, config_.value_size);
                    rocksdb::Status s = db_->Put(write_opts, key, val);
                    if (!s.ok()) {
                        std::cerr << "Preload Put failed on key " << key << ": " << s.ToString() << std::endl;
                        return false;
                    }
                    inserted++;
                }
            }
            std::cout << "[Preload] D0 inserted " << inserted << " surviving keys. Flushing to SST...\n";
            rocksdb::Status fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;

        } else if (config_.preload_mode == "d1_static") {
            // D1: Insert full 1,000,000 keys -> Flush -> DeleteRange U -> Flush
            std::cout << "[Preload] D1 inserting full " << config_.total_keys << " keys...\n";
            for (uint64_t k = 0; k < config_.total_keys; ++k) {
                std::string key = P6ReferenceModel::FormatKey(k);
                std::string val = P6ReferenceModel::GenerateValue(k, 1, config_.value_size);
                rocksdb::Status s = db_->Put(write_opts, key, val);
                if (!s.ok()) return false;
            }
            std::cout << "[Preload] D1 Initial Flush to SST...\n";
            rocksdb::Status fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;

            const auto& del_ops = ref_model_.GetDeleteRangeOps();
            std::cout << "[Preload] D1 Applying " << del_ops.size() << " DeleteRange operations from trace...\n";
            for (const auto& op : del_ops) {
                std::string b_k = P6ReferenceModel::FormatKey(op.begin);
                std::string e_k = P6ReferenceModel::FormatKey(op.end);
                rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), b_k, e_k);
                if (!s.ok()) return false;
            }

            std::cout << "[Preload] D1 Flushing range tombstones to SST...\n";
            fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;

        } else if (config_.preload_mode == "d2_dynamic") {
            // D2: Insert full 1,000,000 keys -> Flush (DeleteRanges run during Main)
            std::cout << "[Preload] D2 inserting full " << config_.total_keys << " keys...\n";
            for (uint64_t k = 0; k < config_.total_keys; ++k) {
                std::string key = P6ReferenceModel::FormatKey(k);
                std::string val = P6ReferenceModel::GenerateValue(k, 1, config_.value_size);
                rocksdb::Status s = db_->Put(write_opts, key, val);
                if (!s.ok()) return false;
            }
            std::cout << "[Preload] D2 Initial Flush to SST...\n";
            rocksdb::Status fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Complete in " << std::fixed << std::setprecision(2) << preload_sec << " s. Live Keys in RefModel = "
                  << ref_model_.GetLiveKeyCount() << "\n";
        return true;
    }

    void RunExperiment() {
        std::cout << "\n=========================================================\n";
        std::cout << "Starting P6 Dynamic Origin Experiment: " << config_.exp_id << " (" << config_.group_name << ")\n";
        std::cout << "Timing: Warm-up=" << config_.warmup_seconds << "s, Main=" << config_.main_seconds
                  << "s, Cool-down=" << config_.cooldown_seconds << "s\n";
        std::cout << "=========================================================\n";

        db_stats_->Reset();
        stop_requested_.store(false);

        auto exp_start_time = std::chrono::high_resolution_clock::now();

        std::atomic<bool> in_warmup(true);
        std::atomic<bool> in_main(false);
        std::atomic<bool> in_cooldown(false);
        std::atomic<uint64_t> op_cursor(0);

        // 1-second sampling monitor thread
        std::thread monitor_thread([&]() {
            int total_secs = config_.warmup_seconds + config_.main_seconds + config_.cooldown_seconds;
            for (int sec = 1; sec <= total_secs; ++sec) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                std::string phase = "warmup";
                if (sec <= config_.warmup_seconds) {
                    phase = "warmup";
                    in_warmup.store(true);
                    in_main.store(false);
                    in_cooldown.store(false);
                } else if (sec <= config_.warmup_seconds + config_.main_seconds) {
                    phase = "main";
                    in_warmup.store(false);
                    in_main.store(true);
                    in_cooldown.store(false);
                } else {
                    phase = "cooldown";
                    in_warmup.store(false);
                    in_main.store(false);
                    in_cooldown.store(true);
                }

                stats_.CaptureTimeSeriesSecond(sec, phase, db_.get(), db_stats_, config_.db_path);
            }
            stop_requested_.store(true);
        });

        // D2 Dynamic DeleteRange Dispatcher Thread
        std::thread d2_del_thread;
        if (config_.preload_mode == "d2_dynamic") {
            d2_del_thread = std::thread([&]() {
                // Wait for Main phase to start
                while (!in_main.load() && !stop_requested_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                const auto& del_ops = ref_model_.GetDeleteRangeOps();
                size_t total_ops_count = del_ops.size(); // 4,000
                int active_secs = 40; // uniformly spread across first 40 seconds of Main
                size_t ops_per_sec = total_ops_count / active_secs; // 100 per sec

                rocksdb::WriteOptions write_opts;
                size_t cur_idx = 0;

                std::cout << "[D2 Dispatcher] Starting uniform DeleteRange injection (100 ops/s for 40s) ...\n";

                for (int s = 0; s < active_secs && !stop_requested_.load(); ++s) {
                    auto t_sec_start = std::chrono::high_resolution_clock::now();
                    for (size_t i = 0; i < ops_per_sec && cur_idx < total_ops_count; ++i, ++cur_idx) {
                        const auto& op = del_ops[cur_idx];
                        std::string b_k = P6ReferenceModel::FormatKey(op.begin);
                        std::string e_k = P6ReferenceModel::FormatKey(op.end);

                        auto t0 = std::chrono::high_resolution_clock::now();
                        rocksdb::Status st = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), b_k, e_k);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                        if (st.ok()) {
                            ref_model_.ApplyDeleteRange(op.begin, op.end);
                            stats_.RecordDeleteRange(true, lat_ns);
                        }
                    }
                    auto t_sec_end = std::chrono::high_resolution_clock::now();
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_sec_end - t_sec_start).count();
                    if (elapsed_ms < 1000) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed_ms));
                    }
                }
                std::cout << "[D2 Dispatcher] Completed all 4000 DeleteRange injections at t=40s of Main phase.\n";
            });
        }

        // Frontend Worker Threads
        std::vector<std::thread> workers;
        workers.reserve(config_.num_threads);

        size_t total_fe_ops = frontend_ops_.size();

        for (int t = 0; t < config_.num_threads; ++t) {
            workers.emplace_back([&, t]() {
                rocksdb::ReadOptions read_opts;
                rocksdb::WriteOptions write_opts;

                while (!stop_requested_.load()) {
                    uint64_t idx = op_cursor.fetch_add(1, std::memory_order_relaxed);
                    const auto& fe_op = frontend_ops_[idx % total_fe_ops];
                    bool is_main = in_main.load();

                    if (fe_op.op_type == 2) {
                        // Put
                        std::string key_str = P6ReferenceModel::FormatKey(fe_op.key1);
                        std::string val_str = P6ReferenceModel::GenerateValue(fe_op.key1, 2, config_.value_size);
                        auto t0 = std::chrono::high_resolution_clock::now();
                        rocksdb::Status s = db_->Put(write_opts, key_str, val_str);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        if (s.ok()) {
                            stats_.RecordPut(is_main, lat_ns);
                        }
                    } else if (fe_op.op_type == 0) {
                        // Get
                        int cat = ref_model_.ClassifyGet(fe_op.key1);
                        std::string key_str = P6ReferenceModel::FormatKey(fe_op.key1);
                        std::string val_str;
                        auto t0 = std::chrono::high_resolution_clock::now();
                        db_->Get(read_opts, key_str, &val_str);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        stats_.RecordGet(is_main, cat, lat_ns);
                    } else if (fe_op.op_type == 1) {
                        // RangeScan
                        uint64_t begin_k = fe_op.key1;
                        uint64_t end_k = std::min(begin_k + fe_op.key2, config_.total_keys);
                        uint64_t span_req = end_k - begin_k;

                        std::string begin_key = P6ReferenceModel::FormatKey(begin_k);
                        std::string end_key = P6ReferenceModel::FormatKey(end_k);

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
                        stats_.RecordScan(is_main, lat_ns, span_req, returned_keys);
                    }
                }
            });
        }

        if (monitor_thread.joinable()) monitor_thread.join();
        if (d2_del_thread.joinable()) d2_del_thread.join();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        // Post-run validation & SHA-256 verification
        std::cout << "\n[Validation] Running Consistency Checks & Computing DB State SHA-256...\n";
        size_t ver_ok = 0, ver_nf = 0;
        std::string err_msg;
        bool sample_pass = ref_model_.SampleVerification(db_.get(), 10000, ver_ok, ver_nf, err_msg);

        uint64_t db_live = 0, db_bytes = 0;
        std::string sha256_hex;
        bool scan_pass = ref_model_.FullScanAndComputeSha256(db_.get(), db_live, db_bytes, sha256_hex, err_msg);

        std::cout << "[Validation Result] Live Keys=" << db_live << " (Expected 600000), Payload="
                  << (db_bytes / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "[State Checksum] SHA-256 = " << sha256_hex << "\n";
        std::cout << "[Verification] Sample: " << (sample_pass ? "PASS" : "FAIL")
                  << ", Full Scan: " << (scan_pass ? "PASS" : "FAIL") << "\n";

        stats_.AppendSummaryCsv(config_.summary_csv, config_.exp_id, config_.group_name, config_.desc,
                                config_.main_seconds, db_.get(), db_stats_, sha256_hex, ver_ok, ver_nf,
                                (sample_pass && scan_pass));

        stats_.DumpTimeSeriesCsv(config_.timeseries_csv, config_.exp_id, config_.group_name);
    }

private:
    P6Config config_;
    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::unique_ptr<rocksdb::DB> db_;
    P6ReferenceModel ref_model_;
    P6StatsCollector stats_;
    std::vector<FrontendOp> frontend_ops_;
    std::atomic<bool> stop_requested_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    P6Config config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
        else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
        else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
        else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
        else if (arg == "--timeseries_csv" && i + 1 < argc) config.timeseries_csv = argv[++i];
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
            else if (arg == "--timeseries_csv" && i + 1 < argc) config.timeseries_csv = argv[++i];
        }
    }

    P6Driver driver(config);
    if (!driver.LoadTraces()) return 1;
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    driver.RunExperiment();

    return 0;
}
