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

namespace study::tv {

struct TVSnapshotPoint {
    int index;
    double elapsed_sec;
    uint64_t completed_ops;
    double progress_pct;

    // Frontend throughput & latency in interval
    uint64_t get_ctrl_ops;
    double get_ctrl_p50_us, get_ctrl_p95_us, get_ctrl_p99_us;
    uint64_t get_aff_ops;
    double get_aff_p50_us, get_aff_p95_us, get_aff_p99_us;
    uint64_t get_del_ops;
    double get_del_p50_us, get_del_p95_us, get_del_p99_us;
    uint64_t scan_ops;
    double scan_p50_us, scan_p95_us, scan_p99_us;
    double scan_avg_keys;
    double scan_us_per_key;
    uint64_t put_ops;
    double put_p50_us, put_p95_us, put_p99_us;
    uint64_t del_range_ops;
    double del_range_p50_us, del_range_p95_us, del_range_p99_us;
    uint64_t total_ops_interval;
    double instantaneous_iops;

    // RocksDB internal properties
    double active_memtable_mb;
    uint64_t immutable_memtable_count;
    uint64_t memtable_flush_pending;
    uint64_t l0_files;
    uint64_t l1_files;
    uint64_t l2_files;
    double pending_compaction_mb;
    uint64_t running_flushes;
    uint64_t running_compactions;
    double total_sst_mb;

    // Ticker deltas
    uint64_t flush_count_delta;
    uint64_t flush_range_del_reason_delta;
    double compaction_read_mb_delta;
    double compaction_write_mb_delta;
    double flush_write_mb_delta;
    uint64_t write_stall_micros_delta;
    uint64_t block_cache_hits_delta;
    uint64_t block_cache_misses_delta;
    double block_cache_hit_rate;
    uint64_t tombstones_dropped_delta;

    // Cumulative stats
    uint64_t cumulative_range_deletions;
    double db_dir_mb;
};

class TVLatencyTracker {
public:
    TVLatencyTracker() : count_(0), sum_ns_(0), max_ns_(0) {
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

class TVStatsCollector {
public:
    TVStatsCollector()
        : total_put_ops_(0), total_get_aff_ops_(0), total_get_del_ops_(0), total_get_ctrl_ops_(0),
          total_scan_ops_(0), total_scan_returned_keys_(0), total_scan_span_requested_(0),
          total_del_range_ops_(0) {}

    void RecordPut(uint64_t lat_ns) {
        total_put_ops_++;
        main_put_lat_.Record(lat_ns);
        sec_put_lat_.Record(lat_ns);
        prog_put_lat_.Record(lat_ns);
    }

    void RecordGet(int category, uint64_t lat_ns) {
        if (category == 0) {
            total_get_aff_ops_++;
            main_get_aff_lat_.Record(lat_ns);
            sec_get_aff_lat_.Record(lat_ns);
            prog_get_aff_lat_.Record(lat_ns);
        } else if (category == 1) {
            total_get_del_ops_++;
            main_get_del_lat_.Record(lat_ns);
            sec_get_del_lat_.Record(lat_ns);
            prog_get_del_lat_.Record(lat_ns);
        } else {
            total_get_ctrl_ops_++;
            main_get_ctrl_lat_.Record(lat_ns);
            sec_get_ctrl_lat_.Record(lat_ns);
            prog_get_ctrl_lat_.Record(lat_ns);
        }
    }

    void RecordScan(uint64_t lat_ns, uint64_t span_req, size_t returned_keys) {
        total_scan_ops_++;
        total_scan_span_requested_ += span_req;
        total_scan_returned_keys_ += returned_keys;
        main_scan_lat_.Record(lat_ns);
        sec_scan_lat_.Record(lat_ns);
        sec_scan_keys_ += returned_keys;
        prog_scan_lat_.Record(lat_ns);
        prog_scan_keys_ += returned_keys;
    }

    void RecordDeleteRange(uint64_t lat_ns) {
        total_del_range_ops_++;
        main_del_range_lat_.Record(lat_ns);
        sec_del_range_lat_.Record(lat_ns);
        prog_del_range_lat_.Record(lat_ns);
    }

    void CaptureWallClockSecond(int sec_idx, double elapsed_sec, uint64_t completed_ops, double prog_pct,
                                uint64_t cur_del_ranges,
                                rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                const std::string& db_path) {
        TVSnapshotPoint pt = CollectSnapshot(sec_idx, elapsed_sec, completed_ops, prog_pct, cur_del_ranges,
                                             sec_get_ctrl_lat_, sec_get_aff_lat_, sec_get_del_lat_,
                                             sec_scan_lat_, sec_scan_keys_, sec_put_lat_, sec_del_range_lat_,
                                             wall_last_flush_count_, wall_last_flush_range_del_reason_,
                                             wall_last_comp_read_, wall_last_comp_write_, wall_last_flush_write_,
                                             wall_last_write_stall_, wall_last_cache_hit_, wall_last_cache_miss_,
                                             wall_last_tomb_drop_, db, stats, db_path);
        std::lock_guard<std::mutex> lk(wall_mutex_);
        wallclock_records_.push_back(pt);
    }

    void CaptureProgressSnapshot(int prog_pct, double elapsed_sec, uint64_t completed_ops,
                                 uint64_t cur_del_ranges,
                                 rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                 const std::string& db_path) {
        TVSnapshotPoint pt = CollectSnapshot(prog_pct, elapsed_sec, completed_ops, static_cast<double>(prog_pct), cur_del_ranges,
                                             prog_get_ctrl_lat_, prog_get_aff_lat_, prog_get_del_lat_,
                                             prog_scan_lat_, prog_scan_keys_, prog_put_lat_, prog_del_range_lat_,
                                             prog_last_flush_count_, prog_last_flush_range_del_reason_,
                                             prog_last_comp_read_, prog_last_comp_write_, prog_last_flush_write_,
                                             prog_last_write_stall_, prog_last_cache_hit_, prog_last_cache_miss_,
                                             prog_last_tomb_drop_, db, stats, db_path);
        std::lock_guard<std::mutex> lk(prog_mutex_);
        progress_records_.push_back(pt);
    }

    void DumpWallClockCsv(const std::string& filepath, const std::string& exp_id, const std::string& group_name) {
        DumpSnapshotVector(filepath, exp_id, group_name, wallclock_records_, "second_idx");
    }

    void DumpProgressCsv(const std::string& filepath, const std::string& exp_id, const std::string& group_name) {
        DumpSnapshotVector(filepath, exp_id, group_name, progress_records_, "progress_pct_idx");
    }

    void AppendSummaryCsv(const std::string& csv_file, const std::string& exp_id, const std::string& group_name,
                          const std::string& desc, double elapsed_sec, size_t value_size, uint32_t memtable_max_del_ranges,
                          rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                          uint64_t union_deleted_keys, double union_cov_pct, uint64_t tombstones_cnt,
                          const std::string& sha256_hex, size_t ver_ok, size_t ver_nf, bool pass) {
        if (csv_file.empty()) return;
        std::ofstream file(csv_file, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name,desc,value_size,memtable_max_range_deletions,elapsed_sec,total_ops,overall_iops,"
                 << "put_count,put_p50_us,put_p95_us,put_p99_us,put_mean_us,"
                 << "get_aff_count,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,get_aff_mean_us,"
                 << "get_del_count,get_del_p50_us,get_del_p95_us,get_del_p99_us,get_del_mean_us,"
                 << "get_ctrl_count,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,get_ctrl_mean_us,"
                 << "scan_count,scan_ops_sec,scan_keys_sec,scan_avg_keys_returned,scan_p50_us,scan_p95_us,scan_p99_us,scan_mean_us,scan_us_per_key,"
                 << "del_range_count,del_range_p50_us,del_range_p95_us,del_range_p99_us,del_range_mean_us,"
                 << "tombstones_count,union_deleted_keys,union_coverage_ratio_pct,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,"
                 << "flush_count_total,flush_reason_range_del_count,flush_write_mb,"
                 << "compaction_read_mb,compaction_write_mb,stall_micros,compaction_drop_keys,"
                 << "write_amplification,l0_files_final,l1_files_final,l2_files_final,pending_compact_mb,total_sst_mb,sha256_checksum,"
                 << "sample_verified_ok,sample_verified_notfound,full_scan_ok\n";
        }

        uint64_t total_ops = total_put_ops_ + total_get_aff_ops_ + total_get_del_ops_ + total_get_ctrl_ops_ + total_scan_ops_ + total_del_range_ops_;
        double overall_iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;

        double put_p50, put_p95, put_p99, put_mean, put_max; uint64_t put_cnt;
        main_put_lat_.GetQuantiles(put_p50, put_p95, put_p99, put_mean, put_max, put_cnt);

        double get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max; uint64_t get_aff_cnt;
        main_get_aff_lat_.GetQuantiles(get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max, get_aff_cnt);

        double get_del_p50, get_del_p95, get_del_p99, get_del_mean, get_del_max; uint64_t get_del_cnt;
        main_get_del_lat_.GetQuantiles(get_del_p50, get_del_p95, get_del_p99, get_del_mean, get_del_max, get_del_cnt);

        double get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max; uint64_t get_ctrl_cnt;
        main_get_ctrl_lat_.GetQuantiles(get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max, get_ctrl_cnt);

        double scan_p50, scan_p95, scan_p99, scan_mean, scan_max; uint64_t scan_cnt;
        main_scan_lat_.GetQuantiles(scan_p50, scan_p95, scan_p99, scan_mean, scan_max, scan_cnt);

        double scan_ops_sec = (elapsed_sec > 0) ? (scan_cnt / elapsed_sec) : 0.0;
        double scan_keys_sec = (elapsed_sec > 0) ? (total_scan_returned_keys_ / elapsed_sec) : 0.0;
        double scan_avg_returned = (scan_cnt > 0) ? (static_cast<double>(total_scan_returned_keys_) / scan_cnt) : 0.0;
        double scan_us_per_key = (scan_avg_returned > 0) ? (scan_mean / scan_avg_returned) : 0.0;

        double del_p50, del_p95, del_p99, del_mean, del_max; uint64_t del_cnt;
        main_del_range_lat_.GetQuantiles(del_p50, del_p95, del_p99, del_mean, del_max, del_cnt);

        uint64_t cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;
        double hit_rate = (cache_hit + cache_miss > 0) ? (100.0 * cache_hit / (cache_hit + cache_miss)) : 0.0;

        uint64_t flush_count_total = stats ? (stats->getTickerCount(rocksdb::Tickers::FLUSH_REASON_WRITE_BUFFER_FULL) +
                                               stats->getTickerCount(rocksdb::Tickers::FLUSH_REASON_MEMTABLE_MAX_RANGE_DELETIONS)) : 0;
        uint64_t flush_range_del_count = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_REASON_MEMTABLE_MAX_RANGE_DELETIONS) : 0;
        uint64_t flush_write_bytes = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) : 0;
        uint64_t comp_read_bytes = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) : 0;
        uint64_t comp_write_bytes = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) : 0;
        uint64_t stall_us = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t drop_keys = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t foreground_put_logical_bytes = put_cnt * value_size;
        double write_amp = (foreground_put_logical_bytes > 0) ? (static_cast<double>(comp_write_bytes) / foreground_put_logical_bytes) : 0.0;

        uint64_t l0_files = 0, l1_files = 0, l2_files = 0, pending_bytes = 0, total_sst = 0;
        if (db) {
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.num-files-at-level1", &l1_files);
            db->GetIntProperty("rocksdb.num-files-at-level2", &l2_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
            db->GetIntProperty("rocksdb.total-sst-files-size", &total_sst);
        }

        file << exp_id << "," << group_name << ",\"" << desc << "\"," << value_size << "," << memtable_max_del_ranges << ","
             << elapsed_sec << "," << total_ops << "," << overall_iops << ","
             << put_cnt << "," << put_p50 << "," << put_p95 << "," << put_p99 << "," << put_mean << ","
             << get_aff_cnt << "," << get_aff_p50 << "," << get_aff_p95 << "," << get_aff_p99 << "," << get_aff_mean << ","
             << get_del_cnt << "," << get_del_p50 << "," << get_del_p95 << "," << get_del_p99 << "," << get_del_mean << ","
             << get_ctrl_cnt << "," << get_ctrl_p50 << "," << get_ctrl_p95 << "," << get_ctrl_p99 << "," << get_ctrl_mean << ","
             << scan_cnt << "," << scan_ops_sec << "," << scan_keys_sec << "," << scan_avg_returned << "," << scan_p50 << "," << scan_p95 << "," << scan_p99 << "," << scan_mean << "," << scan_us_per_key << ","
             << del_cnt << "," << del_p50 << "," << del_p95 << "," << del_p99 << "," << del_mean << ","
             << tombstones_cnt << "," << union_deleted_keys << "," << union_cov_pct << ","
             << cache_hit << "," << cache_miss << "," << hit_rate << ","
             << flush_count_total << "," << flush_range_del_count << "," << (flush_write_bytes / (1024*1024)) << ","
             << (comp_read_bytes / (1024*1024)) << "," << (comp_write_bytes / (1024*1024)) << "," << stall_us << "," << drop_keys << ","
             << write_amp << ","
             << l0_files << "," << l1_files << "," << l2_files << ","
             << (pending_bytes / (1024*1024)) << "," << (total_sst / (1024*1024)) << ","
             << sha256_hex << ","
             << ver_ok << "," << ver_nf << "," << (pass ? "PASS" : "FAIL") << "\n";
    }

private:
    TVSnapshotPoint CollectSnapshot(int index, double elapsed_sec, uint64_t completed_ops, double prog_pct,
                                    uint64_t cur_del_ranges,
                                    TVLatencyTracker& ctrl_lat, TVLatencyTracker& aff_lat, TVLatencyTracker& del_lat,
                                    TVLatencyTracker& scan_lat, std::atomic<uint64_t>& scan_keys,
                                    TVLatencyTracker& put_lat, TVLatencyTracker& del_range_lat,
                                    uint64_t& last_flush_count, uint64_t& last_flush_range_del_reason,
                                    uint64_t& last_comp_read, uint64_t& last_comp_write, uint64_t& last_flush_write,
                                    uint64_t& last_write_stall, uint64_t& last_cache_hit, uint64_t& last_cache_miss,
                                    uint64_t& last_tomb_drop,
                                    rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                    const std::string& db_path) {
        uint64_t cur_flush_range_del = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_REASON_MEMTABLE_MAX_RANGE_DELETIONS) : 0;
        uint64_t cur_comp_read = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) : 0;
        uint64_t cur_comp_write = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) : 0;
        uint64_t cur_flush_write = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) : 0;
        uint64_t cur_write_stall = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t cur_cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cur_cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;
        uint64_t cur_tomb_drop = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t flush_range_del_delta = cur_flush_range_del - last_flush_range_del_reason;
        uint64_t comp_read_delta = cur_comp_read - last_comp_read;
        uint64_t comp_write_delta = cur_comp_write - last_comp_write;
        uint64_t flush_write_delta = cur_flush_write - last_flush_write;
        uint64_t write_stall_delta = cur_write_stall - last_write_stall;
        uint64_t cache_hit_delta = cur_cache_hit - last_cache_hit;
        uint64_t cache_miss_delta = cur_cache_miss - last_cache_miss;
        uint64_t tomb_drop_delta = cur_tomb_drop - last_tomb_drop;

        last_flush_range_del_reason = cur_flush_range_del;
        last_comp_read = cur_comp_read;
        last_comp_write = cur_comp_write;
        last_flush_write = cur_flush_write;
        last_write_stall = cur_write_stall;
        last_cache_hit = cur_cache_hit;
        last_cache_miss = cur_cache_miss;
        last_tomb_drop = cur_tomb_drop;

        uint64_t active_memtable_bytes = 0, num_immutable_memtables = 0, memtable_flush_pending = 0;
        uint64_t l0_files = 0, l1_files = 0, l2_files = 0, pending_compaction_bytes = 0;
        uint64_t running_flushes = 0, running_compactions = 0, total_sst_bytes = 0;

        if (db) {
            db->GetIntProperty("rocksdb.cur-size-active-mem-table", &active_memtable_bytes);
            db->GetIntProperty("rocksdb.num-immutable-mem-table", &num_immutable_memtables);
            db->GetIntProperty("rocksdb.mem-table-flush-pending", &memtable_flush_pending);
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.num-files-at-level1", &l1_files);
            db->GetIntProperty("rocksdb.num-files-at-level2", &l2_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_compaction_bytes);
            db->GetIntProperty("rocksdb.num-running-flushes", &running_flushes);
            db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
            db->GetIntProperty("rocksdb.total-sst-files-size", &total_sst_bytes);
        }

        double p50, p95, p99, mean, max_us;
        uint64_t ctrl_cnt, aff_cnt, del_cnt, scan_cnt, put_cnt, del_range_cnt;

        ctrl_lat.GetQuantiles(p50, p95, p99, mean, max_us, ctrl_cnt);
        double ctrl_p50 = p50, ctrl_p95 = p95, ctrl_p99 = p99;
        ctrl_lat.Clear();

        aff_lat.GetQuantiles(p50, p95, p99, mean, max_us, aff_cnt);
        double aff_p50 = p50, aff_p95 = p95, aff_p99 = p99;
        aff_lat.Clear();

        del_lat.GetQuantiles(p50, p95, p99, mean, max_us, del_cnt);
        double get_del_p50 = p50, get_del_p95 = p95, get_del_p99 = p99;
        del_lat.Clear();

        scan_lat.GetQuantiles(p50, p95, p99, mean, max_us, scan_cnt);
        double scan_p50 = p50, scan_p95 = p95, scan_p99 = p99;
        uint64_t sec_keys = scan_keys.exchange(0);
        double scan_avg_keys = (scan_cnt > 0) ? (static_cast<double>(sec_keys) / scan_cnt) : 0.0;
        double scan_us_per_key = (sec_keys > 0) ? ((mean * scan_cnt) / sec_keys) : 0.0;
        scan_lat.Clear();

        put_lat.GetQuantiles(p50, p95, p99, mean, max_us, put_cnt);
        double put_p50 = p50, put_p95 = p95, put_p99 = p99;
        put_lat.Clear();

        del_range_lat.GetQuantiles(p50, p95, p99, mean, max_us, del_range_cnt);
        double del_r_p50 = p50, del_r_p95 = p95, del_r_p99 = p99;
        del_range_lat.Clear();

        uint64_t interval_total_ops = ctrl_cnt + aff_cnt + del_cnt + scan_cnt + put_cnt + del_range_cnt;
        double hit_rate = (cache_hit_delta + cache_miss_delta > 0) ? (100.0 * cache_hit_delta / (cache_hit_delta + cache_miss_delta)) : 0.0;

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

        TVSnapshotPoint pt;
        pt.index = index;
        pt.elapsed_sec = elapsed_sec;
        pt.completed_ops = completed_ops;
        pt.progress_pct = prog_pct;

        pt.get_ctrl_ops = ctrl_cnt; pt.get_ctrl_p50_us = ctrl_p50; pt.get_ctrl_p95_us = ctrl_p95; pt.get_ctrl_p99_us = ctrl_p99;
        pt.get_aff_ops = aff_cnt; pt.get_aff_p50_us = aff_p50; pt.get_aff_p95_us = aff_p95; pt.get_aff_p99_us = aff_p99;
        pt.get_del_ops = del_cnt; pt.get_del_p50_us = get_del_p50; pt.get_del_p95_us = get_del_p95; pt.get_del_p99_us = get_del_p99;
        pt.scan_ops = scan_cnt; pt.scan_p50_us = scan_p50; pt.scan_p95_us = scan_p95; pt.scan_p99_us = scan_p99;
        pt.scan_avg_keys = scan_avg_keys; pt.scan_us_per_key = scan_us_per_key;
        pt.put_ops = put_cnt; pt.put_p50_us = put_p50; pt.put_p95_us = put_p95; pt.put_p99_us = put_p99;
        pt.del_range_ops = del_range_cnt; pt.del_range_p50_us = del_r_p50; pt.del_range_p95_us = del_r_p95; pt.del_range_p99_us = del_r_p99;
        pt.total_ops_interval = interval_total_ops;
        pt.instantaneous_iops = static_cast<double>(interval_total_ops);

        pt.active_memtable_mb = active_memtable_bytes / (1024.0 * 1024.0);
        pt.immutable_memtable_count = num_immutable_memtables;
        pt.memtable_flush_pending = memtable_flush_pending;
        pt.l0_files = l0_files; pt.l1_files = l1_files; pt.l2_files = l2_files;
        pt.pending_compaction_mb = pending_compaction_bytes / (1024.0 * 1024.0);
        pt.running_flushes = running_flushes; pt.running_compactions = running_compactions;
        pt.total_sst_mb = total_sst_bytes / (1024.0 * 1024.0);

        pt.flush_count_delta = 0;
        pt.flush_range_del_reason_delta = flush_range_del_delta;
        pt.compaction_read_mb_delta = comp_read_delta / (1024.0 * 1024.0);
        pt.compaction_write_mb_delta = comp_write_delta / (1024.0 * 1024.0);
        pt.flush_write_mb_delta = flush_write_delta / (1024.0 * 1024.0);
        pt.write_stall_micros_delta = write_stall_delta;
        pt.block_cache_hits_delta = cache_hit_delta;
        pt.block_cache_misses_delta = cache_miss_delta;
        pt.block_cache_hit_rate = hit_rate;
        pt.tombstones_dropped_delta = tomb_drop_delta;
        pt.cumulative_range_deletions = cur_del_ranges;
        pt.db_dir_mb = db_dir_mb;

        return pt;
    }

    void DumpSnapshotVector(const std::string& filepath, const std::string& exp_id, const std::string& group_name,
                            const std::vector<TVSnapshotPoint>& records, const std::string& index_col_name) {
        if (filepath.empty()) return;
        std::ofstream file(filepath, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name," << index_col_name << ",elapsed_sec,completed_ops,progress_pct,"
                 << "get_ctrl_ops,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,"
                 << "get_aff_ops,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,"
                 << "get_del_ops,get_del_p50_us,get_del_p95_us,get_del_p99_us,"
                 << "scan_ops,scan_p50_us,scan_p95_us,scan_p99_us,scan_avg_keys,scan_us_per_key,"
                 << "put_ops,put_p50_us,put_p95_us,put_p99_us,"
                 << "del_range_ops,del_range_p50_us,del_range_p95_us,del_range_p99_us,total_ops_interval,instantaneous_iops,"
                 << "active_memtable_mb,immutable_memtable_count,memtable_flush_pending,l0_files,l1_files,l2_files,pending_compaction_mb,"
                 << "running_flushes,running_compactions,total_sst_mb,"
                 << "flush_range_del_reason_delta,compaction_read_mb_delta,compaction_write_mb_delta,flush_write_mb_delta,write_stall_micros_delta,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,tombstones_dropped_delta,cumulative_range_deletions,db_dir_mb\n";
        }

        for (const auto& pt : records) {
            file << exp_id << "," << group_name << "," << pt.index << "," << pt.elapsed_sec << "," << pt.completed_ops << "," << pt.progress_pct << ","
                 << pt.get_ctrl_ops << "," << pt.get_ctrl_p50_us << "," << pt.get_ctrl_p95_us << "," << pt.get_ctrl_p99_us << ","
                 << pt.get_aff_ops << "," << pt.get_aff_p50_us << "," << pt.get_aff_p95_us << "," << pt.get_aff_p99_us << ","
                 << pt.get_del_ops << "," << pt.get_del_p50_us << "," << pt.get_del_p95_us << "," << pt.get_del_p99_us << ","
                 << pt.scan_ops << "," << pt.scan_p50_us << "," << pt.scan_p95_us << "," << pt.scan_p99_us << ","
                 << pt.scan_avg_keys << "," << pt.scan_us_per_key << ","
                 << pt.put_ops << "," << pt.put_p50_us << "," << pt.put_p95_us << "," << pt.put_p99_us << ","
                 << pt.del_range_ops << "," << pt.del_range_p50_us << "," << pt.del_range_p95_us << "," << pt.del_range_p99_us << ","
                 << pt.total_ops_interval << "," << pt.instantaneous_iops << ","
                 << pt.active_memtable_mb << "," << pt.immutable_memtable_count << "," << pt.memtable_flush_pending << ","
                 << pt.l0_files << "," << pt.l1_files << "," << pt.l2_files << "," << pt.pending_compaction_mb << ","
                 << pt.running_flushes << "," << pt.running_compactions << "," << pt.total_sst_mb << ","
                 << pt.flush_range_del_reason_delta << "," << pt.compaction_read_mb_delta << "," << pt.compaction_write_mb_delta << "," << pt.flush_write_mb_delta << ","
                 << pt.write_stall_micros_delta << ","
                 << pt.block_cache_hits_delta << "," << pt.block_cache_misses_delta << "," << pt.block_cache_hit_rate << ","
                 << pt.tombstones_dropped_delta << "," << pt.cumulative_range_deletions << "," << pt.db_dir_mb << "\n";
        }
    }

private:
    std::atomic<uint64_t> total_put_ops_;
    std::atomic<uint64_t> total_get_aff_ops_;
    std::atomic<uint64_t> total_get_del_ops_;
    std::atomic<uint64_t> total_get_ctrl_ops_;
    std::atomic<uint64_t> total_scan_ops_;
    std::atomic<uint64_t> total_scan_returned_keys_;
    std::atomic<uint64_t> total_scan_span_requested_;
    std::atomic<uint64_t> total_del_range_ops_;

    TVLatencyTracker main_put_lat_;
    TVLatencyTracker main_get_aff_lat_;
    TVLatencyTracker main_get_del_lat_;
    TVLatencyTracker main_get_ctrl_lat_;
    TVLatencyTracker main_scan_lat_;
    TVLatencyTracker main_del_range_lat_;

    // Wall-clock interval trackers
    TVLatencyTracker sec_put_lat_, sec_get_aff_lat_, sec_get_del_lat_, sec_get_ctrl_lat_, sec_scan_lat_, sec_del_range_lat_;
    std::atomic<uint64_t> sec_scan_keys_{0};
    uint64_t wall_last_flush_count_{0}, wall_last_flush_range_del_reason_{0};
    uint64_t wall_last_comp_read_{0}, wall_last_comp_write_{0}, wall_last_flush_write_{0};
    uint64_t wall_last_write_stall_{0}, wall_last_cache_hit_{0}, wall_last_cache_miss_{0}, wall_last_tomb_drop_{0};
    std::mutex wall_mutex_;
    std::vector<TVSnapshotPoint> wallclock_records_;

    // Progress interval trackers
    TVLatencyTracker prog_put_lat_, prog_get_aff_lat_, prog_get_del_lat_, prog_get_ctrl_lat_, prog_scan_lat_, prog_del_range_lat_;
    std::atomic<uint64_t> prog_scan_keys_{0};
    uint64_t prog_last_flush_count_{0}, prog_last_flush_range_del_reason_{0};
    uint64_t prog_last_comp_read_{0}, prog_last_comp_write_{0}, prog_last_flush_write_{0};
    uint64_t prog_last_write_stall_{0}, prog_last_cache_hit_{0}, prog_last_cache_miss_{0}, prog_last_tomb_drop_{0};
    std::mutex prog_mutex_;
    std::vector<TVSnapshotPoint> progress_records_;
};

} // namespace study::tv
