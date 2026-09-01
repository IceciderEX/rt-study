#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <filesystem>
#include <cassert>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"

#include "supp_config.h"
#include "supp_reference_model.h"
#include "supp_stats_collector.h"

using namespace study::supp;

class SuppWorkloadDriver {
public:
    SuppWorkloadDriver(const SuppConfig& config)
        : config_(config),
          ref_model_(config.total_keys, config.value_size),
          stop_requested_(false),
          is_reclaiming_(false) {}

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

        std::vector<TombstoneSegment> del_segs;
        ref_model_.SetupPreloadState(config_.preload_mode, del_segs);

        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = false;

        if (config_.preload_mode == "clean") {
            // Clean state: Only insert surviving keys
            uint64_t inserted = 0;
            for (uint64_t k = 0; k < config_.total_keys; ++k) {
                if (ref_model_.IsAlive(k)) {
                    std::string key = SuppReferenceModel::FormatKey(k);
                    std::string val = SuppReferenceModel::GenerateValue(k, 1, config_.value_size);
                    rocksdb::Status s = db_->Put(write_opts, key, val);
                    if (!s.ok()) {
                        std::cerr << "Preload Put failed on key " << key << ": " << s.ToString() << std::endl;
                        return false;
                    }
                    inserted++;
                }
            }
            std::cout << "[Preload] Clean mode inserted " << inserted << " surviving keys. Flushing to SST...\n";
            rocksdb::FlushOptions flush_opts;
            flush_opts.wait = true;
            rocksdb::Status fs = db_->Flush(flush_opts);
            if (!fs.ok()) {
                std::cerr << "Preload Flush failed: " << fs.ToString() << std::endl;
                return false;
            }
        } else {
            // Tombstone states: Insert full 1,000,000 keys -> Flush -> DeleteRange U -> Flush
            std::cout << "[Preload] Inserting full " << config_.total_keys << " keys...\n";
            for (uint64_t k = 0; k < config_.total_keys; ++k) {
                std::string key = SuppReferenceModel::FormatKey(k);
                std::string val = SuppReferenceModel::GenerateValue(k, 1, config_.value_size);
                rocksdb::Status s = db_->Put(write_opts, key, val);
                if (!s.ok()) {
                    std::cerr << "Preload Put failed: " << s.ToString() << std::endl;
                    return false;
                }
            }
            std::cout << "[Preload] Initial Flush to SST...\n";
            rocksdb::FlushOptions flush_opts;
            flush_opts.wait = true;
            rocksdb::Status fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;

            std::cout << "[Preload] Applying " << del_segs.size() << " DeleteRange operations for union U (40% coverage)...\n";
            for (const auto& seg : del_segs) {
                std::string b_key = SuppReferenceModel::FormatKey(seg.begin);
                std::string e_key = SuppReferenceModel::FormatKey(seg.end);
                rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), b_key, e_key);
                if (!s.ok()) {
                    std::cerr << "Preload DeleteRange failed: " << s.ToString() << std::endl;
                    return false;
                }
            }

            std::cout << "[Preload] Flushing range tombstones to SST...\n";
            fs = db_->Flush(flush_opts);
            if (!fs.ok()) return false;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Complete in " << std::fixed << std::setprecision(2) << preload_sec << " s. Live Keys in RefModel = "
                  << ref_model_.GetLiveKeyCount() << " (Expected 600000)\n";
        return true;
    }

    void RunBenchmark() {
        std::cout << "\n=========================================================\n";
        std::cout << "Starting Supplement Workload Benchmark: " << config_.exp_id << "\n";
        std::cout << "Mode: " << config_.preload_mode << ", Threads: " << config_.num_threads << ", Ops: " << config_.total_ops << "\n";
        std::cout << "Ratios: Put=" << config_.put_ratio << "%, Get=" << config_.get_ratio << "%, Scan=" << config_.scan_ratio << "%\n";
        std::cout << "=========================================================\n";

        // Reset measurement stats baseline
        db_stats_->Reset();

        std::atomic<uint64_t> completed_ops(0);
        stop_requested_.store(false);
        is_reclaiming_.store(false);

        auto start_time = std::chrono::high_resolution_clock::now();

        // Launch 1-second interval time-series monitor
        std::thread ts_thread([&]() {
            int sec = 0;
            while (!stop_requested_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                sec++;
                stats_.CaptureTimeSeriesSecond(sec, db_.get(), db_stats_, is_reclaiming_.load());

                // Check S3 Compaction trigger
                if (config_.trigger_reclaim && sec == static_cast<int>(config_.reclaim_trigger_second) && !is_reclaiming_.load()) {
                    std::thread reclaim_thread([&]() {
                        is_reclaiming_.store(true);
                        std::cout << "\n>>> [ControlThread] Triggering Async CompactRange ["
                                  << config_.reclaim_begin_key << ", " << config_.reclaim_end_key << ") at t=" << sec << "s <<<\n";
                        std::string b_k = SuppReferenceModel::FormatKey(config_.reclaim_begin_key);
                        std::string e_k = SuppReferenceModel::FormatKey(config_.reclaim_end_key);
                        rocksdb::Slice b_sl(b_k);
                        rocksdb::Slice e_sl(e_k);
                        rocksdb::CompactRangeOptions cr_opts;
                        db_->CompactRange(cr_opts, &b_sl, &e_sl);
                        std::cout << ">>> [ControlThread] CompactRange Completed <<<\n\n";
                        is_reclaiming_.store(false);
                    });
                    reclaim_thread.detach();
                }
            }
        });

        std::vector<std::thread> workers;
        workers.reserve(config_.num_threads);

        for (int t = 0; t < config_.num_threads; ++t) {
            workers.emplace_back(&SuppWorkloadDriver::WorkerLoop, this, t, std::ref(completed_ops), start_time);
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        stop_requested_.store(true);
        if (ts_thread.joinable()) ts_thread.join();

        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();

        // Print & Export
        stats_.PrintSummary(elapsed_sec, db_.get(), db_stats_);

        // Post-run validation
        std::cout << "\n[Validation] Running Post-Run Consistency Checks...\n";
        size_t ver_ok = 0, ver_nf = 0;
        std::string err_msg;
        bool sample_pass = ref_model_.SampleVerification(db_.get(), 10000, ver_ok, ver_nf, err_msg);
        if (!sample_pass) {
            std::cerr << "[CRITICAL ERROR] Random Sample Check Failed: " << err_msg << std::endl;
        } else {
            std::cout << "[PASS] Random Sample Verification (10000 keys): Live OK=" << ver_ok << ", Deleted NF=" << ver_nf << "\n";
        }

        uint64_t db_live = 0, ref_live = 0, db_bytes = 0, ref_bytes = 0;
        bool scan_pass = ref_model_.FullScanVerification(db_.get(), db_live, ref_live, db_bytes, ref_bytes, err_msg);
        if (!scan_pass) {
            std::cerr << "[CRITICAL ERROR] Full DB Scan Verification Failed: " << err_msg << std::endl;
        } else {
            std::cout << "[PASS] Full DB Scan Verification: Total Live Keys=" << db_live << " (Ref=" << ref_live
                      << "), Logical Payload=" << (db_bytes / (1024.0 * 1024.0)) << " MB\n";
        }

        stats_.AppendSummaryCsv(config_.summary_csv, config_.exp_id, config_.desc, elapsed_sec,
                                config_.total_keys, config_.value_size, config_.preload_mode,
                                db_.get(), db_stats_, ver_ok, ver_nf, (sample_pass && scan_pass));

        if (!config_.timeseries_csv.empty()) {
            stats_.DumpTimeSeriesCsv(config_.timeseries_csv, config_.exp_id);
            std::cout << "[TimeSeries] Time series exported to: " << config_.timeseries_csv << "\n";
        }
    }

private:
    void WorkerLoop(int thread_id, std::atomic<uint64_t>& completed_ops,
                    std::chrono::time_point<std::chrono::high_resolution_clock> start_time) {
        std::mt19937_64 rng(config_.random_seed + thread_id * 10007);
        std::uniform_real_distribution<double> ratio_dist(0.0, 100.0);
        std::uniform_int_distribution<uint64_t> uni_key_dist(0, config_.total_keys - 1);

        double put_thresh = config_.put_ratio;
        double get_thresh = put_thresh + config_.get_ratio;
        double scan_thresh = get_thresh + config_.scan_ratio;

        rocksdb::ReadOptions read_opts;
        rocksdb::WriteOptions write_opts;

        while (true) {
            uint64_t cur_op = completed_ops.fetch_add(1, std::memory_order_relaxed);
            if (config_.duration_seconds > 0) {
                auto now = std::chrono::high_resolution_clock::now();
                double el = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
                if (el >= config_.duration_seconds) break;
            } else {
                if (cur_op >= config_.total_ops) break;
            }

            double r = ratio_dist(rng);
            uint64_t key_id = uni_key_dist(rng);

            if (r < put_thresh) {
                // Put
                std::string key_str = SuppReferenceModel::FormatKey(key_id);
                std::string val_str = SuppReferenceModel::GenerateValue(key_id, 2, config_.value_size);
                auto t0 = std::chrono::high_resolution_clock::now();
                rocksdb::Status s = db_->Put(write_opts, key_str, val_str);
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                if (s.ok()) {
                    ref_model_.ApplyPut(key_id);
                    stats_.RecordPut(lat_ns);
                }
            } else if (r < get_thresh) {
                // Get
                int cat = ref_model_.ClassifyGet(key_id);
                std::string key_str = SuppReferenceModel::FormatKey(key_id);
                std::string val_str;
                auto t0 = std::chrono::high_resolution_clock::now();
                db_->Get(read_opts, key_str, &val_str);
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                stats_.RecordGet(cat, lat_ns);
            } else if (r < scan_thresh) {
                // RangeScan
                uint64_t begin_k = key_id;
                uint64_t end_k = std::min(begin_k + config_.scan_len, config_.total_keys);
                uint64_t span_req = end_k - begin_k;

                std::string begin_key = SuppReferenceModel::FormatKey(begin_k);
                std::string end_key = SuppReferenceModel::FormatKey(end_k);

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
            }
        }
    }

    SuppConfig config_;
    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::unique_ptr<rocksdb::DB> db_;
    SuppReferenceModel ref_model_;
    SuppStatsCollector stats_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> is_reclaiming_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    SuppConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--exp_id" && i + 1 < argc) {
            config.exp_id = argv[++i];
        } else if (arg == "--db_path" && i + 1 < argc) {
            config.db_path = argv[++i];
        } else if (arg == "--result_dir" && i + 1 < argc) {
            config.result_dir = argv[++i];
        } else if (arg == "--summary_csv" && i + 1 < argc) {
            config.summary_csv = argv[++i];
        } else if (arg == "--timeseries_csv" && i + 1 < argc) {
            config.timeseries_csv = argv[++i];
        }
    }

    if (!config_file.empty()) {
        if (!config.ParseIni(config_file)) {
            std::cerr << "Failed to parse config file: " << config_file << std::endl;
            return 1;
        }
        // Re-apply CLI overrides if provided
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
            else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
            else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
            else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
            else if (arg == "--timeseries_csv" && i + 1 < argc) config.timeseries_csv = argv[++i];
        }
    }

    SuppWorkloadDriver driver(config);
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    driver.RunBenchmark();

    return 0;
}
