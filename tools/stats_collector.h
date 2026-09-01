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
#include <cstdint>
#include "rocksdb/db.h"
#include "rocksdb/statistics.h"

namespace study {

class LatencyTracker {
public:
    LatencyTracker() = default;

    void Record(uint64_t latency_ns) {
        std::lock_guard<std::mutex> lock(mutex_);
        latencies_ns_.push_back(latency_ns);
    }

    void Merge(const std::vector<uint64_t>& other) {
        std::lock_guard<std::mutex> lock(mutex_);
        latencies_ns_.insert(latencies_ns_.end(), other.begin(), other.end());
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latencies_ns_.size();
    }

    void GetPercentilesUs(double& p50, double& p90, double& p95, double& p99, double& p999, double& max_val, double& mean_val) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latencies_ns_.empty()) {
            p50 = p90 = p95 = p99 = p999 = max_val = mean_val = 0.0;
            return;
        }

        std::vector<uint64_t> sorted = latencies_ns_;
        std::sort(sorted.begin(), sorted.end());

        size_t n = sorted.size();
        p50 = sorted[static_cast<size_t>(n * 0.50)] / 1000.0;
        p90 = sorted[static_cast<size_t>(n * 0.90)] / 1000.0;
        p95 = sorted[static_cast<size_t>(n * 0.95)] / 1000.0;
        p99 = sorted[static_cast<size_t>(n * 0.99)] / 1000.0;
        size_t idx999 = std::min(static_cast<size_t>(n * 0.999), n - 1);
        p999 = sorted[idx999] / 1000.0;
        max_val = sorted.back() / 1000.0;

        double sum = 0.0;
        for (uint64_t l : sorted) sum += l;
        mean_val = (sum / n) / 1000.0;
    }

private:
    std::vector<uint64_t> latencies_ns_;
    mutable std::mutex mutex_;
};

struct RocksDBStatsSnapshot {
    uint64_t compact_read_bytes = 0;
    uint64_t compact_write_bytes = 0;
    uint64_t flush_write_bytes = 0;
    uint64_t stall_micros = 0;
    uint64_t write_stall_count = 0;
    uint64_t block_cache_data_hit = 0;
    uint64_t block_cache_data_miss = 0;
    uint64_t block_cache_index_hit = 0;
    uint64_t block_cache_index_miss = 0;
    uint64_t block_cache_filter_hit = 0;
    uint64_t block_cache_filter_miss = 0;
    uint64_t compaction_key_drop_range_del = 0;

    // DB Properties
    uint64_t num_files_at_level0 = 0;
    uint64_t pending_compaction_bytes = 0;
    uint64_t num_running_flushes = 0;
    uint64_t num_running_compactions = 0;
    uint64_t actual_delayed_write_rate = 0;
    uint64_t total_sst_size = 0;
    uint64_t live_sst_size = 0;
    uint64_t estimate_live_data_size = 0;
    uint64_t block_cache_usage = 0;
    uint64_t block_cache_capacity = 0;
    std::string write_stall_stats = "";
    std::string aggregated_table_properties = "";
};

class StatsCollector {
public:
    StatsCollector() = default;

    void RecordPut(uint64_t latency_ns, bool ok) {
        put_latencies_.Record(latency_ns);
        if (ok) put_ok_count_++;
        else put_err_count_++;
    }

    void RecordGetAffectedLive(uint64_t latency_ns, bool ok) {
        get_affected_live_latencies_.Record(latency_ns);
        if (ok) get_affected_live_ok_count_++;
        else get_affected_live_err_count_++;
    }

    void RecordGetDeleted(uint64_t latency_ns, bool is_not_found) {
        get_deleted_latencies_.Record(latency_ns);
        if (is_not_found) get_deleted_notfound_count_++;
        else get_deleted_err_count_++;
    }

    void RecordGetControl(uint64_t latency_ns, bool ok) {
        get_control_latencies_.Record(latency_ns);
        if (ok) get_control_ok_count_++;
        else get_control_err_count_++;
    }

    void RecordRangeScan(uint64_t latency_ns, uint64_t scan_span, uint64_t keys_returned, bool ok) {
        scan_latencies_.Record(latency_ns);
        if (ok) {
            scan_ok_count_++;
            total_scan_span_ += scan_span;
            total_scan_keys_returned_ += keys_returned;
        } else {
            scan_err_count_++;
        }
    }

    void RecordDeleteRange(uint64_t latency_ns, uint64_t del_span, bool ok) {
        del_range_latencies_.Record(latency_ns);
        if (ok) {
            del_range_ok_count_++;
            total_del_span_ += del_span;
        } else {
            del_range_err_count_++;
        }
    }

    static RocksDBStatsSnapshot CaptureRocksDBSnapshot(rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats) {
        RocksDBStatsSnapshot snap;
        if (stats) {
            snap.compact_read_bytes = stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES);
            snap.compact_write_bytes = stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES);
            snap.flush_write_bytes = stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES);
            snap.stall_micros = stats->getTickerCount(rocksdb::Tickers::STALL_MICROS);
            rocksdb::HistogramData h_data;
            stats->histogramData(rocksdb::Histograms::WRITE_STALL, &h_data);
            snap.write_stall_count = h_data.count;
            snap.block_cache_data_hit = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_DATA_HIT);
            snap.block_cache_data_miss = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_DATA_MISS);
            snap.block_cache_index_hit = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_INDEX_HIT);
            snap.block_cache_index_miss = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_INDEX_MISS);
            snap.block_cache_filter_hit = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_FILTER_HIT);
            snap.block_cache_filter_miss = stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_FILTER_MISS);
            snap.compaction_key_drop_range_del = stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL);
        }

        if (db) {
            uint64_t val = 0;
            if (db->GetIntProperty(rocksdb::DB::Properties::kNumFilesAtLevelPrefix + "0", &val)) snap.num_files_at_level0 = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes, &val)) snap.pending_compaction_bytes = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kNumRunningFlushes, &val)) snap.num_running_flushes = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kNumRunningCompactions, &val)) snap.num_running_compactions = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kActualDelayedWriteRate, &val)) snap.actual_delayed_write_rate = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kTotalSstFilesSize, &val)) snap.total_sst_size = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kLiveSstFilesSize, &val)) snap.live_sst_size = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kEstimateLiveDataSize, &val)) snap.estimate_live_data_size = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kBlockCacheUsage, &val)) snap.block_cache_usage = val;
            if (db->GetIntProperty(rocksdb::DB::Properties::kBlockCacheCapacity, &val)) snap.block_cache_capacity = val;

            std::string str_val;
            if (db->GetProperty(rocksdb::DB::Properties::kDBWriteStallStats, &str_val)) snap.write_stall_stats = str_val;
            if (db->GetProperty(rocksdb::DB::Properties::kAggregatedTableProperties, &str_val)) snap.aggregated_table_properties = str_val;
        }

        return snap;
    }

    void PrintSummary(double elapsed_sec, const RocksDBStatsSnapshot& start_snap, const RocksDBStatsSnapshot& end_snap) {
        std::cout << "\n================ BENCHMARK EXECUTION SUMMARY ================\n";
        std::cout << "Elapsed Time: " << std::fixed << std::setprecision(3) << elapsed_sec << " s\n";

        size_t total_ops = put_ok_count_ + put_err_count_ +
                           get_affected_live_ok_count_ + get_affected_live_err_count_ +
                           get_deleted_notfound_count_ + get_deleted_err_count_ +
                           get_control_ok_count_ + get_control_err_count_ +
                           scan_ok_count_ + scan_err_count_ +
                           del_range_ok_count_ + del_range_err_count_;

        double overall_iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;
        std::cout << "Total Operations: " << total_ops << " (Overall IOPS: " << std::fixed << std::setprecision(1) << overall_iops << " ops/s)\n\n";

        PrintLatencyRow("Put", put_latencies_, put_ok_count_, put_err_count_, elapsed_sec);
        PrintLatencyRow("Get (Affected-Live)", get_affected_live_latencies_, get_affected_live_ok_count_, get_affected_live_err_count_, elapsed_sec);
        PrintLatencyRow("Get (Deleted-ExpectedNF)", get_deleted_latencies_, get_deleted_notfound_count_, get_deleted_err_count_, elapsed_sec);
        PrintLatencyRow("Get (Control-FarLive)", get_control_latencies_, get_control_ok_count_, get_control_err_count_, elapsed_sec);

        // Scan specific
        double sp50, sp90, sp95, sp99, sp999, smax, smean;
        scan_latencies_.GetPercentilesUs(sp50, sp90, sp95, sp99, sp999, smax, smean);
        double scan_ops_sec = (elapsed_sec > 0) ? (scan_ok_count_ / elapsed_sec) : 0.0;
        double scan_keys_sec = (elapsed_sec > 0) ? (total_scan_keys_returned_ / elapsed_sec) : 0.0;
        double avg_keys_per_scan = (scan_ok_count_ > 0) ? (static_cast<double>(total_scan_keys_returned_) / scan_ok_count_) : 0.0;
        double avg_span_per_scan = (scan_ok_count_ > 0) ? (static_cast<double>(total_scan_span_) / scan_ok_count_) : 0.0;
        double avg_us_per_key = (total_scan_keys_returned_ > 0) ? ((smean * scan_ok_count_) / total_scan_keys_returned_) : 0.0;

        std::cout << "--- RangeScan Detailed Metrics ---\n"
                  << "  Count: " << scan_ok_count_ << " (Errors: " << scan_err_count_ << ")\n"
                  << "  Throughput: " << std::fixed << std::setprecision(1) << scan_ops_sec << " scans/s, "
                  << scan_keys_sec << " keys/s\n"
                  << "  Average Key Space Span Requested: " << std::setprecision(1) << avg_span_per_scan << " keys\n"
                  << "  Average Actual Keys Returned: " << std::setprecision(1) << avg_keys_per_scan << " keys/scan\n"
                  << "  Latency: P50=" << std::setprecision(2) << sp50 << "us, P95=" << sp95 << "us, P99=" << sp99
                  << "us, Mean=" << smean << "us, Max=" << smax << "us\n"
                  << "  Normalized Latency Per Key Returned: " << std::setprecision(3) << avg_us_per_key << " us/key\n\n";

        PrintLatencyRow("DeleteRange", del_range_latencies_, del_range_ok_count_, del_range_err_count_, elapsed_sec);

        std::cout << "--- RocksDB Engine Delta Metrics ---\n";
        std::cout << "  Compaction Read: " << (end_snap.compact_read_bytes - start_snap.compact_read_bytes) / 1024 / 1024 << " MB\n";
        std::cout << "  Compaction Write: " << (end_snap.compact_write_bytes - start_snap.compact_write_bytes) / 1024 / 1024 << " MB\n";
        std::cout << "  Flush Write: " << (end_snap.flush_write_bytes - start_snap.flush_write_bytes) / 1024 / 1024 << " MB\n";
        std::cout << "  Write Stall Micros: " << (end_snap.stall_micros - start_snap.stall_micros) << " us\n";
        std::cout << "  Write Stall Trigger Count: " << (end_snap.write_stall_count - start_snap.write_stall_count) << "\n";
        std::cout << "  Keys Dropped by Range Deletions: " << (end_snap.compaction_key_drop_range_del - start_snap.compaction_key_drop_range_del) << "\n";
        std::cout << "  Level 0 Files (Final): " << end_snap.num_files_at_level0 << "\n";
        std::cout << "  Pending Compaction Bytes (Final): " << end_snap.pending_compaction_bytes / 1024 / 1024 << " MB\n";
        std::cout << "  Total SST Size (Final): " << end_snap.total_sst_size / 1024 / 1024 << " MB\n";
        std::cout << "=============================================================\n";
    }

    void WriteCSV(const std::string& filename, const std::string& exp_id, double elapsed_sec,
                  const RocksDBStatsSnapshot& start_snap, const RocksDBStatsSnapshot& end_snap,
                  uint64_t total_tombstones, uint64_t union_deleted_keys, double union_coverage_ratio, double overlap_factor,
                  uint64_t verified_ok, uint64_t verified_notfound, bool full_scan_ok) {
        std::ofstream fout(filename, std::ios::app);
        if (!fout.is_open()) {
            std::cerr << "Error writing CSV summary to: " << filename << std::endl;
            return;
        }

        fout.seekp(0, std::ios::end);
        if (fout.tellp() == 0) {
            fout << "exp_id,elapsed_sec,total_ops,overall_iops,"
                 << "put_count,put_p50_us,put_p95_us,put_p99_us,put_mean_us,"
                 << "get_aff_live_count,get_aff_live_p50_us,get_aff_live_p95_us,get_aff_live_p99_us,get_aff_live_mean_us,"
                 << "get_del_count,get_del_p50_us,get_del_p95_us,get_del_p99_us,get_del_mean_us,"
                 << "get_ctrl_count,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,get_ctrl_mean_us,"
                 << "scan_count,scan_ops_sec,scan_keys_sec,scan_avg_span,scan_avg_keys_returned,scan_p50_us,scan_p95_us,scan_p99_us,scan_mean_us,scan_us_per_key,"
                 << "del_range_count,del_range_p50_us,del_range_p95_us,del_range_p99_us,del_range_mean_us,"
                 << "tombstones_count,union_deleted_keys,union_coverage_ratio,overlap_factor,"
                 << "compaction_read_mb,compaction_write_mb,flush_write_mb,stall_micros,compaction_drop_keys,l0_files_final,pending_compact_mb,total_sst_mb,"
                 << "sample_verified_ok,sample_verified_notfound,full_scan_ok\n";
        }

        size_t total_ops = put_ok_count_ + put_err_count_ +
                           get_affected_live_ok_count_ + get_affected_live_err_count_ +
                           get_deleted_notfound_count_ + get_deleted_err_count_ +
                           get_control_ok_count_ + get_control_err_count_ +
                           scan_ok_count_ + scan_err_count_ +
                           del_range_ok_count_ + del_range_err_count_;

        double iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;

        double pp50, pp90, pp95, pp99, pp999, pmax, pmean;
        put_latencies_.GetPercentilesUs(pp50, pp90, pp95, pp99, pp999, pmax, pmean);

        double ap50, ap90, ap95, ap99, ap999, amax, amean;
        get_affected_live_latencies_.GetPercentilesUs(ap50, ap90, ap95, ap99, ap999, amax, amean);

        double dp50, dp90, dp95, dp99, dp999, dmax, dmean;
        get_deleted_latencies_.GetPercentilesUs(dp50, dp90, dp95, dp99, dp999, dmax, dmean);

        double cp50, cp90, cp95, cp99, cp999, cmax, cmean;
        get_control_latencies_.GetPercentilesUs(cp50, cp90, cp95, cp99, cp999, cmax, cmean);

        double sp50, sp90, sp95, sp99, sp999, smax, smean;
        scan_latencies_.GetPercentilesUs(sp50, sp90, sp95, sp99, sp999, smax, smean);

        double rp50, rp90, rp95, rp99, rp999, rmax, rmean;
        del_range_latencies_.GetPercentilesUs(rp50, rp90, rp95, rp99, rp999, rmax, rmean);

        double scan_ops_sec = (elapsed_sec > 0) ? (scan_ok_count_ / elapsed_sec) : 0.0;
        double scan_keys_sec = (elapsed_sec > 0) ? (total_scan_keys_returned_ / elapsed_sec) : 0.0;
        double avg_keys_per_scan = (scan_ok_count_ > 0) ? (static_cast<double>(total_scan_keys_returned_) / scan_ok_count_) : 0.0;
        double avg_span_per_scan = (scan_ok_count_ > 0) ? (static_cast<double>(total_scan_span_) / scan_ok_count_) : 0.0;
        double avg_us_per_key = (total_scan_keys_returned_ > 0) ? ((smean * scan_ok_count_) / total_scan_keys_returned_) : 0.0;

        fout << exp_id << "," << elapsed_sec << "," << total_ops << "," << iops << ","
             << put_ok_count_ << "," << pp50 << "," << pp95 << "," << pp99 << "," << pmean << ","
             << get_affected_live_ok_count_ << "," << ap50 << "," << ap95 << "," << ap99 << "," << amean << ","
             << get_deleted_notfound_count_ << "," << dp50 << "," << dp95 << "," << dp99 << "," << dmean << ","
             << get_control_ok_count_ << "," << cp50 << "," << cp95 << "," << cp99 << "," << cmean << ","
             << scan_ok_count_ << "," << scan_ops_sec << "," << scan_keys_sec << "," << avg_span_per_scan << "," << avg_keys_per_scan << ","
             << sp50 << "," << sp95 << "," << sp99 << "," << smean << "," << avg_us_per_key << ","
             << del_range_ok_count_ << "," << rp50 << "," << rp95 << "," << rp99 << "," << rmean << ","
             << total_tombstones << "," << union_deleted_keys << "," << union_coverage_ratio << "," << overlap_factor << ","
             << (end_snap.compact_read_bytes - start_snap.compact_read_bytes) / 1024.0 / 1024.0 << ","
             << (end_snap.compact_write_bytes - start_snap.compact_write_bytes) / 1024.0 / 1024.0 << ","
             << (end_snap.flush_write_bytes - start_snap.flush_write_bytes) / 1024.0 / 1024.0 << ","
             << (end_snap.stall_micros - start_snap.stall_micros) << ","
             << (end_snap.compaction_key_drop_range_del - start_snap.compaction_key_drop_range_del) << ","
             << end_snap.num_files_at_level0 << ","
             << end_snap.pending_compaction_bytes / 1024.0 / 1024.0 << ","
             << end_snap.total_sst_size / 1024.0 / 1024.0 << ","
             << verified_ok << "," << verified_notfound << "," << (full_scan_ok ? "PASS" : "FAIL") << "\n";
    }

private:
    void PrintLatencyRow(const std::string& name, LatencyTracker& tracker, size_t ok_cnt, size_t err_cnt, double elapsed_sec) {
        double p50, p90, p95, p99, p999, max_val, mean_val;
        tracker.GetPercentilesUs(p50, p90, p95, p99, p999, max_val, mean_val);
        double ops_sec = (elapsed_sec > 0) ? (ok_cnt / elapsed_sec) : 0.0;
        std::cout << "  " << std::left << std::setw(26) << name
                  << " Count=" << std::setw(8) << ok_cnt
                  << " (Err=" << std::setw(3) << err_cnt << ")"
                  << " Rate=" << std::fixed << std::setprecision(1) << std::setw(8) << ops_sec << " ops/s"
                  << " P50=" << std::setprecision(2) << std::setw(7) << p50 << "us"
                  << " P95=" << std::setw(7) << p95 << "us"
                  << " P99=" << std::setw(7) << p99 << "us"
                  << " Mean=" << std::setw(7) << mean_val << "us\n";
    }

    LatencyTracker put_latencies_;
    LatencyTracker get_affected_live_latencies_;
    LatencyTracker get_deleted_latencies_;
    LatencyTracker get_control_latencies_;
    LatencyTracker scan_latencies_;
    LatencyTracker del_range_latencies_;

    std::atomic<size_t> put_ok_count_{0};
    std::atomic<size_t> put_err_count_{0};
    std::atomic<size_t> get_affected_live_ok_count_{0};
    std::atomic<size_t> get_affected_live_err_count_{0};
    std::atomic<size_t> get_deleted_notfound_count_{0};
    std::atomic<size_t> get_deleted_err_count_{0};
    std::atomic<size_t> get_control_ok_count_{0};
    std::atomic<size_t> get_control_err_count_{0};
    std::atomic<size_t> scan_ok_count_{0};
    std::atomic<size_t> scan_err_count_{0};
    std::atomic<size_t> del_range_ok_count_{0};
    std::atomic<size_t> del_range_err_count_{0};

    std::atomic<uint64_t> total_scan_span_{0};
    std::atomic<uint64_t> total_scan_keys_returned_{0};
    std::atomic<uint64_t> total_del_span_{0};
};

} // namespace study
