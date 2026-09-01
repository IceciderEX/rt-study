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
#include "rocksdb/statistics.h"
#include "rocksdb/db.h"

namespace study::supp {

struct TimeSeriesPoint {
    int second_idx;
    uint64_t get_ctrl_ops;
    double get_ctrl_p99_us;
    uint64_t get_aff_ops;
    double get_aff_p99_us;
    uint64_t scan_ops;
    double scan_p99_us;
    double scan_us_per_key;
    uint64_t put_ops;
    double put_p99_us;
    uint64_t compaction_read_bytes_delta;
    uint64_t compaction_write_bytes_delta;
    uint64_t write_stall_micros_delta;
    uint64_t l0_files;
    uint64_t pending_compaction_bytes;
    uint64_t block_cache_hits_delta;
    uint64_t block_cache_misses_delta;
    bool is_reclaiming;
};

class LatencyTracker {
public:
    LatencyTracker() : count_(0), sum_ns_(0), max_ns_(0) {
        samples_.reserve(200000);
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

    std::vector<uint64_t> GetAndClearSamples() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint64_t> ret = std::move(samples_);
        samples_.clear();
        count_ = 0;
        sum_ns_ = 0;
        max_ns_ = 0;
        return ret;
    }

private:
    std::mutex mutex_;
    std::vector<uint64_t> samples_;
    uint64_t count_;
    uint64_t sum_ns_;
    uint64_t max_ns_;
};

class SuppStatsCollector {
public:
    SuppStatsCollector()
        : total_put_ops_(0), total_get_aff_ops_(0), total_get_del_ops_(0), total_get_ctrl_ops_(0),
          total_scan_ops_(0), total_scan_returned_keys_(0), total_scan_span_requested_(0),
          total_del_range_ops_(0) {}

    void RecordPut(uint64_t lat_ns) {
        total_put_ops_++;
        put_lat_.Record(lat_ns);
        sec_put_lat_.Record(lat_ns);
    }

    void RecordGet(int category, uint64_t lat_ns) {
        if (category == 0) { // Affected-Live
            total_get_aff_ops_++;
            get_aff_lat_.Record(lat_ns);
            sec_get_aff_lat_.Record(lat_ns);
        } else if (category == 1) { // Deleted
            total_get_del_ops_++;
            get_del_lat_.Record(lat_ns);
            sec_get_del_lat_.Record(lat_ns);
        } else { // Control
            total_get_ctrl_ops_++;
            get_ctrl_lat_.Record(lat_ns);
            sec_get_ctrl_lat_.Record(lat_ns);
        }
    }

    void RecordScan(uint64_t lat_ns, uint64_t span_req, size_t returned_keys) {
        total_scan_ops_++;
        total_scan_span_requested_ += span_req;
        total_scan_returned_keys_ += returned_keys;
        scan_lat_.Record(lat_ns);
        sec_scan_lat_.Record(lat_ns);
        sec_scan_keys_ += returned_keys;
    }

    void RecordDeleteRange(uint64_t lat_ns) {
        total_del_range_ops_++;
        del_range_lat_.Record(lat_ns);
    }

    // Capture 1-second interval sample for time series
    void CaptureTimeSeriesSecond(int sec_idx, rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats, bool is_reclaim) {
        uint64_t cur_compact_read = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) : 0;
        uint64_t cur_compact_write = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) : 0;
        uint64_t cur_write_stall = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t cur_cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cur_cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;

        uint64_t compact_read_delta = cur_compact_read - last_compact_read_;
        uint64_t compact_write_delta = cur_compact_write - last_compact_write_;
        uint64_t write_stall_delta = cur_write_stall - last_write_stall_;
        uint64_t cache_hit_delta = cur_cache_hit - last_cache_hit_;
        uint64_t cache_miss_delta = cur_cache_miss - last_cache_miss_;

        last_compact_read_ = cur_compact_read;
        last_compact_write_ = cur_compact_write;
        last_write_stall_ = cur_write_stall;
        last_cache_hit_ = cur_cache_hit;
        last_cache_miss_ = cur_cache_miss;

        uint64_t l0_files = 0;
        uint64_t pending_compact_bytes = 0;
        if (db) {
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_compact_bytes);
        }

        double p50, p95, p99, mean, max_us;
        uint64_t ctrl_cnt, aff_cnt, scan_cnt, put_cnt;

        sec_get_ctrl_lat_.GetQuantiles(p50, p95, p99, mean, max_us, ctrl_cnt);
        double ctrl_p99 = p99;
        sec_get_ctrl_lat_.Clear();

        sec_get_aff_lat_.GetQuantiles(p50, p95, p99, mean, max_us, aff_cnt);
        double aff_p99 = p99;
        sec_get_aff_lat_.Clear();

        sec_put_lat_.GetQuantiles(p50, p95, p99, mean, max_us, put_cnt);
        double put_p99 = p99;
        sec_put_lat_.Clear();

        sec_scan_lat_.GetQuantiles(p50, p95, p99, mean, max_us, scan_cnt);
        double scan_p99 = p99;
        uint64_t sec_keys = sec_scan_keys_.exchange(0);
        double scan_us_per_key = (sec_keys > 0) ? ((mean * scan_cnt) / sec_keys) : 0.0;
        sec_scan_lat_.Clear();

        TimeSeriesPoint pt;
        pt.second_idx = sec_idx;
        pt.get_ctrl_ops = ctrl_cnt;
        pt.get_ctrl_p99_us = ctrl_p99;
        pt.get_aff_ops = aff_cnt;
        pt.get_aff_p99_us = aff_p99;
        pt.scan_ops = scan_cnt;
        pt.scan_p99_us = scan_p99;
        pt.scan_us_per_key = scan_us_per_key;
        pt.put_ops = put_cnt;
        pt.put_p99_us = put_p99;
        pt.compaction_read_bytes_delta = compact_read_delta;
        pt.compaction_write_bytes_delta = compact_write_delta;
        pt.write_stall_micros_delta = write_stall_delta;
        pt.l0_files = l0_files;
        pt.pending_compaction_bytes = pending_compact_bytes;
        pt.block_cache_hits_delta = cache_hit_delta;
        pt.block_cache_misses_delta = cache_miss_delta;
        pt.is_reclaiming = is_reclaim;

        std::lock_guard<std::mutex> lk(ts_mutex_);
        timeseries_records_.push_back(pt);
    }

    void DumpTimeSeriesCsv(const std::string& filepath, const std::string& exp_id) {
        if (filepath.empty()) return;
        std::ofstream file(filepath, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,second_idx,get_ctrl_ops,get_ctrl_p99_us,get_aff_ops,get_aff_p99_us,"
                 << "scan_ops,scan_p99_us,scan_us_per_key,put_ops,put_p99_us,"
                 << "compaction_read_mb_delta,compaction_write_mb_delta,write_stall_micros_delta,"
                 << "l0_files,pending_compact_mb,block_cache_hits,block_cache_misses,is_reclaiming\n";
        }

        std::lock_guard<std::mutex> lk(ts_mutex_);
        for (const auto& pt : timeseries_records_) {
            file << exp_id << ","
                 << pt.second_idx << ","
                 << pt.get_ctrl_ops << ","
                 << pt.get_ctrl_p99_us << ","
                 << pt.get_aff_ops << ","
                 << pt.get_aff_p99_us << ","
                 << pt.scan_ops << ","
                 << pt.scan_p99_us << ","
                 << pt.scan_us_per_key << ","
                 << pt.put_ops << ","
                 << pt.put_p99_us << ","
                 << (pt.compaction_read_bytes_delta / (1024.0 * 1024.0)) << ","
                 << (pt.compaction_write_bytes_delta / (1024.0 * 1024.0)) << ","
                 << pt.write_stall_micros_delta << ","
                 << pt.l0_files << ","
                 << (pt.pending_compaction_bytes / (1024.0 * 1024.0)) << ","
                 << pt.block_cache_hits_delta << ","
                 << pt.block_cache_misses_delta << ","
                 << (pt.is_reclaiming ? 1 : 0) << "\n";
        }
    }

    void PrintSummary(double elapsed_sec, rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats) {
        std::cout << "\n================ SUPPLEMENT BENCHMARK SUMMARY ================\n";
        std::cout << "Elapsed Time: " << std::fixed << std::setprecision(3) << elapsed_sec << " s\n";
        uint64_t total_ops = total_put_ops_ + total_get_aff_ops_ + total_get_del_ops_ + total_get_ctrl_ops_ + total_scan_ops_ + total_del_range_ops_;
        double overall_iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;
        std::cout << "Total Operations: " << total_ops << " (Overall IOPS: " << std::setprecision(1) << overall_iops << " ops/s)\n\n";

        double p50, p95, p99, mean, max_us;
        uint64_t cnt;

        put_lat_.GetQuantiles(p50, p95, p99, mean, max_us, cnt);
        if (cnt > 0) {
            std::cout << "  Put                 Count=" << std::setw(8) << cnt << " P50=" << std::setw(6) << p50 << "us P95=" << std::setw(7) << p95 << "us P99=" << std::setw(7) << p99 << "us Mean=" << mean << "us\n";
        }

        get_aff_lat_.GetQuantiles(p50, p95, p99, mean, max_us, cnt);
        if (cnt > 0) {
            std::cout << "  Get (Affected-Live) Count=" << std::setw(8) << cnt << " P50=" << std::setw(6) << p50 << "us P95=" << std::setw(7) << p95 << "us P99=" << std::setw(7) << p99 << "us Mean=" << mean << "us\n";
        }

        get_del_lat_.GetQuantiles(p50, p95, p99, mean, max_us, cnt);
        if (cnt > 0) {
            std::cout << "  Get (Deleted)       Count=" << std::setw(8) << cnt << " P50=" << std::setw(6) << p50 << "us P95=" << std::setw(7) << p95 << "us P99=" << std::setw(7) << p99 << "us Mean=" << mean << "us\n";
        }

        get_ctrl_lat_.GetQuantiles(p50, p95, p99, mean, max_us, cnt);
        if (cnt > 0) {
            std::cout << "  Get (Control)       Count=" << std::setw(8) << cnt << " P50=" << std::setw(6) << p50 << "us P95=" << std::setw(7) << p95 << "us P99=" << std::setw(7) << p99 << "us Mean=" << mean << "us\n";
        }

        scan_lat_.GetQuantiles(p50, p95, p99, mean, max_us, cnt);
        if (cnt > 0) {
            double avg_ret_keys = static_cast<double>(total_scan_returned_keys_) / cnt;
            double us_per_key = (avg_ret_keys > 0) ? (mean / avg_ret_keys) : 0.0;
            std::cout << "  RangeScan           Count=" << std::setw(8) << cnt
                      << " AvgKeys=" << std::setprecision(1) << avg_ret_keys
                      << " P50=" << std::setprecision(2) << p50 << "us P95=" << p95 << "us P99=" << p99 << "us Mean=" << mean << "us\n";
            std::cout << "  --> Normalized Cost Per Key: " << std::setprecision(3) << us_per_key << " us/key\n";
        }

        if (stats) {
            std::cout << "--- Cache & Compaction Stats ---\n";
            uint64_t cache_hit = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT);
            uint64_t cache_miss = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS);
            double hit_rate = (cache_hit + cache_miss > 0) ? (100.0 * cache_hit / (cache_hit + cache_miss)) : 0.0;
            std::cout << "  Block Cache: Hits=" << cache_hit << ", Misses=" << cache_miss << " (Hit Rate: " << std::setprecision(2) << hit_rate << "%)\n";
            std::cout << "  Compaction Read: " << (stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) / (1024*1024)) << " MB, Write: "
                      << (stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) / (1024*1024)) << " MB\n";
            std::cout << "  Write Stall: " << stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) << " us\n";
        }
        std::cout << "===============================================================\n";
    }

    void AppendSummaryCsv(const std::string& csv_file, const std::string& exp_id, const std::string& desc,
                          double elapsed_sec, uint64_t total_keys, size_t val_size, const std::string& mode,
                          rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                          uint64_t verified_ok, uint64_t verified_nf, bool full_scan_pass) {
        if (csv_file.empty()) return;
        std::ofstream file(csv_file, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,desc,preload_mode,elapsed_sec,total_ops,overall_iops,"
                 << "put_count,put_p50_us,put_p95_us,put_p99_us,put_mean_us,"
                 << "get_aff_count,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,get_aff_mean_us,"
                 << "get_del_count,get_del_p50_us,get_del_p95_us,get_del_p99_us,get_del_mean_us,"
                 << "get_ctrl_count,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,get_ctrl_mean_us,"
                 << "scan_count,scan_ops_sec,scan_keys_sec,scan_avg_span,scan_avg_keys_returned,scan_p50_us,scan_p95_us,scan_p99_us,scan_mean_us,scan_us_per_key,"
                 << "del_range_count,del_range_p50_us,del_range_p95_us,del_range_p99_us,del_range_mean_us,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,"
                 << "compaction_read_mb,compaction_write_mb,flush_write_mb,stall_micros,compaction_drop_keys,"
                 << "l0_files_final,pending_compact_mb,total_sst_mb,"
                 << "sample_verified_ok,sample_verified_notfound,full_scan_ok\n";
        }

        uint64_t total_ops = total_put_ops_ + total_get_aff_ops_ + total_get_del_ops_ + total_get_ctrl_ops_ + total_scan_ops_ + total_del_range_ops_;
        double overall_iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;

        double put_p50, put_p95, put_p99, put_mean, put_max; uint64_t put_cnt;
        put_lat_.GetQuantiles(put_p50, put_p95, put_p99, put_mean, put_max, put_cnt);

        double get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max; uint64_t get_aff_cnt;
        get_aff_lat_.GetQuantiles(get_aff_p50, get_aff_p95, get_aff_p99, get_aff_mean, get_aff_max, get_aff_cnt);

        double get_del_p50, get_del_p95, get_del_p99, get_del_mean, get_del_max; uint64_t get_del_cnt;
        get_del_lat_.GetQuantiles(get_del_p50, get_del_p95, get_del_p99, get_del_mean, get_del_max, get_del_cnt);

        double get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max; uint64_t get_ctrl_cnt;
        get_ctrl_lat_.GetQuantiles(get_ctrl_p50, get_ctrl_p95, get_ctrl_p99, get_ctrl_mean, get_ctrl_max, get_ctrl_cnt);

        double scan_p50, scan_p95, scan_p99, scan_mean, scan_max; uint64_t scan_cnt;
        scan_lat_.GetQuantiles(scan_p50, scan_p95, scan_p99, scan_mean, scan_max, scan_cnt);

        double scan_ops_sec = (elapsed_sec > 0) ? (scan_cnt / elapsed_sec) : 0.0;
        double scan_keys_sec = (elapsed_sec > 0) ? (total_scan_returned_keys_ / elapsed_sec) : 0.0;
        double scan_avg_span = (scan_cnt > 0) ? (static_cast<double>(total_scan_span_requested_) / scan_cnt) : 0.0;
        double scan_avg_returned = (scan_cnt > 0) ? (static_cast<double>(total_scan_returned_keys_) / scan_cnt) : 0.0;
        double scan_us_per_key = (scan_avg_returned > 0) ? (scan_mean / scan_avg_returned) : 0.0;

        double del_p50, del_p95, del_p99, del_mean, del_max; uint64_t del_cnt;
        del_range_lat_.GetQuantiles(del_p50, del_p95, del_p99, del_mean, del_max, del_cnt);

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

        file << exp_id << ",\"" << desc << "\"," << mode << "," << elapsed_sec << "," << total_ops << "," << overall_iops << ","
             << put_cnt << "," << put_p50 << "," << put_p95 << "," << put_p99 << "," << put_mean << ","
             << get_aff_cnt << "," << get_aff_p50 << "," << get_aff_p95 << "," << get_aff_p99 << "," << get_aff_mean << ","
             << get_del_cnt << "," << get_del_p50 << "," << get_del_p95 << "," << get_del_p99 << "," << get_del_mean << ","
             << get_ctrl_cnt << "," << get_ctrl_p50 << "," << get_ctrl_p95 << "," << get_ctrl_p99 << "," << get_ctrl_mean << ","
             << scan_cnt << "," << scan_ops_sec << "," << scan_keys_sec << "," << scan_avg_span << "," << scan_avg_returned << ","
             << scan_p50 << "," << scan_p95 << "," << scan_p99 << "," << scan_mean << "," << scan_us_per_key << ","
             << del_cnt << "," << del_p50 << "," << del_p95 << "," << del_p99 << "," << del_mean << ","
             << cache_hit << "," << cache_miss << "," << hit_rate << ","
             << comp_read << "," << comp_write << "," << flush_write << "," << stall_us << "," << drop_keys << ","
             << l0_files << "," << (pending_bytes / (1024*1024)) << "," << (total_sst / (1024*1024)) << ","
             << verified_ok << "," << verified_nf << "," << (full_scan_pass ? "PASS" : "FAIL") << "\n";
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

    LatencyTracker put_lat_;
    LatencyTracker get_aff_lat_;
    LatencyTracker get_del_lat_;
    LatencyTracker get_ctrl_lat_;
    LatencyTracker scan_lat_;
    LatencyTracker del_range_lat_;

    // 1-second interval trackers
    LatencyTracker sec_put_lat_;
    LatencyTracker sec_get_aff_lat_;
    LatencyTracker sec_get_del_lat_;
    LatencyTracker sec_get_ctrl_lat_;
    LatencyTracker sec_scan_lat_;
    std::atomic<uint64_t> sec_scan_keys_{0};

    uint64_t last_compact_read_{0};
    uint64_t last_compact_write_{0};
    uint64_t last_write_stall_{0};
    uint64_t last_cache_hit_{0};
    uint64_t last_cache_miss_{0};

    std::mutex ts_mutex_;
    std::vector<TimeSeriesPoint> timeseries_records_;
};

} // namespace study::supp
