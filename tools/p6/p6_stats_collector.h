#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

#include "rocksdb/statistics.h"
#include "rocksdb/db.h"

namespace study::p6 {

struct P6TimeSeriesPoint {
    int second_idx;
    std::string phase; // "warmup", "main", "cooldown"
    
    // Frontend throughput & latency
    uint64_t get_ctrl_ops;
    double get_ctrl_p50_us, get_ctrl_p95_us, get_ctrl_p99_us;
    uint64_t get_aff_ops;
    double get_aff_p50_us, get_aff_p95_us, get_aff_p99_us;
    uint64_t scan_ops;
    double scan_p50_us, scan_p95_us, scan_p99_us;
    double scan_avg_keys;
    double scan_us_per_key;
    uint64_t put_ops;
    double put_p50_us, put_p95_us, put_p99_us;
    uint64_t del_range_ops;
    double del_range_p99_us;
    uint64_t total_ops;
    double overall_iops;

    // RocksDB internal state
    double active_memtable_mb;
    uint64_t immutable_memtable_count;
    uint64_t memtable_flush_pending;
    uint64_t l0_files;
    double pending_compaction_mb;
    uint64_t running_flushes;
    uint64_t running_compactions;
    double total_sst_mb;

    // Ticker deltas
    double compaction_read_mb_delta;
    double compaction_write_mb_delta;
    double flush_write_mb_delta;
    uint64_t write_stall_micros_delta;
    uint64_t block_cache_hits_delta;
    uint64_t block_cache_misses_delta;
    double block_cache_hit_rate;
    uint64_t tombstones_dropped_delta;

    // System stats
    double cpu_util_pct;
    double db_dir_mb;
};

class P6LatencyTracker {
public:
    P6LatencyTracker() : count_(0), sum_ns_(0), max_ns_(0) {
        samples_.reserve(300000);
    }

    void Record(uint64_t lat_ns) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(lat_ns);
        count_++;
        sum_ns_ += lat_ns;
        if (lat_ns > max_ns_) max_ns_ = lat_ns;
    }

    void GetQuantiles(double& p50_us, double& p95_us, double& p99_us, double& mean_us, double& max_us, uint64_t& cnt) {
        std::lock_guard<std::mutex> lock(mutex_);
        cnt = count_;
        if (samples_.empty()) {
            p50_us = p95_us = p99_us = mean_us = max_us = 0.0;
            return;
        }

        std::sort(samples_.begin(), samples_.end());
        size_t n = samples_.size();
        p50_us = samples_[static_cast<size_t>(n * 0.50)] / 1000.0;
        p95_us = samples_[static_cast<size_t>(n * 0.95)] / 1000.0;
        p99_us = samples_[static_cast<size_t>(n * 0.99)] / 1000.0;
        mean_us = (static_cast<double>(sum_ns_) / n) / 1000.0;
        max_us = max_ns_ / 1000.0;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        count_ = 0;
        sum_ns_ = 0;
        max_ns_ = 0;
    }

private:
    std::mutex mutex_;
    std::vector<uint64_t> samples_;
    uint64_t count_;
    uint64_t sum_ns_;
    uint64_t max_ns_;
};

class P6StatsCollector {
public:
    P6StatsCollector()
        : main_put_ops_(0), main_get_aff_ops_(0), main_get_del_ops_(0), main_get_ctrl_ops_(0),
          main_scan_ops_(0), main_scan_returned_keys_(0), main_scan_span_requested_(0),
          main_del_range_ops_(0) {}

    void RecordPut(bool is_main, uint64_t lat_ns) {
        if (is_main) {
            main_put_ops_++;
            main_put_lat_.Record(lat_ns);
        }
        sec_put_lat_.Record(lat_ns);
    }

    void RecordGet(bool is_main, int category, uint64_t lat_ns) {
        if (category == 0) { // Affected-Live
            if (is_main) {
                main_get_aff_ops_++;
                main_get_aff_lat_.Record(lat_ns);
            }
            sec_get_aff_lat_.Record(lat_ns);
        } else if (category == 1) { // Deleted
            if (is_main) {
                main_get_del_ops_++;
                main_get_del_lat_.Record(lat_ns);
            }
            sec_get_del_lat_.Record(lat_ns);
        } else { // Control
            if (is_main) {
                main_get_ctrl_ops_++;
                main_get_ctrl_lat_.Record(lat_ns);
            }
            sec_get_ctrl_lat_.Record(lat_ns);
        }
    }

    void RecordScan(bool is_main, uint64_t lat_ns, uint64_t span_req, size_t returned_keys) {
        if (is_main) {
            main_scan_ops_++;
            main_scan_span_requested_ += span_req;
            main_scan_returned_keys_ += returned_keys;
            main_scan_lat_.Record(lat_ns);
        }
        sec_scan_lat_.Record(lat_ns);
        sec_scan_keys_ += returned_keys;
    }

    void RecordDeleteRange(bool is_main, uint64_t lat_ns) {
        if (is_main) {
            main_del_range_ops_++;
            main_del_range_lat_.Record(lat_ns);
        }
        sec_del_range_lat_.Record(lat_ns);
    }

    // Capture 1-second sample with RocksDB internal properties and system state
    void CaptureTimeSeriesSecond(int sec_idx, const std::string& phase,
                                 rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                 const std::string& db_path) {
        // Tickers
        uint64_t cur_comp_read = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) : 0;
        uint64_t cur_comp_write = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) : 0;
        uint64_t cur_flush_write = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) : 0;
        uint64_t cur_write_stall = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t cur_cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cur_cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;
        uint64_t cur_tomb_drop = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t comp_read_delta = cur_comp_read - last_comp_read_;
        uint64_t comp_write_delta = cur_comp_write - last_comp_write_;
        uint64_t flush_write_delta = cur_flush_write - last_flush_write_;
        uint64_t write_stall_delta = cur_write_stall - last_write_stall_;
        uint64_t cache_hit_delta = cur_cache_hit - last_cache_hit_;
        uint64_t cache_miss_delta = cur_cache_miss - last_cache_miss_;
        uint64_t tomb_drop_delta = cur_tomb_drop - last_tomb_drop_;

        last_comp_read_ = cur_comp_read;
        last_comp_write_ = cur_comp_write;
        last_flush_write_ = cur_flush_write;
        last_write_stall_ = cur_write_stall;
        last_cache_hit_ = cur_cache_hit;
        last_cache_miss_ = cur_cache_miss;
        last_tomb_drop_ = cur_tomb_drop;

        // RocksDB Properties
        uint64_t active_memtable_bytes = 0;
        uint64_t num_immutable_memtables = 0;
        uint64_t memtable_flush_pending = 0;
        uint64_t l0_files = 0;
        uint64_t pending_compaction_bytes = 0;
        uint64_t running_flushes = 0;
        uint64_t running_compactions = 0;
        uint64_t total_sst_bytes = 0;

        if (db) {
            db->GetIntProperty("rocksdb.cur-size-active-mem-table", &active_memtable_bytes);
            db->GetIntProperty("rocksdb.num-immutable-mem-table", &num_immutable_memtables);
            db->GetIntProperty("rocksdb.mem-table-flush-pending", &memtable_flush_pending);
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_compaction_bytes);
            db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
            db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
            db->GetIntProperty("rocksdb.total-sst-files-size", &total_sst_bytes);
        }

        // Frontend Latencies
        double p50, p95, p99, mean, max_us;
        uint64_t ctrl_cnt, aff_cnt, scan_cnt, put_cnt, del_cnt;

        sec_get_ctrl_lat_.GetQuantiles(p50, p95, p99, mean, max_us, ctrl_cnt);
        double ctrl_p50 = p50, ctrl_p95 = p95, ctrl_p99 = p99;
        sec_get_ctrl_lat_.Clear();

        sec_get_aff_lat_.GetQuantiles(p50, p95, p99, mean, max_us, aff_cnt);
        double aff_p50 = p50, aff_p95 = p95, aff_p99 = p99;
        sec_get_aff_lat_.Clear();

        sec_scan_lat_.GetQuantiles(p50, p95, p99, mean, max_us, scan_cnt);
        double scan_p50 = p50, scan_p95 = p95, scan_p99 = p99;
        uint64_t sec_keys = sec_scan_keys_.exchange(0);
        double scan_avg_keys = (scan_cnt > 0) ? (static_cast<double>(sec_keys) / scan_cnt) : 0.0;
        double scan_us_per_key = (sec_keys > 0) ? ((mean * scan_cnt) / sec_keys) : 0.0;
        sec_scan_lat_.Clear();

        sec_put_lat_.GetQuantiles(p50, p95, p99, mean, max_us, put_cnt);
        double put_p50 = p50, put_p95 = p95, put_p99 = p99;
        sec_put_lat_.Clear();

        sec_del_range_lat_.GetQuantiles(p50, p95, p99, mean, max_us, del_cnt);
        double del_p99 = p99;
        sec_del_range_lat_.Clear();

        uint64_t sec_total_ops = ctrl_cnt + aff_cnt + scan_cnt + put_cnt + del_cnt;
        double hit_rate = (cache_hit_delta + cache_miss_delta > 0) ? (100.0 * cache_hit_delta / (cache_hit_delta + cache_miss_delta)) : 0.0;

        // DB physical size on disk
        double db_dir_mb = 0.0;
        try {
            uint64_t total_dir_bytes = 0;
            if (std::filesystem::exists(db_path)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(db_path)) {
                    if (entry.is_regular_file()) total_dir_bytes += entry.file_size();
                }
            }
            db_dir_mb = total_dir_bytes / (1024.0 * 1024.0);
        } catch (...) {}

        P6TimeSeriesPoint pt;
        pt.second_idx = sec_idx;
        pt.phase = phase;
        pt.get_ctrl_ops = ctrl_cnt;
        pt.get_ctrl_p50_us = ctrl_p50; pt.get_ctrl_p95_us = ctrl_p95; pt.get_ctrl_p99_us = ctrl_p99;
        pt.get_aff_ops = aff_cnt;
        pt.get_aff_p50_us = aff_p50; pt.get_aff_p95_us = aff_p95; pt.get_aff_p99_us = aff_p99;
        pt.scan_ops = scan_cnt;
        pt.scan_p50_us = scan_p50; pt.scan_p95_us = scan_p95; pt.scan_p99_us = scan_p99;
        pt.scan_avg_keys = scan_avg_keys;
        pt.scan_us_per_key = scan_us_per_key;
        pt.put_ops = put_cnt;
        pt.put_p50_us = put_p50; pt.put_p95_us = put_p95; pt.put_p99_us = put_p99;
        pt.del_range_ops = del_cnt;
        pt.del_range_p99_us = del_p99;
        pt.total_ops = sec_total_ops;
        pt.overall_iops = static_cast<double>(sec_total_ops);

        pt.active_memtable_mb = active_memtable_bytes / (1024.0 * 1024.0);
        pt.immutable_memtable_count = num_immutable_memtables;
        pt.memtable_flush_pending = memtable_flush_pending;
        pt.l0_files = l0_files;
        pt.pending_compaction_mb = pending_compaction_bytes / (1024.0 * 1024.0);
        pt.running_flushes = running_flushes;
        pt.running_compactions = running_compactions;
        pt.total_sst_mb = total_sst_bytes / (1024.0 * 1024.0);

        pt.compaction_read_mb_delta = comp_read_delta / (1024.0 * 1024.0);
        pt.compaction_write_mb_delta = comp_write_delta / (1024.0 * 1024.0);
        pt.flush_write_mb_delta = flush_write_delta / (1024.0 * 1024.0);
        pt.write_stall_micros_delta = write_stall_delta;
        pt.block_cache_hits_delta = cache_hit_delta;
        pt.block_cache_misses_delta = cache_miss_delta;
        pt.block_cache_hit_rate = hit_rate;
        pt.tombstones_dropped_delta = tomb_drop_delta;

        pt.cpu_util_pct = 0.0; // Handled by process stats
        pt.db_dir_mb = db_dir_mb;

        std::lock_guard<std::mutex> lk(ts_mutex_);
        timeseries_records_.push_back(pt);
    }

    void DumpTimeSeriesCsv(const std::string& filepath, const std::string& exp_id, const std::string& group_name) {
        if (filepath.empty()) return;
        std::ofstream file(filepath, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name,second_idx,phase,"
                 << "get_ctrl_ops,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,"
                 << "get_aff_ops,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,"
                 << "scan_ops,scan_p50_us,scan_p95_us,scan_p99_us,scan_avg_keys,scan_us_per_key,"
                 << "put_ops,put_p50_us,put_p95_us,put_p99_us,"
                 << "del_range_ops,del_range_p99_us,total_ops,overall_iops,"
                 << "active_memtable_mb,immutable_memtable_count,memtable_flush_pending,l0_files,pending_compaction_mb,"
                 << "running_flushes,running_compactions,total_sst_mb,"
                 << "compaction_read_mb_delta,compaction_write_mb_delta,flush_write_mb_delta,write_stall_micros_delta,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,tombstones_dropped_delta,db_dir_mb\n";
        }

        std::lock_guard<std::mutex> lk(ts_mutex_);
        for (const auto& pt : timeseries_records_) {
            file << exp_id << "," << group_name << "," << pt.second_idx << "," << pt.phase << ","
                 << pt.get_ctrl_ops << "," << pt.get_ctrl_p50_us << "," << pt.get_ctrl_p95_us << "," << pt.get_ctrl_p99_us << ","
                 << pt.get_aff_ops << "," << pt.get_aff_p50_us << "," << pt.get_aff_p95_us << "," << pt.get_aff_p99_us << ","
                 << pt.scan_ops << "," << pt.scan_p50_us << "," << pt.scan_p95_us << "," << pt.scan_p99_us << ","
                 << pt.scan_avg_keys << "," << pt.scan_us_per_key << ","
                 << pt.put_ops << "," << pt.put_p50_us << "," << pt.put_p95_us << "," << pt.put_p99_us << ","
                 << pt.del_range_ops << "," << pt.del_range_p99_us << "," << pt.total_ops << "," << pt.overall_iops << ","
                 << pt.active_memtable_mb << "," << pt.immutable_memtable_count << "," << pt.memtable_flush_pending << ","
                 << pt.l0_files << "," << pt.pending_compaction_mb << ","
                 << pt.running_flushes << "," << pt.running_compactions << "," << pt.total_sst_mb << ","
                 << pt.compaction_read_mb_delta << "," << pt.compaction_write_mb_delta << "," << pt.flush_write_mb_delta << ","
                 << pt.write_stall_micros_delta << ","
                 << pt.block_cache_hits_delta << "," << pt.block_cache_misses_delta << "," << pt.block_cache_hit_rate << ","
                 << pt.tombstones_dropped_delta << "," << pt.db_dir_mb << "\n";
        }
    }

    void AppendSummaryCsv(const std::string& csv_file, const std::string& exp_id, const std::string& group_name,
                          const std::string& desc, double main_duration_sec,
                          rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                          const std::string& sha256_hex, size_t ver_ok, size_t ver_nf, bool pass) {
        if (csv_file.empty()) return;
        std::ofstream file(csv_file, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name,desc,main_duration_sec,total_ops,overall_iops,"
                 << "put_count,put_p50_us,put_p95_us,put_p99_us,put_mean_us,"
                 << "get_aff_count,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,get_aff_mean_us,"
                 << "get_ctrl_count,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,get_ctrl_mean_us,"
                 << "scan_count,scan_ops_sec,scan_keys_sec,scan_avg_span,scan_avg_keys_returned,"
                 << "scan_p50_us,scan_p95_us,scan_p99_us,scan_mean_us,scan_us_per_key,"
                 << "del_range_count,del_range_p50_us,del_range_p95_us,del_range_p99_us,del_range_mean_us,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,"
                 << "compaction_read_mb,compaction_write_mb,flush_write_mb,stall_micros,compaction_drop_keys,"
                 << "l0_files_final,pending_compact_mb,total_sst_mb,sha256_checksum,"
                 << "sample_verified_ok,sample_verified_notfound,full_scan_ok\n";
        }

        uint64_t total_ops = main_put_ops_ + main_get_aff_ops_ + main_get_del_ops_ + main_get_ctrl_ops_ + main_scan_ops_ + main_del_range_ops_;
        double overall_iops = (main_duration_sec > 0) ? (total_ops / main_duration_sec) : 0.0;

        double put_p50, put_p95, put_p99, put_mean, put_max; uint64_t put_cnt;
        main_put_lat_.GetQuantiles(put_p50, put_p95, put_p99, put_mean, put_max, put_cnt);

        double get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max; uint64_t get_aff_cnt;
        main_get_aff_lat_.GetQuantiles(get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max, get_aff_cnt);

        double get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max; uint64_t get_ctrl_cnt;
        main_get_ctrl_lat_.GetQuantiles(get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max, get_ctrl_cnt);

        double scan_p50, scan_p95, scan_p99, scan_mean, scan_max; uint64_t scan_cnt;
        main_scan_lat_.GetQuantiles(scan_p50, scan_p95, scan_p99, scan_mean, scan_max, scan_cnt);

        double scan_ops_sec = (main_duration_sec > 0) ? (scan_cnt / main_duration_sec) : 0.0;
        double scan_keys_sec = (main_duration_sec > 0) ? (main_scan_returned_keys_ / main_duration_sec) : 0.0;
        double scan_avg_span = (scan_cnt > 0) ? (static_cast<double>(main_scan_span_requested_) / scan_cnt) : 0.0;
        double scan_avg_returned = (scan_cnt > 0) ? (static_cast<double>(main_scan_returned_keys_) / scan_cnt) : 0.0;
        double scan_us_per_key = (scan_avg_returned > 0) ? (scan_mean / scan_avg_returned) : 0.0;

        double del_p50, del_p95, del_p99, del_mean, del_max; uint64_t del_cnt;
        main_del_range_lat_.GetQuantiles(del_p50, del_p95, del_p99, del_mean, del_max, del_cnt);

        uint64_t cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;
        double hit_rate = (cache_hit + cache_miss > 0) ? (100.0 * cache_hit / (cache_hit + cache_miss)) : 0.0;

        uint64_t comp_read = stats ? (stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) / (1024*1024)) : 0;
        uint64_t comp_write = stats ? (stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) / (1024*1024)) : 0;
        uint64_t flush_write = stats ? (stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) / (1024*1024)) : 0;
        uint64_t stall_us = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t drop_keys = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t l0_files = 0, pending_bytes = 0, total_sst = 0;
        if (db) {
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
            db->GetIntProperty("rocksdb.total-sst-files-size", &total_sst);
        }

        file << exp_id << "," << group_name << ",\"" << desc << "\"," << main_duration_sec << "," << total_ops << "," << overall_iops << ","
             << put_cnt << "," << put_p50 << "," << put_p95 << "," << put_p99 << "," << put_mean << ","
             << get_aff_cnt << "," << get_aff_p50 << "," << get_aff_p95 << "," << get_aff_p99 << "," << get_aff_mean << ","
             << get_ctrl_cnt << "," << get_ctrl_p50 << "," << get_ctrl_p95 << "," << get_ctrl_p99 << "," << get_ctrl_mean << ","
             << scan_cnt << "," << scan_ops_sec << "," << scan_keys_sec << "," << scan_avg_span << "," << scan_avg_returned << ","
             << scan_p50 << "," << scan_p95 << "," << scan_p99 << "," << scan_mean << "," << scan_us_per_key << ","
             << del_cnt << "," << del_p50 << "," << del_p95 << "," << del_p99 << "," << del_mean << ","
             << cache_hit << "," << cache_miss << "," << hit_rate << ","
             << comp_read << "," << comp_write << "," << flush_write << "," << stall_us << "," << drop_keys << ","
             << l0_files << "," << (pending_bytes / (1024*1024)) << "," << (total_sst / (1024*1024)) << ","
             << sha256_hex << ","
             << ver_ok << "," << ver_nf << "," << (pass ? "PASS" : "FAIL") << "\n";
    }

private:
    std::atomic<uint64_t> main_put_ops_;
    std::atomic<uint64_t> main_get_aff_ops_;
    std::atomic<uint64_t> main_get_del_ops_;
    std::atomic<uint64_t> main_get_ctrl_ops_;
    std::atomic<uint64_t> main_scan_ops_;
    std::atomic<uint64_t> main_scan_returned_keys_;
    std::atomic<uint64_t> main_scan_span_requested_;
    std::atomic<uint64_t> main_del_range_ops_;

    P6LatencyTracker main_put_lat_;
    P6LatencyTracker main_get_aff_lat_;
    P6LatencyTracker main_get_del_lat_;
    P6LatencyTracker main_get_ctrl_lat_;
    P6LatencyTracker main_scan_lat_;
    P6LatencyTracker main_del_range_lat_;

    // 1-second interval trackers
    P6LatencyTracker sec_put_lat_;
    P6LatencyTracker sec_get_aff_lat_;
    P6LatencyTracker sec_get_del_lat_;
    P6LatencyTracker sec_get_ctrl_lat_;
    P6LatencyTracker sec_scan_lat_;
    P6LatencyTracker sec_del_range_lat_;
    std::atomic<uint64_t> sec_scan_keys_{0};

    uint64_t last_comp_read_{0};
    uint64_t last_comp_write_{0};
    uint64_t last_flush_write_{0};
    uint64_t last_write_stall_{0};
    uint64_t last_cache_hit_{0};
    uint64_t last_cache_miss_{0};
    uint64_t last_tomb_drop_{0};

    std::mutex ts_mutex_;
    std::vector<P6TimeSeriesPoint> timeseries_records_;
};

} // namespace study::p6
