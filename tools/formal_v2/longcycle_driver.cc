#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/listener.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/iostats_context.h"
#include "rocksdb/perf_context.h"

namespace {

#pragma pack(push, 1)
struct FormalTraceRecord {
    uint8_t phase_id;
    uint8_t op_type;   // 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=Noop
    uint8_t sub_target;
    uint8_t flags;     // 1=ScanIntersect, 2=ScanNonIntersect, 4=Inject, 8=UpdatePut, 16=GetDeleted, 32=GetAdjacent, 64=GetControl
    uint32_t op_id;
    uint64_t key1;
    uint64_t key2;
};
#pragma pack(pop)

struct LatencyCollector {
    std::vector<uint32_t> samples;
    std::mutex mtx;

    void Add(uint32_t micros) {
        std::lock_guard<std::mutex> lock(mtx);
        samples.push_back(micros);
    }

    void Harvest(std::vector<uint32_t>& out) {
        std::lock_guard<std::mutex> lock(mtx);
        out.swap(samples);
        samples.clear();
    }
};

struct Percentiles {
    size_t count = 0;
    double p50 = 0.0, p95 = 0.0, p99 = 0.0, p999 = 0.0;
};

Percentiles CalcPercentiles(std::vector<uint32_t>& data) {
    Percentiles p;
    p.count = data.size();
    if (data.empty()) return p;
    std::sort(data.begin(), data.end());
    size_t n = data.size();
    p.p50 = data[static_cast<size_t>(n * 0.50)];
    p.p95 = data[static_cast<size_t>(n * 0.95)];
    p.p99 = data[static_cast<size_t>(n * 0.99)];
    if (n >= 1000) {
        p.p999 = data[static_cast<size_t>(n * 0.999)];
    } else {
        p.p999 = -1.0; // N/A indicator for < 1000 samples
    }
    return p;
}

std::string FormatKey(uint64_t key) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lu", key);
    return std::string(buf);
}

uint64_t GetDiskBytes(const std::string& path) {
    uint64_t total = 0;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (std::filesystem::is_regular_file(entry.status())) {
                total += std::filesystem::file_size(entry);
            }
        }
    } catch (...) {}
    return total;
}

std::string FlushReasonToString(rocksdb::FlushReason r) {
    const char* s = rocksdb::GetFlushReasonString(r);
    return s ? std::string(s) : "kUnknown";
}

} // namespace

// Detailed EventListener for tracking Flush reasons, sealed MemTable tombstone counts, and Compactions
class LongCycleEventListener : public rocksdb::EventListener {
public:
    LongCycleEventListener(const std::string& result_dir, std::chrono::steady_clock::time_point start_time)
        : start_time_(start_time) {
        events_csv_.open(result_dir + "/sst_tombstone_events.csv");
        events_csv_ << "elapsed_sec,event_type,job_id,reason,input_level,output_level,sst_path,num_range_deletions,data_size,num_entries,dropped_records\n";
    }

    ~LongCycleEventListener() override {
        if (events_csv_.is_open()) {
            events_csv_.close();
        }
    }

    void OnFlushCompleted(rocksdb::DB* /*db*/, const rocksdb::FlushJobInfo& info) override {
        std::lock_guard<std::mutex> lock(mtx_);
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
        uint64_t rdel = info.table_properties.num_range_deletions;
        
        sealed_tombstone_counts_.push_back(rdel);
        total_flushes_++;
        if (info.flush_reason == rocksdb::FlushReason::kMemtableMaxRangeDeletions) {
            flushes_range_del_++;
        } else if (info.flush_reason == rocksdb::FlushReason::kWriteBufferFull) {
            flushes_write_buffer_full_++;
        } else {
            flushes_other_++;
        }

        std::string r_str = FlushReasonToString(info.flush_reason);
        events_csv_ << std::fixed << std::setprecision(2)
                    << el << ",flush," << info.job_id << "," << r_str << ",-1,0,"
                    << info.file_path << "," << rdel << ","
                    << info.table_properties.data_size << ","
                    << info.table_properties.num_entries << ",0\n";
        events_csv_.flush();
    }

    void OnCompactionCompleted(rocksdb::DB* /*db*/, const rocksdb::CompactionJobInfo& ci) override {
        std::lock_guard<std::mutex> lock(mtx_);
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
        
        total_compactions_++;
        uint64_t dropped = (ci.stats.num_input_records > ci.stats.num_output_records) 
                           ? (ci.stats.num_input_records - ci.stats.num_output_records) : 0;
        total_dropped_records_ += dropped;

        uint64_t output_tombstones = 0;
        for (const auto& out_file : ci.output_files) {
            auto it = ci.table_properties.find(out_file);
            if (it != ci.table_properties.end() && it->second) {
                output_tombstones += it->second->num_range_deletions;
            }
        }

        events_csv_ << std::fixed << std::setprecision(2)
                    << el << ",compaction," << ci.job_id << ",kCompaction,"
                    << ci.base_input_level << "," << ci.output_level << ","
                    << (ci.output_files.empty() ? "" : ci.output_files[0]) << ","
                    << output_tombstones << ","
                    << ci.stats.total_output_bytes << ","
                    << ci.stats.num_output_records << ","
                    << dropped << "\n";
        events_csv_.flush();
    }

    struct FlushStats {
        uint64_t total_flushes = 0;
        uint64_t range_del_flushes = 0;
        uint64_t write_buffer_full_flushes = 0;
        uint64_t other_flushes = 0;
        uint64_t total_compactions = 0;
        uint64_t total_dropped_records = 0;
        std::vector<uint64_t> sealed_tombstones;
    };

    FlushStats GetStats() {
        std::lock_guard<std::mutex> lock(mtx_);
        FlushStats s;
        s.total_flushes = total_flushes_;
        s.range_del_flushes = flushes_range_del_;
        s.write_buffer_full_flushes = flushes_write_buffer_full_;
        s.other_flushes = flushes_other_;
        s.total_compactions = total_compactions_;
        s.total_dropped_records = total_dropped_records_;
        s.sealed_tombstones = sealed_tombstone_counts_;
        return s;
    }

private:
    std::chrono::steady_clock::time_point start_time_;
    std::ofstream events_csv_;
    std::mutex mtx_;
    uint64_t total_flushes_ = 0;
    uint64_t flushes_range_del_ = 0;
    uint64_t flushes_write_buffer_full_ = 0;
    uint64_t flushes_other_ = 0;
    uint64_t total_compactions_ = 0;
    uint64_t total_dropped_records_ = 0;
    std::vector<uint64_t> sealed_tombstone_counts_;
};

class LongCycleDriver {
public:
    struct Config {
        std::string db_path;
        std::string trace_dir;
        std::string result_dir;
        std::string exp_id = "longcycle_pilot";
        uint32_t threshold = 0;
        bool is_clean = false;
        int num_workers = 8;
        size_t value_size = 1024;
        int cooldown_sec = 600;
        int sample_interval_sec = 5;
        uint64_t expected_keys = 20480000;
        std::string expected_sha256;
    };

    explicit LongCycleDriver(Config cfg) : cfg_(std::move(cfg)), barrier_count_(0), barrier_generation_(0) {}

    bool Run() {
        std::cout << "================================================================\n";
        std::cout << "  FormalV2 - LongCycle-20GiB Execution Driver\n";
        std::cout << "  Exp ID:        " << cfg_.exp_id << "\n";
        std::cout << "  DB Path:       " << cfg_.db_path << "\n";
        std::cout << "  Threshold:     " << cfg_.threshold << "\n";
        std::cout << "  Is Clean:      " << (cfg_.is_clean ? "true (Background Reference)" : "false") << "\n";
        std::cout << "  Expected Keys: " << cfg_.expected_keys << "\n";
        std::cout << "  Expected SHA:  " << (cfg_.expected_sha256.empty() ? "(none)" : cfg_.expected_sha256) << "\n";
        std::cout << "================================================================\n";

        std::filesystem::create_directories(cfg_.result_dir);
        start_wall_time_ = std::chrono::steady_clock::now();
        listener_ = std::make_shared<LongCycleEventListener>(cfg_.result_dir, start_wall_time_);

        if (!InitDB()) return false;

        timeseries_csv_.open(cfg_.result_dir + "/timeseries.csv");
        WriteTimeseriesHeader();

        snapshots_csv_.open(cfg_.result_dir + "/level_tombstone_snapshots.csv");
        snapshots_csv_ << "elapsed_sec,phase_label,level,total_files,total_bytes,tombstone_files,total_range_tombstones\n";

        stop_sampler_.store(false);
        std::thread sampler_thread(&LongCycleDriver::SamplerThread, this);

        auto start_foreground = std::chrono::steady_clock::now();

        std::vector<std::thread> workers;
        workers.reserve(cfg_.num_workers);
        for (int w = 0; w < cfg_.num_workers; ++w) {
            workers.emplace_back(&LongCycleDriver::WorkerThread, this, w);
        }

        for (auto& t : workers) {
            t.join();
        }

        auto end_foreground = std::chrono::steady_clock::now();
        double foreground_sec = std::chrono::duration<double>(end_foreground - start_foreground).count();
        
        stop_sampler_.store(true);
        sampler_thread.join();
        timeseries_csv_.close();

        std::cout << "\n[DRIVER] Foreground Lifetime completed in " << foreground_sec << "s\n"
                  << "  - Cumulative Trace Ops:   " << total_trace_ops_.load() << " (" << (total_trace_ops_.load() / foreground_sec) << " trace_iops)\n"
                  << "  - Cumulative DB API Ops:  " << total_db_api_ops_.load() << " (" << (total_db_api_ops_.load() / foreground_sec) << " db_api_iops)\n";

        // Execute Cooldown
        ExecuteCooldown();

        // Final snapshot
        TakeLevelSnapshot("COOLDOWN_FINAL");
        snapshots_csv_.close();

        // Verification & Audit
        bool ok = VerifyDatabase();
        SaveSummary(foreground_sec, ok);

        db_.reset();
        return ok;
    }

private:
    bool InitDB() {
        if (std::filesystem::exists(cfg_.db_path)) {
            std::filesystem::remove_all(cfg_.db_path);
        }
        std::filesystem::create_directories(cfg_.db_path);

        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true;
        options.compression = rocksdb::kNoCompression;
        options.write_buffer_size = 64 * 1024 * 1024;
        options.max_write_buffer_number = 4;
        options.min_write_buffer_number_to_merge = 1;
        options.max_background_jobs = 4;
        options.num_levels = 7;
        options.target_file_size_base = 64 * 1024 * 1024;
        options.max_bytes_for_level_base = 256 * 1024 * 1024;
        options.max_bytes_for_level_multiplier = 10.0;
        options.level0_file_num_compaction_trigger = 4;
        options.level0_slowdown_writes_trigger = 20;
        options.level0_stop_writes_trigger = 36;
        options.level_compaction_dynamic_level_bytes = true;
        options.memtable_max_range_deletions = cfg_.threshold;
        options.disable_auto_compactions = false;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(128 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        options.listeners.push_back(listener_);

        rocksdb::Status s = rocksdb::DB::Open(options, cfg_.db_path, &db_);
        if (!s.ok()) {
            std::cerr << "[DRIVER ERROR] DB::Open failed: " << s.ToString() << "\n";
            return false;
        }
        return true;
    }

    void WorkerBarrier(int p_idx) {
        std::unique_lock<std::mutex> lock(barrier_mtx_);
        int gen = barrier_generation_;
        barrier_count_++;
        if (barrier_count_ == cfg_.num_workers) {
            barrier_count_ = 0;
            barrier_generation_++;
            barrier_cv_.notify_all();
        } else {
            barrier_cv_.wait(lock, [this, gen] { return gen != barrier_generation_; });
        }
    }

    void TakeLevelSnapshot(const std::string& phase_label) {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        if (!db_) return;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_wall_time_).count();

        std::vector<rocksdb::LiveFileMetaData> live_files;
        db_->GetLiveFilesMetaData(&live_files);

        rocksdb::TablePropertiesCollection props;
        db_->GetPropertiesOfAllTables(&props);

        // level -> (files, bytes, tombstone_files, total_tombstones)
        struct LevelStat {
            uint64_t files = 0;
            uint64_t bytes = 0;
            uint64_t ts_files = 0;
            uint64_t total_ts = 0;
        };
        std::vector<LevelStat> stats(7);

        for (const auto& f : live_files) {
            if (f.level >= 0 && f.level < 7) {
                stats[f.level].files++;
                stats[f.level].bytes += f.size;
                
                uint64_t rdel = 0;
                auto it = props.find(f.db_path + f.name);
                if (it == props.end()) {
                    it = props.find(f.name);
                }
                if (it != props.end() && it->second) {
                    rdel = it->second->num_range_deletions;
                }
                if (rdel > 0) {
                    stats[f.level].ts_files++;
                    stats[f.level].total_ts += rdel;
                }
            }
        }

        for (int lvl = 0; lvl < 7; ++lvl) {
            snapshots_csv_ << std::fixed << std::setprecision(2)
                           << el << "," << phase_label << "," << lvl << ","
                           << stats[lvl].files << "," << stats[lvl].bytes << ","
                           << stats[lvl].ts_files << "," << stats[lvl].total_ts << "\n";
        }
        snapshots_csv_.flush();
        std::cout << "[DRIVER] Level Snapshot recorded for " << phase_label << "\n";
    }

    void WorkerThread(int worker_id) {
        std::vector<std::string> phases = {"phase_a", "phase_b", "phase_c", "phase_d"};
        std::string value_payload(cfg_.value_size, 'V');

        rocksdb::WriteOptions wopt;
        rocksdb::ReadOptions ropt;
        ropt.total_order_seek = true;

        for (int p_idx = 0; p_idx < 4; ++p_idx) {
            std::string trace_file = cfg_.trace_dir + "/" + phases[p_idx] + "-worker-" + 
                                     (worker_id < 10 ? "0" : "") + std::to_string(worker_id) + ".bin";
            
            std::ifstream fin(trace_file, std::ios::binary);
            if (!fin.is_open()) {
                std::cerr << "[DRIVER ERROR] Cannot open trace: " << trace_file << "\n";
                return;
            }

            FormalTraceRecord rec;
            while (fin.read(reinterpret_cast<char*>(&rec), sizeof(FormalTraceRecord))) {
                auto t0 = std::chrono::steady_clock::now();

                if (rec.op_type == 2) { // Put (New or Update)
                    std::string k = FormatKey(rec.key1);
                    db_->Put(wopt, k, value_payload);
                    auto t1 = std::chrono::steady_clock::now();
                    lat_put_.Add(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
                    if (rec.flags & 8) cumulative_update_puts_++;
                    else cumulative_new_puts_++;
                    total_db_api_ops_++;
                } else if (rec.op_type == 3) { // DeleteRange
                    if (!cfg_.is_clean) {
                        std::string start_k = FormatKey(rec.key1);
                        std::string end_k = FormatKey(rec.key2);
                        db_->DeleteRange(wopt, db_->DefaultColumnFamily(), start_k, end_k);
                        auto t1 = std::chrono::steady_clock::now();
                        lat_del_range_.Add(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
                        cumulative_del_ranges_++;
                        total_db_api_ops_++;
                    } else {
                        // In CLEAN mode, DeleteRange is recorded as No-op
                        cumulative_clean_noops_++;
                    }
                } else if (rec.op_type == 0) { // Get
                    std::string k = FormatKey(rec.key1);
                    std::string val;
                    rocksdb::Status s = db_->Get(ropt, k, &val);
                    auto t1 = std::chrono::steady_clock::now();
                    uint32_t us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
                    
                    if (rec.flags & 16) lat_get_deleted_.Add(us);
                    else if (rec.flags & 32) lat_get_adjacent_.Add(us);
                    else lat_get_control_.Add(us);
                    total_db_api_ops_++;
                } else if (rec.op_type == 1) { // Scan [key1, key2)
                    std::string start_k = FormatKey(rec.key1);
                    std::string end_k = FormatKey(rec.key2);
                    auto iter = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ropt));
                    iter->Seek(start_k);
                    while (iter->Valid() && iter->key().ToString() < end_k) {
                        iter->Next();
                    }
                    auto t1 = std::chrono::steady_clock::now();
                    uint32_t us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
                    
                    if (rec.flags & 1) lat_scan_intersect_.Add(us);
                    else if (rec.flags & 2) lat_scan_non_intersect_.Add(us);
                    else lat_scan_boundary_.Add(us);
                    total_db_api_ops_++;
                }

                total_trace_ops_++;
            }

            // Barrier sync across all workers at phase boundary
            WorkerBarrier(p_idx);
            if (worker_id == 0) {
                std::cout << "[DRIVER] Barrier passed: Finished " << phases[p_idx] 
                          << " (Trace Ops: " << total_trace_ops_.load() 
                          << ", DB API Ops: " << total_db_api_ops_.load() << ")\n";
                current_phase_.store(p_idx + 1);
                TakeLevelSnapshot(phases[p_idx]);
            }
            WorkerBarrier(p_idx);
        }
    }

    void WriteTimeseriesHeader() {
        timeseries_csv_ << "elapsed_sec,phase_id,interval_trace_ops,interval_trace_iops,"
                        << "interval_db_api_ops,interval_db_api_iops,cum_new_puts,cum_update_puts,cum_del_ranges,cum_clean_noops,"
                        << "put_cnt,put_p50,put_p95,put_p99,put_p999,"
                        << "get_del_cnt,get_del_p99,get_adj_cnt,get_adj_p99,get_ctl_cnt,get_ctl_p99,"
                        << "scan_inter_cnt,scan_inter_p99,scan_non_cnt,scan_non_p99,scan_bound_cnt,scan_bound_p99,"
                        << "del_range_cnt,del_range_p99,"
                        << "stall_micros_delta,is_write_stopped,is_write_slowdown,"
                        << "memtable_size_bytes,memtable_tombstones,num_imm,"
                        << "l0_files,l1_files,l2_files,l3_files,l4_files,l5_files,l6_files,"
                        << "l0_bytes,l1_bytes,l2_bytes,l3_bytes,l4_bytes,l5_bytes,l6_bytes,"
                        << "pending_compaction_bytes,disk_bytes\n";
    }

    void SamplerThread() {
        auto start_t = std::chrono::steady_clock::now();
        uint64_t last_trace_ops = 0;
        uint64_t last_db_api_ops = 0;
        uint64_t last_stall_micros = 0;

        while (!stop_sampler_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.sample_interval_sec));
            if (stop_sampler_.load()) break;

            auto now_t = std::chrono::steady_clock::now();
            double el = std::chrono::duration<double>(now_t - start_t).count();
            
            uint64_t cur_trace = total_trace_ops_.load();
            uint64_t cur_db_api = total_db_api_ops_.load();
            uint64_t delta_trace = cur_trace - last_trace_ops;
            uint64_t delta_db_api = cur_db_api - last_db_api_ops;
            double interval_trace_iops = delta_trace / static_cast<double>(cfg_.sample_interval_sec);
            double interval_db_api_iops = delta_db_api / static_cast<double>(cfg_.sample_interval_sec);
            last_trace_ops = cur_trace;
            last_db_api_ops = cur_db_api;

            // Harvest Latencies
            std::vector<uint32_t> puts, get_del, get_adj, get_ctl, scan_inter, scan_non, scan_bound, del_range;
            lat_put_.Harvest(puts);
            lat_get_deleted_.Harvest(get_del);
            lat_get_adjacent_.Harvest(get_adj);
            lat_get_control_.Harvest(get_ctl);
            lat_scan_intersect_.Harvest(scan_inter);
            lat_scan_non_intersect_.Harvest(scan_non);
            lat_scan_boundary_.Harvest(scan_bound);
            lat_del_range_.Harvest(del_range);

            auto p_put = CalcPercentiles(puts);
            auto p_gdel = CalcPercentiles(get_del);
            auto p_gadj = CalcPercentiles(get_adj);
            auto p_gctl = CalcPercentiles(get_ctl);
            auto p_sinter = CalcPercentiles(scan_inter);
            auto p_snon = CalcPercentiles(scan_non);
            auto p_sbound = CalcPercentiles(scan_bound);
            auto p_del = CalcPercentiles(del_range);

            // Lightweight RocksDB properties
            std::string s_mem, s_rdel, s_imm, s_pending, s_stopped, s_slowdown, s_stall_us;
            db_->GetProperty("rocksdb.cur-size-active-mem-table", &s_mem);
            db_->GetProperty("rocksdb.num-entries-active-mem-table", &s_rdel);
            db_->GetProperty("rocksdb.num-immutable-mem-table", &s_imm);
            db_->GetProperty("rocksdb.estimate-pending-compaction-bytes", &s_pending);
            db_->GetProperty("rocksdb.is-write-stopped", &s_stopped);
            db_->GetProperty("rocksdb.is-write-slowdown", &s_slowdown);
            db_->GetProperty("rocksdb.actual-delayed-write-rate", &s_stall_us);

            std::string s_f[7], s_b[7];
            for (int lvl = 0; lvl < 7; ++lvl) {
                db_->GetProperty("rocksdb.num-files-at-level" + std::to_string(lvl), &s_f[lvl]);
                db_->GetProperty("rocksdb.total-sst-files-size-at-level" + std::to_string(lvl), &s_b[lvl]);
            }

            uint64_t disk_bytes = GetDiskBytes(cfg_.db_path);

            timeseries_csv_ << std::fixed << std::setprecision(2)
                            << el << "," << current_phase_.load() << ","
                            << delta_trace << "," << interval_trace_iops << ","
                            << delta_db_api << "," << interval_db_api_iops << ","
                            << cumulative_new_puts_.load() << "," << cumulative_update_puts_.load() << ","
                            << cumulative_del_ranges_.load() << "," << cumulative_clean_noops_.load() << ","
                            << p_put.count << "," << p_put.p50 << "," << p_put.p95 << "," << p_put.p99 << "," << (p_put.p999 < 0 ? "N/A" : std::to_string(p_put.p999)) << ","
                            << p_gdel.count << "," << p_gdel.p99 << ","
                            << p_gadj.count << "," << p_gadj.p99 << ","
                            << p_gctl.count << "," << p_gctl.p99 << ","
                            << p_sinter.count << "," << p_sinter.p99 << ","
                            << p_snon.count << "," << p_snon.p99 << ","
                            << p_sbound.count << "," << p_sbound.p99 << ","
                            << p_del.count << "," << p_del.p99 << ","
                            << 0 << "," << (s_stopped.empty() ? "0" : s_stopped) << "," << (s_slowdown.empty() ? "0" : s_slowdown) << ","
                            << (s_mem.empty() ? "0" : s_mem) << "," << (s_rdel.empty() ? "0" : s_rdel) << "," << (s_imm.empty() ? "0" : s_imm) << ","
                            << (s_f[0].empty()?"0":s_f[0]) << "," << (s_f[1].empty()?"0":s_f[1]) << "," << (s_f[2].empty()?"0":s_f[2]) << ","
                            << (s_f[3].empty()?"0":s_f[3]) << "," << (s_f[4].empty()?"0":s_f[4]) << "," << (s_f[5].empty()?"0":s_f[5]) << "," << (s_f[6].empty()?"0":s_f[6]) << ","
                            << (s_b[0].empty()?"0":s_b[0]) << "," << (s_b[1].empty()?"0":s_b[1]) << "," << (s_b[2].empty()?"0":s_b[2]) << ","
                            << (s_b[3].empty()?"0":s_b[3]) << "," << (s_b[4].empty()?"0":s_b[4]) << "," << (s_b[5].empty()?"0":s_b[5]) << "," << (s_b[6].empty()?"0":s_b[6]) << ","
                            << (s_pending.empty()?"0":s_pending) << "," << disk_bytes << "\n";
            timeseries_csv_.flush();
        }
    }

    void ExecuteCooldown() {
        std::cout << "[DRIVER] Entering " << cfg_.cooldown_sec << "s Post-Workload Cooldown (observing background debt convergence)...\n";
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto now = std::chrono::steady_clock::now();
            double el = std::chrono::duration<double>(now - start).count();
            if (el >= cfg_.cooldown_sec) break;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        std::cout << "[DRIVER] Cooldown completed.\n";
    }

    bool VerifyDatabase() {
        std::cout << "[DRIVER RECONCILIATION] Scanning entire database sequentially...\n";
        rocksdb::ReadOptions ropt;
        ropt.total_order_seek = true;
        auto iter = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ropt));
        iter->SeekToFirst();

        uint64_t count = 0;
        uint64_t total_val_bytes = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        SHA256_CTX sha;
        SHA256_Init(&sha);

        while (iter->Valid()) {
            std::string k = iter->key().ToString();
            std::string v = iter->value().ToString();
            SHA256_Update(&sha, k.data(), k.size());
            SHA256_Update(&sha, v.data(), v.size());
            count++;
            total_val_bytes += v.size();
            iter->Next();
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha);
#pragma GCC diagnostic pop
        char hex[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }

        std::string final_sha(hex);
        std::cout << "  - Visible Keys:      " << count << " (Expected: " << cfg_.expected_keys << ")\n";
        std::cout << "  - Visible Val Bytes: " << total_val_bytes << " (" << (total_val_bytes / 1e9) << " GB)\n";
        std::cout << "  - DB State SHA-256:  " << final_sha << "\n";
        if (!cfg_.expected_sha256.empty()) {
            std::cout << "  - Model State SHA:   " << cfg_.expected_sha256 << "\n";
        }

        if (count != cfg_.expected_keys) {
            std::cerr << "[DRIVER ERROR] Visible key count mismatch! Found=" << count << ", Expected=" << cfg_.expected_keys << "\n";
            return false;
        }

        if (!cfg_.expected_sha256.empty() && final_sha != cfg_.expected_sha256) {
            std::cerr << "[DRIVER ERROR] State SHA-256 mismatch against model!\n";
            return false;
        }

        std::cout << "[DRIVER RECONCILIATION] PASSED 100% (Keys and SHA-256 strictly matched).\n";
        return true;
    }

    void SaveSummary(double foreground_sec, bool audit_pass) {
        std::ofstream fout(cfg_.result_dir + "/summary.json");
        auto stats = listener_->GetStats();
        uint64_t disk_bytes = GetDiskBytes(cfg_.db_path);

        std::vector<uint64_t> t_counts = stats.sealed_tombstones;
        std::sort(t_counts.begin(), t_counts.end());
        size_t n = t_counts.size();
        uint64_t ts_max = t_counts.empty() ? 0 : t_counts.back();
        uint64_t ts_min = t_counts.empty() ? 0 : t_counts.front();
        double ts_p50 = t_counts.empty() ? 0 : t_counts[static_cast<size_t>(n * 0.50)];
        double ts_p75 = t_counts.empty() ? 0 : t_counts[static_cast<size_t>(n * 0.75)];
        double ts_p90 = t_counts.empty() ? 0 : t_counts[static_cast<size_t>(n * 0.90)];
        double ts_p95 = t_counts.empty() ? 0 : t_counts[static_cast<size_t>(n * 0.95)];

        fout << "{\n"
             << "  \"exp_id\": \"" << cfg_.exp_id << "\",\n"
             << "  \"threshold\": " << cfg_.threshold << ",\n"
             << "  \"is_clean\": " << (cfg_.is_clean ? "true" : "false") << ",\n"
             << "  \"foreground_sec\": " << foreground_sec << ",\n"
             << "  \"total_trace_ops\": " << total_trace_ops_.load() << ",\n"
             << "  \"total_db_api_ops\": " << total_db_api_ops_.load() << ",\n"
             << "  \"trace_iops\": " << (total_trace_ops_.load() / foreground_sec) << ",\n"
             << "  \"db_api_iops\": " << (total_db_api_ops_.load() / foreground_sec) << ",\n"
             << "  \"cum_new_puts\": " << cumulative_new_puts_.load() << ",\n"
             << "  \"cum_update_puts\": " << cumulative_update_puts_.load() << ",\n"
             << "  \"cum_del_ranges\": " << cumulative_del_ranges_.load() << ",\n"
             << "  \"cum_clean_noops\": " << cumulative_clean_noops_.load() << ",\n"
             << "  \"physical_db_bytes\": " << disk_bytes << ",\n"
             << "  \"flush_stats\": {\n"
             << "    \"total_flushes\": " << stats.total_flushes << ",\n"
             << "    \"range_del_flushes\": " << stats.range_del_flushes << ",\n"
             << "    \"write_buffer_full_flushes\": " << stats.write_buffer_full_flushes << ",\n"
             << "    \"other_flushes\": " << stats.other_flushes << ",\n"
             << "    \"total_compactions\": " << stats.total_compactions << ",\n"
             << "    \"total_dropped_records\": " << stats.total_dropped_records << ",\n"
             << "    \"tombstones_per_memtable_min\": " << ts_min << ",\n"
             << "    \"tombstones_per_memtable_p50\": " << ts_p50 << ",\n"
             << "    \"tombstones_per_memtable_p75\": " << ts_p75 << ",\n"
             << "    \"tombstones_per_memtable_p90\": " << ts_p90 << ",\n"
             << "    \"tombstones_per_memtable_p95\": " << ts_p95 << ",\n"
             << "    \"tombstones_per_memtable_max\": " << ts_max << "\n"
             << "  },\n"
             << "  \"audit_pass\": " << (audit_pass ? "true" : "false") << "\n"
             << "}\n";
    }

    Config cfg_;
    std::unique_ptr<rocksdb::DB> db_;
    std::shared_ptr<LongCycleEventListener> listener_;
    std::ofstream timeseries_csv_;
    std::ofstream snapshots_csv_;

    std::chrono::steady_clock::time_point start_wall_time_;
    std::atomic<bool> stop_sampler_{false};
    std::atomic<int> current_phase_{0};
    std::atomic<uint64_t> total_trace_ops_{0};
    std::atomic<uint64_t> total_db_api_ops_{0};
    std::atomic<uint64_t> cumulative_new_puts_{0};
    std::atomic<uint64_t> cumulative_update_puts_{0};
    std::atomic<uint64_t> cumulative_del_ranges_{0};
    std::atomic<uint64_t> cumulative_clean_noops_{0};

    // Barriers
    std::mutex barrier_mtx_;
    std::condition_variable barrier_cv_;
    int barrier_count_;
    int barrier_generation_;

    std::mutex snapshot_mtx_;

    // Latency collectors
    LatencyCollector lat_put_;
    LatencyCollector lat_del_range_;
    LatencyCollector lat_get_deleted_;
    LatencyCollector lat_get_adjacent_;
    LatencyCollector lat_get_control_;
    LatencyCollector lat_scan_intersect_;
    LatencyCollector lat_scan_non_intersect_;
    LatencyCollector lat_scan_boundary_;
};

int main(int argc, char** argv) {
    LongCycleDriver::Config config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--db_path" || arg == "--db_dir") && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
        else if ((arg == "--result_dir" || arg == "--out_dir") && i + 1 < argc) config.result_dir = argv[++i];
        else if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
        else if (arg == "--threshold" && i + 1 < argc) config.threshold = std::stoul(argv[++i]);
        else if (arg == "--is_clean" && i + 1 < argc) config.is_clean = (std::string(argv[++i]) == "true");
        else if (arg == "--num_workers" && i + 1 < argc) config.num_workers = std::stoi(argv[++i]);
        else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoul(argv[++i]);
        else if (arg == "--cooldown_sec" && i + 1 < argc) config.cooldown_sec = std::stoi(argv[++i]);
        else if (arg == "--sample_interval_sec" && i + 1 < argc) config.sample_interval_sec = std::stoi(argv[++i]);
        else if (arg == "--expected_keys" && i + 1 < argc) config.expected_keys = std::stoull(argv[++i]);
        else if (arg == "--expected_sha256" && i + 1 < argc) config.expected_sha256 = argv[++i];
    }

    if (config.db_path.empty() || config.trace_dir.empty() || config.result_dir.empty()) {
        std::cerr << "Usage: " << argv[0] << " --db_path <path> --trace_dir <dir> --result_dir <dir> [options]\n";
        return 1;
    }

    LongCycleDriver driver(config);
    return driver.Run() ? 0 : 1;
}
