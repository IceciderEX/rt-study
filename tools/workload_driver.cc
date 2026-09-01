#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <memory>
#include <cassert>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"

#include "config.h"
#include "reference_model.h"
#include "stats_collector.h"

namespace study {

// Fast Zipfian Generator based on Gray's transformation
class ZipfianGenerator {
public:
    ZipfianGenerator(uint64_t min_val, uint64_t max_val, double theta = 0.99)
        : min_val_(min_val), max_val_(max_val), n_(max_val - min_val + 1), theta_(theta),
          zeta_n_(Zeta(n_, theta)),
          alpha_(1.0 / (1.0 - theta)),
          eta_((1.0 - std::pow(2.0 / n_, 1.0 - theta)) / (1.0 - Zeta(2, theta) / zeta_n_)) {}

    uint64_t Next(std::mt19937_64& rng) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double u = dist(rng);
        double uz = u * zeta_n_;

        if (uz < 1.0) return min_val_;
        if (uz < 1.0 + std::pow(0.5, theta_)) return min_val_ + 1;

        uint64_t v = min_val_ + static_cast<uint64_t>(n_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
        return std::min(v, max_val_);
    }

private:
    static double Zeta(uint64_t n, double theta) {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; ++i) {
            sum += (1.0 / std::pow(static_cast<double>(i), theta));
        }
        return sum;
    }

    uint64_t min_val_;
    uint64_t max_val_;
    uint64_t n_;
    double theta_;
    double zeta_n_;
    double alpha_;
    double eta_;
};

class WorkloadDriver {
public:
    WorkloadDriver(const BenchmarkConfig& config)
        : config_(config),
          ref_model_(config.total_keys, config.value_size) {}

    bool Run() {
        std::cout << "=========================================================\n"
                  << "Starting RocksDB Range Deletion Workload Benchmark\n"
                  << "Experiment ID: " << config_.exp_id << "\n"
                  << "Database Path: " << config_.db_path << "\n"
                  << "Total Keys: " << config_.total_keys << ", Value Size: " << config_.value_size << " B\n"
                  << "Threads: " << config_.num_threads << ", Total Ops: " << config_.total_ops << "\n"
                  << "Operation Ratios: Put=" << config_.put_ratio << "%, Get=" << config_.get_ratio
                  << "%, Scan=" << config_.scan_ratio << "%, DeleteRange=" << config_.delete_range_ratio << "%\n"
                  << "Key Distribution: " << config_.key_distribution << "\n"
                  << "=========================================================\n";

        // Step 1: Open Database
        if (!OpenDatabase()) {
            std::cerr << "[WorkloadDriver] Failed to open RocksDB at " << config_.db_path << std::endl;
            return false;
        }

        // Step 2: Preload Phase
        if (config_.populate_db) {
            if (!PreloadData()) {
                std::cerr << "[WorkloadDriver] Preload data failed!" << std::endl;
                CloseDatabase();
                return false;
            }
        }

        // Capture Start Engine Stats
        RocksDBStatsSnapshot start_snap = StatsCollector::CaptureRocksDBSnapshot(db_.get(), statistics_);

        // Step 3: Run Benchmark Workload
        auto start_time = std::chrono::high_resolution_clock::now();
        RunWorkers();
        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

        // Capture End Engine Stats
        RocksDBStatsSnapshot end_snap = StatsCollector::CaptureRocksDBSnapshot(db_.get(), statistics_);

        // Step 4: Verification Phase
        size_t verified_ok = 0, verified_notfound = 0;
        std::string sample_err;
        bool sample_ok = ref_model_.SampleVerification(db_.get(), config_.post_run_sample_keys, verified_ok, verified_notfound, sample_err);
        if (!sample_ok) {
            std::cerr << "\n[CRITICAL ERROR] Random Sample Verification FAILED: " << sample_err << std::endl;
        } else {
            std::cout << "\n[PASS] Random Sample Verification (" << config_.post_run_sample_keys
                      << " keys): Live OK=" << verified_ok << ", Deleted NotFound=" << verified_notfound << std::endl;
        }

        uint64_t db_live_keys = 0, ref_live_keys = 0, db_data_bytes = 0, ref_data_bytes = 0;
        std::string scan_err;
        bool full_scan_ok = true;
        if (config_.enable_full_scan_verification) {
            full_scan_ok = ref_model_.FullScanVerification(db_.get(), db_live_keys, ref_live_keys, db_data_bytes, ref_data_bytes, scan_err);
            if (!full_scan_ok) {
                std::cerr << "[CRITICAL ERROR] Full DB Scan Verification FAILED: " << scan_err << std::endl;
            } else {
                std::cout << "[PASS] Full DB Scan Verification: Total Live Keys=" << db_live_keys
                          << ", Total Logical Payload=" << (db_data_bytes / 1024.0 / 1024.0) << " MB" << std::endl;
            }
        }

        // Tombstone metrics
        uint64_t total_tombstones = 0, union_deleted_keys = 0;
        double union_coverage_ratio = 0.0, overlap_factor = 0.0;
        ref_model_.GetTombstoneStats(total_tombstones, union_deleted_keys, union_coverage_ratio, overlap_factor);
        std::cout << "--- Range Tombstone Spatial Statistics ---\n"
                  << "  Total DeleteRange Ops: " << total_tombstones << "\n"
                  << "  Union Deleted Keys: " << union_deleted_keys << "\n"
                  << "  Key Space Coverage: " << std::fixed << std::setprecision(2) << (union_coverage_ratio * 100.0) << " %\n"
                  << "  Tombstone Overlap Factor: " << std::setprecision(3) << overlap_factor << "x\n";

        // Step 5: Output Summary and CSV
        stats_.PrintSummary(elapsed_sec, start_snap, end_snap);
        stats_.WriteCSV(config_.summary_csv, config_.exp_id, elapsed_sec, start_snap, end_snap,
                        total_tombstones, union_deleted_keys, union_coverage_ratio, overlap_factor,
                        verified_ok, verified_notfound, full_scan_ok);

        CloseDatabase();
        return (sample_ok && full_scan_ok);
    }

private:
    bool OpenDatabase() {
        options_ = rocksdb::Options();
        options_.create_if_missing = true;
        options_.error_if_exists = false;

        statistics_ = rocksdb::CreateDBStatistics();
        options_.statistics = statistics_;

        options_.write_buffer_size = config_.write_buffer_size;
        options_.max_write_buffer_number = config_.max_write_buffer_number;
        options_.level0_file_num_compaction_trigger = config_.level0_file_num_compaction_trigger;
        options_.level0_slowdown_writes_trigger = config_.level0_slowdown_writes_trigger;
        options_.level0_stop_writes_trigger = config_.level0_stop_writes_trigger;
        options_.target_file_size_base = config_.target_file_size_base;
        options_.max_bytes_for_level_base = config_.max_bytes_for_level_base;
        options_.max_bytes_for_level_multiplier = config_.max_bytes_for_level_multiplier;
        options_.max_background_jobs = config_.max_background_jobs;

        rocksdb::BlockBasedTableOptions table_options;
        if (config_.block_cache_size > 0) {
            table_options.block_cache = rocksdb::NewLRUCache(config_.block_cache_size);
        }
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        rocksdb::Status s = rocksdb::DB::Open(options_, config_.db_path, &db_);
        if (!s.ok()) {
            std::cerr << "[WorkloadDriver] RocksDB Open failed: " << s.ToString() << std::endl;
            return false;
        }
        return true;
    }

    void CloseDatabase() {
        if (db_) {
            db_.reset();
        }
    }

    bool PreloadData() {
        std::cout << "[WorkloadDriver] Preloading " << config_.total_keys << " keys into DB..." << std::endl;
        auto t0 = std::chrono::high_resolution_clock::now();

        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = false;
        write_opts.sync = false;

        for (uint64_t i = 0; i < config_.total_keys; ++i) {
            std::string key = ReferenceModel::FormatKey(i);
            std::string val = ReferenceModel::GenerateValue(i, 1, config_.value_size);
            rocksdb::Status s = db_->Put(write_opts, key, val);
            if (!s.ok()) {
                std::cerr << "[WorkloadDriver] Preload Put failed at key " << i << ": " << s.ToString() << std::endl;
                return false;
            }
        }

        ref_model_.PopulateAll();
        auto t1 = std::chrono::high_resolution_clock::now();
        double preload_sec = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[WorkloadDriver] Preload complete in " << std::fixed << std::setprecision(2) << preload_sec << " s\n";
        return true;
    }

    void RunWorkers() {
        std::vector<std::thread> workers;
        std::atomic<uint64_t> completed_ops{0};
        std::atomic<bool> stop_flag{false};

        uint64_t ops_per_thread = config_.total_ops / config_.num_threads;

        for (uint32_t tid = 0; tid < config_.num_threads; ++tid) {
            workers.emplace_back([this, tid, ops_per_thread, &completed_ops, &stop_flag]() {
                WorkerLoop(tid, ops_per_thread, completed_ops, stop_flag);
            });
        }

        // Monitoring and Controlled Compaction loop
        uint64_t compact_trigger_op = (config_.total_ops * config_.compact_trigger_op_percent) / 100;
        bool compaction_triggered = false;

        while (completed_ops.load() < config_.total_ops && !stop_flag.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            uint64_t cur_ops = completed_ops.load();

            // Check P5 Controlled Compaction Trigger
            if (config_.trigger_compact_range && !compaction_triggered && cur_ops >= compact_trigger_op) {
                compaction_triggered = true;
                std::cout << "\n>>> [P5 TRIGGER] Controlled CompactRange() initiated at "
                          << cur_ops << " ops (" << config_.compact_trigger_op_percent << "% of workload) <<<\n";
                std::string begin_k = ReferenceModel::FormatKey(config_.compact_range_begin);
                std::string end_k = ReferenceModel::FormatKey(config_.compact_range_end);
                rocksdb::Slice s_begin(begin_k);
                rocksdb::Slice s_end(end_k);
                rocksdb::CompactRangeOptions cr_opts;
                auto ct0 = std::chrono::high_resolution_clock::now();
                rocksdb::Status cs = db_->CompactRange(cr_opts, &s_begin, &s_end);
                auto ct1 = std::chrono::high_resolution_clock::now();
                double c_sec = std::chrono::duration<double>(ct1 - ct0).count();
                std::cout << ">>> [P5 TRIGGER] CompactRange() completed in " << std::fixed << std::setprecision(3)
                          << c_sec << " s, status: " << cs.ToString() << " <<<\n\n";
            }
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }

    void WorkerLoop(uint32_t tid, uint64_t max_ops, std::atomic<uint64_t>& completed_ops, std::atomic<bool>& stop_flag) {
        std::mt19937_64 rng(config_.random_seed + tid * 10007);
        std::uniform_real_distribution<double> op_dist(0.0, 100.0);
        std::uniform_int_distribution<uint64_t> uni_key_dist(0, config_.total_keys - 1);
        ZipfianGenerator zipf_gen(0, config_.total_keys - 1, config_.zipf_theta);

        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = config_.disable_wal;
        write_opts.sync = config_.sync_writes;

        rocksdb::ReadOptions read_opts;

        double put_thresh = config_.put_ratio;
        double get_thresh = put_thresh + config_.get_ratio;
        double scan_thresh = get_thresh + config_.scan_ratio;

        // Hotspot range for P4 (e.g. first 20% of keys)
        uint64_t hotspot_size = config_.total_keys / 5;
        if (hotspot_size == 0) hotspot_size = 1;

        for (uint64_t op = 0; op < max_ops && !stop_flag.load(); ++op) {
            double r = op_dist(rng);

            if (r < put_thresh) {
                // Operation: Put
                uint64_t k = (config_.key_distribution == "zipfian") ? zipf_gen.Next(rng) : uni_key_dist(rng);
                std::string key = ReferenceModel::FormatKey(k);
                std::string val = ReferenceModel::GenerateValue(k, 2, config_.value_size);

                auto t0 = std::chrono::high_resolution_clock::now();
                rocksdb::Status s;
                if (config_.enable_online_verification) {
                    std::unique_lock<std::shared_mutex> lk(op_mutex_);
                    s = db_->Put(write_opts, key, val);
                    if (s.ok()) ref_model_.ApplyPut(k);
                } else {
                    s = db_->Put(write_opts, key, val);
                    if (s.ok()) ref_model_.ApplyPut(k);
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                stats_.RecordPut(lat_ns, s.ok());
            } else if (r < get_thresh) {
                // Operation: Get
                uint64_t k = (config_.key_distribution == "zipfian") ? zipf_gen.Next(rng) : uni_key_dist(rng);
                std::string key = ReferenceModel::FormatKey(k);
                std::string val;
                GetCategory cat = GET_CONTROL;

                auto t0 = std::chrono::high_resolution_clock::now();
                rocksdb::Status s;
                if (config_.enable_online_verification) {
                    std::shared_lock<std::shared_mutex> lk(op_mutex_);
                    cat = ref_model_.ClassifyGet(k);
                    s = db_->Get(read_opts, key, &val);
                    std::string err;
                    if (!ref_model_.VerifyGet(k, s, val, err)) {
                        std::cerr << "[Online Verification Failure] " << err << std::endl;
                    }
                } else {
                    cat = ref_model_.ClassifyGet(k);
                    s = db_->Get(read_opts, key, &val);
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                if (cat == GET_AFFECTED_LIVE) {
                    stats_.RecordGetAffectedLive(lat_ns, s.ok());
                } else if (cat == GET_DELETED) {
                    stats_.RecordGetDeleted(lat_ns, s.IsNotFound());
                } else {
                    stats_.RecordGetControl(lat_ns, s.ok());
                }
            } else if (r < scan_thresh) {
                // Operation: RangeScan
                uint64_t begin_k = (config_.key_distribution == "zipfian") ? zipf_gen.Next(rng) : uni_key_dist(rng);
                uint64_t end_k = std::min(begin_k + config_.scan_len, config_.total_keys);
                uint64_t scan_span = end_k - begin_k;

                std::string begin_key = ReferenceModel::FormatKey(begin_k);
                std::string end_key = ReferenceModel::FormatKey(end_k);

                std::vector<std::pair<uint64_t, std::string>> returned_kvs;

                auto t0 = std::chrono::high_resolution_clock::now();
                bool ok = false;
                if (config_.enable_online_verification) {
                    std::shared_lock<std::shared_mutex> lk(op_mutex_);
                    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
                    it->Seek(begin_key);
                    while (it->Valid()) {
                        std::string k_str = it->key().ToString();
                        if (k_str >= end_key) break;
                        uint64_t k_id = 0;
                        ReferenceModel::ParseKey(k_str, k_id);
                        returned_kvs.push_back({k_id, it->value().ToString()});
                        it->Next();
                    }
                    ok = it->status().ok();
                    if (ok) {
                        size_t exp_cnt = 0;
                        std::string err;
                        if (!ref_model_.VerifyRangeScan(begin_k, end_k, returned_kvs, exp_cnt, err)) {
                            std::cerr << "[Online Scan Verification Failure] " << err << std::endl;
                        }
                    }
                } else {
                    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
                    it->Seek(begin_key);
                    while (it->Valid()) {
                        std::string k_str = it->key().ToString();
                        if (k_str >= end_key) break;
                        uint64_t k_id = 0;
                        ReferenceModel::ParseKey(k_str, k_id);
                        returned_kvs.push_back({k_id, it->value().ToString()});
                        it->Next();
                    }
                    ok = it->status().ok();
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                stats_.RecordRangeScan(lat_ns, scan_span, returned_kvs.size(), ok);
            } else {
                // Operation: DeleteRange
                uint64_t begin_k = 0;

                // Pattern / Overlap Logic
                if (config_.hotspot_overlap_ratio > 0.5) {
                    // Hot deletion: target inside hotspot
                    std::uniform_int_distribution<uint64_t> hot_dist(0, hotspot_size - 1);
                    begin_k = hot_dist(rng);
                } else if (config_.hotspot_overlap_ratio > 0.0) {
                    // Medium deletion: partially overlapping hotspot
                    std::uniform_int_distribution<uint64_t> med_dist(hotspot_size / 2, hotspot_size + hotspot_size / 2);
                    begin_k = med_dist(rng);
                } else {
                    // Cold deletion: far from hotspot
                    std::uniform_int_distribution<uint64_t> cold_dist(hotspot_size, config_.total_keys - 1);
                    begin_k = cold_dist(rng);
                }

                uint64_t end_k = std::min(begin_k + config_.delete_range_len, config_.total_keys);
                if (begin_k >= end_k) end_k = begin_k + 1;
                uint64_t del_span = end_k - begin_k;

                std::string begin_key = ReferenceModel::FormatKey(begin_k);
                std::string end_key = ReferenceModel::FormatKey(end_k);

                auto t0 = std::chrono::high_resolution_clock::now();
                rocksdb::Status s;
                if (config_.enable_online_verification) {
                    std::unique_lock<std::shared_mutex> lk(op_mutex_);
                    s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), begin_key, end_key);
                    if (s.ok()) ref_model_.ApplyDeleteRange(begin_k, end_k, completed_ops.load());
                } else {
                    s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), begin_key, end_key);
                    if (s.ok()) ref_model_.ApplyDeleteRange(begin_k, end_k, completed_ops.load());
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

                stats_.RecordDeleteRange(lat_ns, del_span, s.ok());
            }

            completed_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    BenchmarkConfig config_;
    ReferenceModel ref_model_;
    StatsCollector stats_;
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> statistics_;
    std::shared_mutex op_mutex_;
};

} // namespace study

int main(int argc, char** argv) {
    study::BenchmarkConfig config;
    if (!study::ConfigParser::ParseCommandLine(argc, argv, config)) {
        return 1;
    }

    study::WorkloadDriver driver(config);
    bool success = driver.Run();
    return success ? 0 : 2;
}
