#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <barrier>
#include <cassert>
#include <memory>
#include <unordered_set>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/write_batch.h"

#include "formal_config.h"
#include "manifest_parser.h"
#include "thread_local_histogram.h"
#include "formal_event_listener.h"
#include "worker_state_model.h"
#include "kv_verifier.h"
#include "rtp_mc_controller.h"

using namespace study::formal;

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

struct PhaseStatsAgg {
    std::string phase_name;
    double elapsed_sec = 0.0;
    uint64_t completed_ops = 0;
    double true_phase_iops = 0.0;
    double get_live_p50 = 0, get_live_p90 = 0, get_live_p95 = 0, get_live_p99 = 0, get_live_p999 = 0;
    double get_del_p50 = 0, get_del_p90 = 0, get_del_p95 = 0, get_del_p99 = 0, get_del_p999 = 0;
    double scan_p50 = 0, scan_p90 = 0, scan_p95 = 0, scan_p99 = 0, scan_p999 = 0;
    double scan_intersect_p99 = 0;
    double scan_non_intersect_p99 = 0;
    double put_p50 = 0, put_p90 = 0, put_p95 = 0, put_p99 = 0, put_p999 = 0;
    uint64_t scan_total_keys_found = 0;
    double scan_us_per_key = 0.0;
    uint64_t scan_limit_truncated_count = 0;
};

class FormalDriver {
public:
    FormalDriver(const FormalConfig& config)
        : config_(config),
          num_workers_(config.num_workers > 0 ? config.num_workers : 8),
          experiment_failed_(false)
    {
        worker_ranges_.resize(num_workers_);
        uint64_t base_k = config_.total_keys / num_workers_;
        uint64_t rem = config_.total_keys % num_workers_;

        uint64_t curr = 0;
        for (int w = 0; w < num_workers_; ++w) {
            uint64_t count = base_k + (w == num_workers_ - 1 ? rem : 0);
            worker_ranges_[w] = {curr, curr + count};
            worker_models_.push_back(std::make_unique<WorkerStateModel>(curr, curr + count, config_.value_size));
            curr += count;
        }

        event_listener_ = std::make_shared<FormalEventListener>();
    }

    bool LoadAllWorkerTraces() {
        TraceManifest manifest;
        std::string manifest_err;
        if (!TraceManifest::ParseAndValidate(config_.trace_dir, config_.total_keys, num_workers_, config_.value_size, manifest, manifest_err)) {
            std::cerr << "[FormalDriver AUDIT ERROR] Trace Manifest validation failed:\n  " << manifest_err << std::endl;
            return false;
        }

        std::cout << "[FormalDriver] Trace Manifest Audit Passed: Workload=" << manifest.workload_id 
                  << ", GeneratorCommit=" << manifest.generator_commit 
                  << ", ValueSize=" << manifest.value_size
                  << ", TotalOps=" << manifest.total_ops_count << "\n";

        std::unordered_set<uint32_t> global_seen_op_ids;
        global_seen_op_ids.reserve(manifest.total_ops_count);

        worker_traces_.resize(num_workers_);
        for (int w = 0; w < num_workers_; ++w) {
            worker_traces_[w].resize(3); // Phase A, Phase B, Phase C
            std::vector<std::string> p_names = {"phase_a", "phase_b", "phase_c"};
            uint64_t w_start = worker_ranges_[w].first;
            uint64_t w_end = worker_ranges_[w].second;

            for (int p = 0; p < 3; ++p) {
                char fname[64];
                snprintf(fname, sizeof(fname), "%s-worker-%02d.bin", p_names[p].c_str(), w);
                std::filesystem::path trace_file = std::filesystem::path(config_.trace_dir) / fname;

                if (!std::filesystem::exists(trace_file)) {
                    std::cerr << "[FormalDriver AUDIT ERROR] Trace file not found: " << trace_file << std::endl;
                    return false;
                }

                auto f_size = std::filesystem::file_size(trace_file);
                if (f_size % sizeof(FormalTraceRecord) != 0) {
                    std::cerr << "[FormalDriver AUDIT ERROR] Trace file size " << f_size << " not aligned to 24 bytes: " << trace_file << std::endl;
                    return false;
                }

                size_t num_records = f_size / sizeof(FormalTraceRecord);
                worker_traces_[w][p].resize(num_records);

                std::ifstream fin(trace_file, std::ios::binary);
                if (!fin.read(reinterpret_cast<char*>(worker_traces_[w][p].data()), f_size)) {
                    std::cerr << "[FormalDriver ERROR] Failed reading: " << trace_file << std::endl;
                    return false;
                }

                // Strict Record-Level Admission Audit
                for (size_t r = 0; r < num_records; ++r) {
                    const auto& rec = worker_traces_[w][p][r];
                    if (rec.phase_id != p) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Phase ID mismatch in " << fname << " record " << r 
                                  << " (expected " << p << ", got " << (int)rec.phase_id << ")" << std::endl;
                        return false;
                    }
                    if (rec.op_type > 4) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Invalid op_type " << (int)rec.op_type << " in " << fname << std::endl;
                        return false;
                    }
                    if (rec.scan_mode > 1) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Invalid scan_mode " << (int)rec.scan_mode << " in " << fname << std::endl;
                        return false;
                    }
                    if (!global_seen_op_ids.insert(rec.op_id).second) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Duplicate op_id " << rec.op_id << " detected in " << fname << std::endl;
                        return false;
                    }
                    if (rec.key1 < w_start || rec.key1 >= w_end) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Key1 " << rec.key1 << " out of partition [" 
                                  << w_start << ", " << w_end << ") in " << fname << std::endl;
                        return false;
                    }
                    if (rec.op_type == 3) { // DeleteRange
                        if (rec.key1 >= rec.key2 || rec.key2 > w_end) {
                            std::cerr << "[FormalDriver AUDIT ERROR] DeleteRange [" << rec.key1 << ", " << rec.key2 
                                      << ") exceeds partition bounds [" << w_start << ", " << w_end << ") in " << fname << std::endl;
                            return false;
                        }
                    }
                    if (rec.op_type == 1 && rec.scan_mode == 0) { // SCAN_RANGE
                        if (rec.key1 >= rec.key2 || rec.key2 > w_end) {
                            std::cerr << "[FormalDriver AUDIT ERROR] SCAN_RANGE [" << rec.key1 << ", " << rec.key2 
                                      << ") exceeds partition bounds [" << w_start << ", " << w_end << ") in " << fname << std::endl;
                            return false;
                        }
                    }
                }
            }
        }
        std::cout << "[FormalDriver] Passed 100% Comprehensive Admission Audit for 24 payload traces (Unique OpIds: " 
                  << global_seen_op_ids.size() << ") from " << config_.trace_dir << "\n";
        return true;
    }

    bool InitializeDB() {
        if (std::filesystem::exists(config_.db_path)) {
            std::cerr << "[FormalDriver ERROR] Target DB directory already exists! Refusing to run on dirty DB: " 
                      << config_.db_path << std::endl;
            return false;
        }

        std::filesystem::create_directories(config_.db_path);
        std::filesystem::create_directories(config_.result_dir);

        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true; // Reject existing dirty DB
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
        options.memtable_op_scan_flush_trigger = 0; // Fixed 0 to eliminate confounding
        options.enable_range_tombstone_controller =
            config_.enable_range_tombstone_controller;
        options.range_tombstone_controller_observe_only =
            config_.range_tombstone_controller_observe_only;
        options.range_tombstone_controller_min_range_deletions =
            config_.range_tombstone_controller_min_range_deletions;
        options.range_tombstone_controller_min_memtable_bytes =
            config_.range_tombstone_controller_min_memtable_bytes;
        options.range_tombstone_controller_cooldown_micros =
            config_.range_tombstone_controller_cooldown_micros;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_size = 4 * 1024;
        table_options.block_cache = rocksdb::NewLRUCache(config_.block_cache_size);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        db_stats_ = rocksdb::CreateDBStatistics();
        options.statistics = db_stats_;

        options.listeners.push_back(event_listener_);
        options_ = options;

        rocksdb::Status status = rocksdb::DB::Open(options_, config_.db_path, &db_);
        if (!status.ok()) {
            std::cerr << "[FormalDriver ERROR] RocksDB::Open failed: " << status.ToString() << std::endl;
            return false;
        }
        return true;
    }

    bool PreloadDatabase() {
        std::cout << "[Preload] Preloading " << config_.total_keys << " keys (Value size=" 
                  << config_.value_size << " B) using " << num_workers_ << " concurrent workers...\n";
        auto t0 = std::chrono::steady_clock::now();

        std::vector<std::thread> workers;
        std::atomic<bool> preload_failed(false);

        for (int w = 0; w < num_workers_; ++w) {
            uint64_t start_k = worker_ranges_[w].first;
            uint64_t end_k = worker_ranges_[w].second;

            workers.emplace_back([&, start_k, end_k]() {
                rocksdb::WriteOptions write_opts;
                write_opts.disableWAL = false;
                const size_t kBatchSize = 4096;

                for (uint64_t k = start_k; k < end_k && !preload_failed.load(); k += kBatchSize) {
                    rocksdb::WriteBatch batch;
                    uint64_t batch_end = std::min(k + kBatchSize, end_k);
                    for (uint64_t i = k; i < batch_end; ++i) {
                        std::string key = WorkerStateModel::FormatKey(i);
                        std::string val = WorkerStateModel::GenerateValue(i, 1, config_.value_size);
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
        rocksdb::Status ws = db_->WaitForCompact(wait_opts);
        if (!ws.ok()) {
            std::cerr << "[Preload ERROR] WaitForCompact failed: " << ws.ToString() << std::endl;
            return false;
        }

        auto t1 = std::chrono::steady_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Preload completed and settled in " << std::fixed << std::setprecision(2) << preload_sec << " s.\n";

        return true;
    }

    bool ExecuteExperiment() {
        std::cout << "\n=========================================================\n";
        std::cout << "  Starting Formal V2 Thesis Experiment: " << config_.exp_id << "\n";
        std::cout << "  Group: " << config_.group_name << ", Threshold: " << config_.memtable_max_range_deletions << "\n";
        std::cout << "  Range Tombstone Controller: "
                  << (config_.enable_range_tombstone_controller ? "enabled" : "disabled")
                  << ", mode="
                  << (config_.range_tombstone_controller_observe_only ? "observe" : "active")
                  << ", min_tombstones="
                  << config_.range_tombstone_controller_min_range_deletions
                  << ", min_memtable_bytes="
                  << config_.range_tombstone_controller_min_memtable_bytes
                  << ", cooldown_us="
                  << config_.range_tombstone_controller_cooldown_micros << "\n";
        std::cout << "  Workers: " << num_workers_ << ", Key Space: " << config_.total_keys << "\n";
        std::cout << "=========================================================\n";

        experiment_failed_.store(false);
        db_stats_->Reset();

        // Instantiate Double-buffered bundles for RTP-MC V2
        std::vector<std::unique_ptr<WorkerHistogramBundle>> worker_rtp_bundles;
        for (int w = 0; w < num_workers_; ++w) {
            worker_rtp_bundles.push_back(std::make_unique<WorkerHistogramBundle>());
        }

        auto rtp_controller = std::make_unique<RtpMcController>(
            db_.get(), config_, config_.exp_id, worker_rtp_bundles, event_listener_);
        event_listener_->SetFlushCompletedCallback([&](uint64_t out_bytes) {
            rtp_controller->NotifyFlushCompleted(out_bytes);
        });
        rtp_controller->Start();

        // Switch EventListener stage to FOREGROUND
        event_listener_->StartForegroundExperiment();
        auto fg_wallclock_t0 = std::chrono::steady_clock::now();

        // Barrier synchronization pairs across 8 workers + 1 coordinator thread = 9
        std::vector<std::unique_ptr<std::barrier<>>> phase_start_barriers;
        std::vector<std::unique_ptr<std::barrier<>>> phase_end_barriers;
        for (int p = 0; p < 3; ++p) {
            phase_start_barriers.push_back(std::make_unique<std::barrier<>>(num_workers_ + 1));
            phase_end_barriers.push_back(std::make_unique<std::barrier<>>(num_workers_ + 1));
        }

        struct SubphaseTracking {
            ThreadLocalHistogram hist_get_live;
            ThreadLocalHistogram hist_get_del;
            ThreadLocalHistogram hist_scan;
            ThreadLocalHistogram hist_scan_intersect;
            ThreadLocalHistogram hist_scan_non_intersect;
            ThreadLocalHistogram hist_put;
            uint64_t scan_keys_found = 0;
            uint64_t scan_limit_truncated_count = 0;
            uint64_t completed_ops = 0;
        };

        struct WorkerThreadStats {
            ThreadLocalHistogram hist_get_live;
            ThreadLocalHistogram hist_get_del;
            ThreadLocalHistogram hist_scan;
            ThreadLocalHistogram hist_scan_intersect;
            ThreadLocalHistogram hist_scan_non_intersect;
            ThreadLocalHistogram hist_put;
            ThreadLocalHistogram hist_del;
            uint64_t scan_keys_found = 0;
            uint64_t scan_limit_truncated_count = 0;
            uint64_t completed_ops = 0;
            uint64_t db_api_calls = 0;
            uint64_t logical_put_bytes = 0;
            SubphaseTracking sub_inj;
            SubphaseTracking sub_post;
        };

        std::vector<std::vector<WorkerThreadStats>> worker_phase_stats(num_workers_);
        for (int w = 0; w < num_workers_; ++w) {
            worker_phase_stats[w].resize(3);
        }

        std::vector<std::thread> workers;
        workers.reserve(num_workers_);
        static std::atomic<uint64_t> global_del_ops{0};

        for (int w = 0; w < num_workers_; ++w) {
            uint64_t w_end = worker_ranges_[w].second;

            workers.emplace_back([&, w, w_end]() {
                rocksdb::ReadOptions read_opts;
                rocksdb::WriteOptions write_opts;
                auto& model = *worker_models_[w];
                std::string w_end_key = WorkerStateModel::FormatKey(w_end);
                rocksdb::Slice w_end_slice(w_end_key);

                for (int p = 0; p < 3; ++p) {
                    const auto& trace = worker_traces_[w][p];
                    auto& stats = worker_phase_stats[w][p];

                    // 1. Wait for coordinator start barrier
                    phase_start_barriers[p]->arrive_and_wait();

                    for (const auto& op : trace) {
                        if (experiment_failed_.load()) break;

                        if (op.op_type == 0) { // Get
                            // Prep work outside timing envelope
                            ExpectedState exp_state = model.ClassifyGet(op.key1);
                            std::string key = WorkerStateModel::FormatKey(op.key1);
                            std::string val;

                            // Pure DB Call Timing Envelope
                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->Get(read_opts, key, &val);
                            auto t_end = std::chrono::steady_clock::now();

                            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                            if ((op.flags & 1) != 0) {
                                stats.hist_get_del.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_get_del.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_get_del.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_get_del.Record(lat_ns);
                            } else {
                                stats.hist_get_live.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_get_live.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_get_live.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_get_live.Record(lat_ns);
                            }

                            // Post-timing strict validation
                            if (exp_state == ExpectedState::kExpectedLive) {
                                if (!s.ok()) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Expected Live key " 
                                              << key << " NOT FOUND! Status: " << s.ToString() << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                                uint32_t exp_ver = model.GetKeyVersion(op.key1);
                                std::string exp_val = WorkerStateModel::GenerateValue(op.key1, exp_ver, config_.value_size);
                                if (val != exp_val) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Value corruption for key " << key << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                            } else {
                                if (!s.IsNotFound()) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Expected Deleted key " 
                                              << key << " was FOUND! Status: " << s.ToString() << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                            }
                            stats.db_api_calls++;
                        } else if (op.op_type == 1) { // Scan (Full Scan API Lifecycle: NewIterator + Seek + Next)
                            // Prep work outside timing envelope
                            std::string start_key = WorkerStateModel::FormatKey(op.key1);
                            uint64_t keys_found = 0;
                            uint64_t limit_k = op.key2;
                            std::unique_ptr<rocksdb::Iterator> it;

                            // Pure DB Scan API Timing Envelope (Includes NewIterator + Seek + Iteration)
                            auto t_start = std::chrono::steady_clock::now();
                            it.reset(db_->NewIterator(read_opts));
                            it->Seek(start_key);

                            if (op.scan_mode == 0) { // SCAN_RANGE (Strictly bounded by key2 and w_end)
                                std::string end_key = WorkerStateModel::FormatKey(op.key2);
                                rocksdb::Slice end_slice(end_key);
                                while (it->Valid() && it->key().compare(end_slice) < 0 && it->key().compare(w_end_slice) < 0) {
                                    keys_found++;
                                    it->Next();
                                }
                            } else { // SCAN_LIMIT (Strictly bounded by limit_k and w_end)
                                while (it->Valid() && keys_found < limit_k && it->key().compare(w_end_slice) < 0) {
                                    keys_found++;
                                    it->Next();
                                }
                            }
                            auto t_end = std::chrono::steady_clock::now();

                            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                            stats.hist_scan.Record(lat_ns);
                            worker_rtp_bundles[w]->hist_scan.Record(lat_ns);
                            stats.scan_keys_found += keys_found;

                            if ((op.flags & 1) != 0) {
                                stats.hist_scan_intersect.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_scan_intersect.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_scan_intersect.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_scan_intersect.Record(lat_ns);
                            } else {
                                stats.hist_scan_non_intersect.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_scan_non_intersect.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_scan_non_intersect.Record(lat_ns);
                            }

                            if (op.flags & 2) {
                                stats.sub_inj.hist_scan.Record(lat_ns);
                                stats.sub_inj.scan_keys_found += keys_found;
                            }
                            if (op.flags & 4) {
                                stats.sub_post.hist_scan.Record(lat_ns);
                                stats.sub_post.scan_keys_found += keys_found;
                            }

                            // Post-timing validation
                            if (!it->status().ok()) {
                                std::cerr << "[Worker " << w << " ERROR] Scan iterator error: " << it->status().ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }
                            if (op.scan_mode == 1 && keys_found < limit_k) {
                                stats.scan_limit_truncated_count++;
                                if (op.flags & 2) stats.sub_inj.scan_limit_truncated_count++;
                                if (op.flags & 4) stats.sub_post.scan_limit_truncated_count++;
                            }
                            stats.db_api_calls++;
                        } else if (op.op_type == 2) { // Put
                            // Prep work outside timing envelope
                            uint32_t next_ver = model.GetKeyVersion(op.key1) + 1;
                            std::string key = WorkerStateModel::FormatKey(op.key1);
                            std::string val = WorkerStateModel::GenerateValue(op.key1, next_ver, config_.value_size);

                            // Pure DB Call Timing Envelope
                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->Put(write_opts, key, val);
                            auto t_end = std::chrono::steady_clock::now();

                            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                            stats.hist_put.Record(lat_ns);
                            worker_rtp_bundles[w]->hist_put.Record(lat_ns);
                            if (op.flags & 2) stats.sub_inj.hist_put.Record(lat_ns);
                            if (op.flags & 4) stats.sub_post.hist_put.Record(lat_ns);

                            // Post-timing update & validation
                            if (!s.ok()) {
                                std::cerr << "[Worker " << w << " ERROR] Put failed: " << s.ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }
                            model.ApplyPut(op.key1);
                            stats.logical_put_bytes += config_.value_size;
                            stats.db_api_calls++;
                        } else if (op.op_type == 3) { // DeleteRange
                            // Prep work outside timing envelope
                            std::string start_key = WorkerStateModel::FormatKey(op.key1);
                            std::string end_key = WorkerStateModel::FormatKey(op.key2);

                            // Pure DB Call Timing Envelope
                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), start_key, end_key);
                            auto t_end = std::chrono::steady_clock::now();

                            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                            stats.hist_del.Record(lat_ns);

                            // Post-timing update & validation
                            if (!s.ok()) {
                                std::cerr << "[Worker " << w << " ERROR] DeleteRange failed: " << s.ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }
                            model.ApplyDeleteRange(op.key1, op.key2);
                            stats.db_api_calls++;

                            if ((global_del_ops.fetch_add(1, std::memory_order_relaxed) + 1) % config_.range_del_checkpoint == 0) {
                                rtp_controller->NotifyDeleteRangeCheckpoint();
                            }
                        } else if (op.op_type == 4) { // No-op
                            // Functional Clean Baseline
                        }

                        stats.completed_ops++;
                        if (op.flags & 2) stats.sub_inj.completed_ops++;
                        if (op.flags & 4) stats.sub_post.completed_ops++;
                    }

                    // 2. Arrive at phase end barrier and wait for coordinator & all workers
                    phase_end_barriers[p]->arrive_and_wait();
                }
            });
        }

        // Coordinator Thread: Controls Phase Timing with Exact Start/End Barrier Pairs
        std::vector<PhaseStatsAgg> phase_results;
        std::vector<std::string> phase_names = {
            "Phase A (Read Sensitive)",
            "Phase B (Write Burst)",
            "Phase C (Read Recovery)"
        };

        double sum_phase_active_sec = 0.0;
        double oracle_flush_wait_sec = 0.0;

        for (int p = 0; p < 3; ++p) {
            std::cout << "[Coordinator] Phase " << p << " (" << phase_names[p] << ") released at exact T0...\n";
            rtp_controller->SetCurrentPhase(p);
            auto p_t0 = std::chrono::steady_clock::now();

            // Release all workers simultaneously
            phase_start_barriers[p]->arrive_and_wait();

            // Wait for all workers to finish this phase
            phase_end_barriers[p]->arrive_and_wait();

            auto p_t1 = std::chrono::steady_clock::now();
            double p_sec = std::chrono::duration_cast<std::chrono::duration<double>>(p_t1 - p_t0).count();
            sum_phase_active_sec += p_sec;

            // Aggregate Phase Stats
            PhaseStatsAgg agg;
            agg.phase_name = phase_names[p];
            agg.elapsed_sec = p_sec;

            ThreadLocalHistogram p_hist_get_live;
            ThreadLocalHistogram p_hist_get_del;
            ThreadLocalHistogram p_hist_scan;
            ThreadLocalHistogram p_hist_scan_intersect;
            ThreadLocalHistogram p_hist_scan_non_intersect;
            ThreadLocalHistogram p_hist_put;

            for (int w = 0; w < num_workers_; ++w) {
                const auto& ws = worker_phase_stats[w][p];
                agg.completed_ops += ws.completed_ops;
                agg.scan_total_keys_found += ws.scan_keys_found;
                agg.scan_limit_truncated_count += ws.scan_limit_truncated_count;

                p_hist_get_live.MergeFrom(ws.hist_get_live);
                p_hist_get_del.MergeFrom(ws.hist_get_del);
                p_hist_scan.MergeFrom(ws.hist_scan);
                p_hist_scan_intersect.MergeFrom(ws.hist_scan_intersect);
                p_hist_scan_non_intersect.MergeFrom(ws.hist_scan_non_intersect);
                p_hist_put.MergeFrom(ws.hist_put);
            }

            agg.true_phase_iops = (p_sec > 0) ? (agg.completed_ops / p_sec) : 0.0;

            double dummy_mean, dummy_max, dummy_50, dummy_90, dummy_95, dummy_999;
            p_hist_get_live.ComputeQuantiles(agg.get_live_p50, agg.get_live_p90, agg.get_live_p95, agg.get_live_p99, agg.get_live_p999, dummy_mean, dummy_max);
            p_hist_get_del.ComputeQuantiles(agg.get_del_p50, agg.get_del_p90, agg.get_del_p95, agg.get_del_p99, agg.get_del_p999, dummy_mean, dummy_max);
            p_hist_scan.ComputeQuantiles(agg.scan_p50, agg.scan_p90, agg.scan_p95, agg.scan_p99, agg.scan_p999, dummy_mean, dummy_max);
            p_hist_scan_intersect.ComputeQuantiles(dummy_50, dummy_90, dummy_95, agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
            p_hist_scan_non_intersect.ComputeQuantiles(dummy_50, dummy_90, dummy_95, agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
            p_hist_put.ComputeQuantiles(agg.put_p50, agg.put_p90, agg.put_p95, agg.put_p99, agg.put_p999, dummy_mean, dummy_max);

            if (agg.scan_total_keys_found > 0) {
                agg.scan_us_per_key = (p_hist_scan.GetSumNs() / 1000.0) / agg.scan_total_keys_found;
            }

            phase_results.push_back(agg);
            std::cout << "[Coordinator] Phase " << p << " completed in " << std::fixed << std::setprecision(4) 
                      << p_sec << " s, True IOPS = " << std::setprecision(2) << agg.true_phase_iops << "\n";

            // If Phase B, also aggregate Phase B-Inject and Phase B-PostBurst
            if (p == 1) {
                PhaseStatsAgg inj_agg;
                inj_agg.phase_name = "Phase B-Inject (20% Window)";
                inj_agg.elapsed_sec = p_sec * 0.20; // 20% quota

                PhaseStatsAgg post_agg;
                post_agg.phase_name = "Phase B-PostBurst (80% Window)";
                post_agg.elapsed_sec = p_sec * 0.80; // 80% quota

                ThreadLocalHistogram inj_hist_get_live, inj_hist_get_del, inj_hist_scan, inj_hist_scan_int, inj_hist_scan_non, inj_hist_put;
                ThreadLocalHistogram post_hist_get_live, post_hist_get_del, post_hist_scan, post_hist_scan_int, post_hist_scan_non, post_hist_put;

                for (int w = 0; w < num_workers_; ++w) {
                    const auto& ws = worker_phase_stats[w][1];
                    inj_agg.completed_ops += ws.sub_inj.completed_ops;
                    inj_agg.scan_total_keys_found += ws.sub_inj.scan_keys_found;
                    inj_agg.scan_limit_truncated_count += ws.sub_inj.scan_limit_truncated_count;
                    inj_hist_get_live.MergeFrom(ws.sub_inj.hist_get_live);
                    inj_hist_get_del.MergeFrom(ws.sub_inj.hist_get_del);
                    inj_hist_scan.MergeFrom(ws.sub_inj.hist_scan);
                    inj_hist_scan_int.MergeFrom(ws.sub_inj.hist_scan_intersect);
                    inj_hist_scan_non.MergeFrom(ws.sub_inj.hist_scan_non_intersect);
                    inj_hist_put.MergeFrom(ws.sub_inj.hist_put);

                    post_agg.completed_ops += ws.sub_post.completed_ops;
                    post_agg.scan_total_keys_found += ws.sub_post.scan_keys_found;
                    post_agg.scan_limit_truncated_count += ws.sub_post.scan_limit_truncated_count;
                    post_hist_get_live.MergeFrom(ws.sub_post.hist_get_live);
                    post_hist_get_del.MergeFrom(ws.sub_post.hist_get_del);
                    post_hist_scan.MergeFrom(ws.sub_post.hist_scan);
                    post_hist_scan_int.MergeFrom(ws.sub_post.hist_scan_intersect);
                    post_hist_scan_non.MergeFrom(ws.sub_post.hist_scan_non_intersect);
                    post_hist_put.MergeFrom(ws.sub_post.hist_put);
                }

                if (inj_agg.elapsed_sec > 0) inj_agg.true_phase_iops = inj_agg.completed_ops / inj_agg.elapsed_sec;
                inj_hist_get_live.ComputeQuantiles(inj_agg.get_live_p50, inj_agg.get_live_p90, inj_agg.get_live_p95, inj_agg.get_live_p99, inj_agg.get_live_p999, dummy_mean, dummy_max);
                inj_hist_get_del.ComputeQuantiles(inj_agg.get_del_p50, inj_agg.get_del_p90, inj_agg.get_del_p95, inj_agg.get_del_p99, inj_agg.get_del_p999, dummy_mean, dummy_max);
                inj_hist_scan.ComputeQuantiles(inj_agg.scan_p50, inj_agg.scan_p90, inj_agg.scan_p95, inj_agg.scan_p99, inj_agg.scan_p999, dummy_mean, dummy_max);
                inj_hist_scan_int.ComputeQuantiles(dummy_50, dummy_90, dummy_95, inj_agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
                inj_hist_scan_non.ComputeQuantiles(dummy_50, dummy_90, dummy_95, inj_agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
                inj_hist_put.ComputeQuantiles(inj_agg.put_p50, inj_agg.put_p90, inj_agg.put_p95, inj_agg.put_p99, inj_agg.put_p999, dummy_mean, dummy_max);
                if (inj_agg.scan_total_keys_found > 0) inj_agg.scan_us_per_key = (inj_hist_scan.GetSumNs() / 1000.0) / inj_agg.scan_total_keys_found;

                if (post_agg.elapsed_sec > 0) post_agg.true_phase_iops = post_agg.completed_ops / post_agg.elapsed_sec;
                post_hist_get_live.ComputeQuantiles(post_agg.get_live_p50, post_agg.get_live_p90, post_agg.get_live_p95, post_agg.get_live_p99, post_agg.get_live_p999, dummy_mean, dummy_max);
                post_hist_get_del.ComputeQuantiles(post_agg.get_del_p50, post_agg.get_del_p90, post_agg.get_del_p95, post_agg.get_del_p99, post_agg.get_del_p999, dummy_mean, dummy_max);
                post_hist_scan.ComputeQuantiles(post_agg.scan_p50, post_agg.scan_p90, post_agg.scan_p95, post_agg.scan_p99, post_agg.scan_p999, dummy_mean, dummy_max);
                post_hist_scan_int.ComputeQuantiles(dummy_50, dummy_90, dummy_95, post_agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
                post_hist_scan_non.ComputeQuantiles(dummy_50, dummy_90, dummy_95, post_agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
                post_hist_put.ComputeQuantiles(post_agg.put_p50, post_agg.put_p90, post_agg.put_p95, post_agg.put_p99, post_agg.put_p999, dummy_mean, dummy_max);
                if (post_agg.scan_total_keys_found > 0) post_agg.scan_us_per_key = (post_hist_scan.GetSumNs() / 1000.0) / post_agg.scan_total_keys_found;

                phase_results.push_back(inj_agg);
                phase_results.push_back(post_agg);

                // Oracle Flush if configured
                if (config_.oracle_flush_after_phase_b) {
                    std::cout << "[Coordinator] Executing Oracle Synchronous Flush after Phase B...\n";
                    auto oracle_t0 = std::chrono::steady_clock::now();
                    rocksdb::FlushOptions flush_opts;
                    flush_opts.wait = true;
                    rocksdb::Status fs = db_->Flush(flush_opts);
                    auto oracle_t1 = std::chrono::steady_clock::now();
                    oracle_flush_wait_sec = std::chrono::duration_cast<std::chrono::duration<double>>(oracle_t1 - oracle_t0).count();
                    std::cout << "[Coordinator] Oracle Flush completed in " << std::fixed << std::setprecision(4) 
                              << oracle_flush_wait_sec << " s, Status: " << fs.ToString() << "\n";
                    if (!fs.ok()) {
                        std::cerr << "[Coordinator ERROR] Oracle Flush failed: " << fs.ToString() << "\n";
                        experiment_failed_.store(true);
                    }
                }
            }
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        rtp_controller->Stop();

        auto fg_wallclock_t1 = std::chrono::steady_clock::now();
        double foreground_wallclock_sec = std::chrono::duration_cast<std::chrono::duration<double>>(fg_wallclock_t1 - fg_wallclock_t0).count();

        if (experiment_failed_.load()) {
            std::cerr << "[FormalDriver CRITICAL ERROR] Experiment failed during phase execution!\n";
            return false;
        }

        // Switch EventListener stage to COOLDOWN observation window (Strict 10s Window)
        event_listener_->StartCooldownObservation();
        std::cout << "\n[Cooldown] Strict 10-second post-run background observation window...\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Switch EventListener stage to VERIFICATION (Freezes Cooldown Metrics)
        event_listener_->StartVerificationStage();

        // 3. Deep Key/Value & SHA-256 Full-Scan Verification
        std::cout << "\n[Verification] Running Full Key/Value Version-Aware Verification...\n";
        auto ver_report = KvVerifier::VerifyFullDatabase(db_.get(), worker_models_, config_.total_keys, config_.value_size);

        std::cout << "  DB Live Keys:    " << ver_report.db_live_keys << " (Expected: " << ver_report.model_live_keys << ")\n";
        std::cout << "  DB Payload:      " << (ver_report.db_payload_bytes / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "  DB SHA-256:      " << ver_report.db_sha256_hex << "\n";
        std::cout << "  Model SHA-256:   " << ver_report.model_sha256_hex << "\n";
        std::cout << "  Verification:    " << (ver_report.is_pass ? ">> PASS <<" : ">> FAIL <<") << "\n";

        if (!ver_report.is_pass) {
            std::cerr << "[FormalDriver CRITICAL ERROR] Verification FAILED: " << ver_report.error_detail << std::endl;
            return false;
        }

        // 4. Calculate Formal Metrics
        uint64_t total_trace_events = 0;
        uint64_t total_db_api_calls = 0;
        uint64_t total_logical_put_bytes = 0;
        uint64_t total_scan_limit_truncated = 0;

        ThreadLocalHistogram total_hist_get_live;
        ThreadLocalHistogram total_hist_get_del;
        ThreadLocalHistogram total_hist_scan;
        ThreadLocalHistogram total_hist_put;
        uint64_t total_scan_keys_found = 0;

        for (int w = 0; w < num_workers_; ++w) {
            for (int p = 0; p < 3; ++p) {
                const auto& ws = worker_phase_stats[w][p];
                total_trace_events += ws.completed_ops;
                total_db_api_calls += ws.db_api_calls;
                total_logical_put_bytes += ws.logical_put_bytes;
                total_scan_keys_found += ws.scan_keys_found;
                total_scan_limit_truncated += ws.scan_limit_truncated_count;

                total_hist_get_live.MergeFrom(ws.hist_get_live);
                total_hist_get_del.MergeFrom(ws.hist_get_del);
                total_hist_scan.MergeFrom(ws.hist_scan);
                total_hist_put.MergeFrom(ws.hist_put);
            }
        }

        // Foreground Window Overall Metrics (Divided by foreground_wallclock_sec)
        double fg_trace_iops = (foreground_wallclock_sec > 0) ? (total_trace_events / foreground_wallclock_sec) : 0.0;
        double fg_db_api_iops = (foreground_wallclock_sec > 0) ? (total_db_api_calls / foreground_wallclock_sec) : 0.0;

        uint64_t fg_flush_cnt = event_listener_->GetForegroundFlushCount();
        uint64_t fg_flush_bytes = event_listener_->GetForegroundFlushBytes();
        uint64_t fg_comp_read_bytes = event_listener_->GetForegroundCompactionReadBytes();
        uint64_t fg_comp_write_bytes = event_listener_->GetForegroundCompactionWriteBytes();

        double fg_flush_mb = fg_flush_bytes / (1024.0 * 1024.0);
        double fg_comp_write_mb = fg_comp_write_bytes / (1024.0 * 1024.0);
        double fg_comp_read_mb = fg_comp_read_bytes / (1024.0 * 1024.0);

        double fwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_flush_bytes) / total_logical_put_bytes) : 0.0;
        double cwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_comp_write_bytes) / total_logical_put_bytes) : 0.0;
        double pwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_flush_bytes + fg_comp_write_bytes) / total_logical_put_bytes) : 0.0;

        // Total Experiment (Foreground + Strict 10s Cooldown Window) Metrics
        uint64_t total_exp_flush_cnt = event_listener_->GetTotalExperimentFlushCount();
        uint64_t total_exp_flush_bytes = event_listener_->GetTotalExperimentFlushBytes();
        uint64_t total_exp_comp_write_bytes = event_listener_->GetTotalExperimentCompactionWriteBytes();
        uint64_t controller_fg_flush_cnt =
            event_listener_->GetForegroundFlushCountByReason(
                rocksdb::FlushReason::kRangeTombstoneController);
        uint64_t controller_total_flush_cnt =
            event_listener_->GetTotalExperimentFlushCountByReason(
                rocksdb::FlushReason::kRangeTombstoneController);

        double total_exp_flush_mb = total_exp_flush_bytes / (1024.0 * 1024.0);
        double total_exp_comp_write_mb = total_exp_comp_write_bytes / (1024.0 * 1024.0);

        double fwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_flush_bytes) / total_logical_put_bytes) : 0.0;
        double cwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_comp_write_bytes) / total_logical_put_bytes) : 0.0;
        double pwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_flush_bytes + total_exp_comp_write_bytes) / total_logical_put_bytes) : 0.0;

        double scan_us_per_key = (total_scan_keys_found > 0) ? ((total_hist_scan.GetSumNs() / 1000.0) / total_scan_keys_found) : 0.0;

        double get_live_p50, get_live_p90, get_live_p95, get_live_p99, get_live_p999, dummy_mean, dummy_max;
        double get_del_p50, get_del_p90, get_del_p95, get_del_p99, get_del_p999;
        double scan_p50, scan_p90, scan_p95, scan_p99, scan_p999;
        double put_p50, put_p90, put_p95, put_p99, put_p999;

        total_hist_get_live.ComputeQuantiles(get_live_p50, get_live_p90, get_live_p95, get_live_p99, get_live_p999, dummy_mean, dummy_max);
        total_hist_get_del.ComputeQuantiles(get_del_p50, get_del_p90, get_del_p95, get_del_p99, get_del_p999, dummy_mean, dummy_max);
        total_hist_scan.ComputeQuantiles(scan_p50, scan_p90, scan_p95, scan_p99, scan_p999, dummy_mean, dummy_max);
        total_hist_put.ComputeQuantiles(put_p50, put_p90, put_p95, put_p99, put_p999, dummy_mean, dummy_max);

        // Get SST size on disk
        uint64_t sst_size_total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(config_.db_path)) {
            if (entry.path().extension() == ".sst") {
                sst_size_total += entry.file_size();
            }
        }
        double sst_mb = sst_size_total / (1024.0 * 1024.0);

        // Append to Summary CSV
        bool summary_header = !std::filesystem::exists(config_.summary_csv);
        std::ofstream fsum(config_.summary_csv, std::ios::app);
        if (summary_header) {
            fsum << "exp_id,group_name,desc,threshold,range_tombstone_controller_enabled,range_tombstone_controller_observe_only,range_tombstone_controller_min_range_deletions,range_tombstone_controller_min_memtable_bytes,range_tombstone_controller_cooldown_micros,rtp_mc_mode,rtp_mc_windows,rtp_mc_seals,rtp_mc_conflicts,total_keys,value_size,foreground_wallclock_sec,sum_phase_active_sec,oracle_flush_wait_sec,"
                 << "fg_trace_iops,fg_db_api_iops,scan_us_per_key,scan_p99_us,get_live_p99_us,get_del_p99_us,put_p99_us,"
                 << "scan_limit_truncated_count,"
                 << "fg_flush_count,controller_fg_flush_count,fg_flush_engine_out_mb,fg_comp_read_mb,fg_comp_write_mb,fwa_val_norm_fg,cwa_val_norm_fg,pwa_val_norm_fg,"
                 << "total_exp_flush_count,controller_total_flush_count,total_exp_flush_engine_out_mb,total_exp_comp_write_mb,fwa_val_norm_total,cwa_val_norm_total,pwa_val_norm_total,sst_mb,"
                 << "db_live_keys,model_live_keys,sha256_hex,verification_status\n";
        }
        fsum << config_.exp_id << "," << config_.group_name << ",\"" << config_.desc << "\","
             << config_.memtable_max_range_deletions << ","
             << config_.enable_range_tombstone_controller << ","
             << config_.range_tombstone_controller_observe_only << ","
             << config_.range_tombstone_controller_min_range_deletions << ","
             << config_.range_tombstone_controller_min_memtable_bytes << ","
             << config_.range_tombstone_controller_cooldown_micros << ","
             << config_.rtp_mc_mode << ","
             << rtp_controller->GetTotalWindowsLogged() << ","
             << rtp_controller->GetTotalSealsTriggered() << ","
             << rtp_controller->GetTotalConflictsLogged() << ","
             << config_.total_keys << "," << config_.value_size << ","
             << std::fixed << std::setprecision(4) << foreground_wallclock_sec << ","
             << sum_phase_active_sec << ","
             << oracle_flush_wait_sec << ","
             << fg_trace_iops << "," << fg_db_api_iops << ","
             << scan_us_per_key << "," << scan_p99 << "," << get_live_p99 << "," << get_del_p99 << "," << put_p99 << ","
             << total_scan_limit_truncated << ","
             << fg_flush_cnt << "," << controller_fg_flush_cnt << "," << fg_flush_mb << "," << fg_comp_read_mb << "," << fg_comp_write_mb << ","
             << fwa_val_norm_fg << "," << cwa_val_norm_fg << "," << pwa_val_norm_fg << ","
             << total_exp_flush_cnt << "," << controller_total_flush_cnt << "," << total_exp_flush_mb << "," << total_exp_comp_write_mb << ","
             << fwa_val_norm_total << "," << cwa_val_norm_total << "," << pwa_val_norm_total << "," << sst_mb << ","
             << ver_report.db_live_keys << "," << ver_report.model_live_keys << ","
             << ver_report.db_sha256_hex << ",PASS\n";

        // Append to Phases CSV
        bool phases_header = !std::filesystem::exists(config_.phases_csv);
        std::ofstream fphases(config_.phases_csv, std::ios::app);
        if (phases_header) {
            fphases << "exp_id,group_name,threshold,phase,elapsed_sec,completed_ops,true_phase_iops,"
                    << "scan_us_per_key,scan_p99_us,scan_intersect_p99_us,scan_non_intersect_p99_us,get_live_p99_us,get_del_p99_us,put_p99_us,scan_limit_truncated_count\n";
        }
        for (const auto& pr : phase_results) {
            fphases << config_.exp_id << "," << config_.group_name << "," << config_.memtable_max_range_deletions << ","
                    << "\"" << pr.phase_name << "\"," << std::fixed << std::setprecision(4) << pr.elapsed_sec << ","
                    << pr.completed_ops << "," << pr.true_phase_iops << ","
                    << pr.scan_us_per_key << "," << pr.scan_p99 << ","
                    << pr.scan_intersect_p99 << "," << pr.scan_non_intersect_p99 << ","
                    << pr.get_live_p99 << "," << pr.get_del_p99 << "," << pr.put_p99 << ","
                    << pr.scan_limit_truncated_count << "\n";
        }

        // Dump Events CSV (Offline)
        event_listener_->DumpEventsCsv(config_.events_csv, config_.exp_id);

        std::cout << "[FormalDriver] All summaries and event logs successfully dumped.\n";
        return true;
    }

private:
    FormalConfig config_;
    int num_workers_;
    std::atomic<bool> experiment_failed_;
    std::vector<std::pair<uint64_t, uint64_t>> worker_ranges_;

    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::shared_ptr<FormalEventListener> event_listener_;
    std::unique_ptr<rocksdb::DB> db_;

    std::vector<std::unique_ptr<WorkerStateModel>> worker_models_;
    std::vector<std::vector<std::vector<FormalTraceRecord>>> worker_traces_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    FormalConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
        else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
        else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
        else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
        else if (arg == "--events_csv" && i + 1 < argc) config.events_csv = argv[++i];
        else if (arg == "--phases_csv" && i + 1 < argc) config.phases_csv = argv[++i];
        else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
        else if (arg == "--total_keys" && i + 1 < argc) config.total_keys = std::stoull(argv[++i]);
        else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoull(argv[++i]);
        else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--enable_range_tombstone_controller") config.enable_range_tombstone_controller = true;
        else if (arg == "--disable_range_tombstone_controller") config.enable_range_tombstone_controller = false;
        else if (arg == "--range_tombstone_controller_observe_only") config.range_tombstone_controller_observe_only = true;
        else if (arg == "--range_tombstone_controller_active") config.range_tombstone_controller_observe_only = false;
        else if (arg == "--range_tombstone_controller_min_range_deletions" && i + 1 < argc) config.range_tombstone_controller_min_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--range_tombstone_controller_min_memtable_bytes" && i + 1 < argc) config.range_tombstone_controller_min_memtable_bytes = std::stoull(argv[++i]);
        else if (arg == "--range_tombstone_controller_cooldown_micros" && i + 1 < argc) config.range_tombstone_controller_cooldown_micros = std::stoull(argv[++i]);
        else if (arg == "--rtp_mc_mode" && i + 1 < argc) config.rtp_mc_mode = argv[++i];
        else if (arg == "--windows_csv" && i + 1 < argc) config.windows_csv = argv[++i];
        else if (arg == "--actions_csv" && i + 1 < argc) config.actions_csv = argv[++i];
        else if (arg == "--control_epoch_ms" && i + 1 < argc) config.control_epoch_ms = std::stoull(argv[++i]);
        else if (arg == "--range_del_checkpoint" && i + 1 < argc) config.range_del_checkpoint = std::stoull(argv[++i]);
        else if (arg == "--scan_slo_us" && i + 1 < argc) config.scan_slo_us = std::stod(argv[++i]);
        else if (arg == "--getlive_slo_us" && i + 1 < argc) config.getlive_slo_us = std::stod(argv[++i]);
        else if (arg == "--put_slo_us" && i + 1 < argc) config.put_slo_us = std::stod(argv[++i]);
        else if (arg == "--write_buffer_size" && i + 1 < argc) config.write_buffer_size = std::stoull(argv[++i]);
        else if (arg == "--oracle_flush_after_phase_b") config.oracle_flush_after_phase_b = true;
    }

    if (!config_file.empty()) {
        if (!config.ParseIni(config_file)) {
            std::cerr << "Failed to parse config file: " << config_file << std::endl;
            return 1;
        }
        // Command line overrides
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
            else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
            else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
            else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
            else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
            else if (arg == "--events_csv" && i + 1 < argc) config.events_csv = argv[++i];
            else if (arg == "--phases_csv" && i + 1 < argc) config.phases_csv = argv[++i];
            else if (arg == "--windows_csv" && i + 1 < argc) config.windows_csv = argv[++i];
            else if (arg == "--actions_csv" && i + 1 < argc) config.actions_csv = argv[++i];
            else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
            else if (arg == "--total_keys" && i + 1 < argc) config.total_keys = std::stoull(argv[++i]);
            else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoull(argv[++i]);
            else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--enable_range_tombstone_controller") config.enable_range_tombstone_controller = true;
            else if (arg == "--disable_range_tombstone_controller") config.enable_range_tombstone_controller = false;
            else if (arg == "--range_tombstone_controller_observe_only") config.range_tombstone_controller_observe_only = true;
            else if (arg == "--range_tombstone_controller_active") config.range_tombstone_controller_observe_only = false;
            else if (arg == "--range_tombstone_controller_min_range_deletions" && i + 1 < argc) config.range_tombstone_controller_min_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--range_tombstone_controller_min_memtable_bytes" && i + 1 < argc) config.range_tombstone_controller_min_memtable_bytes = std::stoull(argv[++i]);
            else if (arg == "--range_tombstone_controller_cooldown_micros" && i + 1 < argc) config.range_tombstone_controller_cooldown_micros = std::stoull(argv[++i]);
            else if (arg == "--rtp_mc_mode" && i + 1 < argc) config.rtp_mc_mode = argv[++i];
            else if (arg == "--control_epoch_ms" && i + 1 < argc) config.control_epoch_ms = std::stoull(argv[++i]);
            else if (arg == "--range_del_checkpoint" && i + 1 < argc) config.range_del_checkpoint = std::stoull(argv[++i]);
            else if (arg == "--scan_slo_us" && i + 1 < argc) config.scan_slo_us = std::stod(argv[++i]);
            else if (arg == "--getlive_slo_us" && i + 1 < argc) config.getlive_slo_us = std::stod(argv[++i]);
            else if (arg == "--put_slo_us" && i + 1 < argc) config.put_slo_us = std::stod(argv[++i]);
            else if (arg == "--write_buffer_size" && i + 1 < argc) config.write_buffer_size = std::stoull(argv[++i]);
            else if (arg == "--oracle_flush_after_phase_b") config.oracle_flush_after_phase_b = true;
        }
    }

    FormalDriver driver(config);
    if (!driver.LoadAllWorkerTraces()) return 1;
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    if (!driver.ExecuteExperiment()) return 1;

    return 0;
}
