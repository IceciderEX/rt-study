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

namespace study::p8 {

struct P8SnapshotPoint {
    int index;
    std::string phase_label; // "PhaseA", "PhaseB", "PhaseC", "PhaseD"
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
    double compaction_read_mb_delta;
    double compaction_write_mb_delta;
    double flush_write_mb_delta;
    uint64_t write_stall_micros_delta;
    uint64_t block_cache_hits_delta;
    uint64_t block_cache_misses_delta;
    double block_cache_hit_rate;
    uint64_t tombstones_dropped_delta;

    // System stats
    double db_dir_mb;
};

class P8LatencyTracker {
public:
    P8LatencyTracker() : count_(0), sum_ns_(0), max_ns_(0) {
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

struct PhaseMetrics {
    uint64_t put_cnt{0};
    double put_p50{0}, put_p95{0}, put_p99{0}, put_mean{0};
    uint64_t get_aff_cnt{0};
    double get_aff_p50{0}, get_aff_p95{0}, get_aff_p99{0}, get_aff_mean{0};
    uint64_t get_del_cnt{0};
    double get_del_p50{0}, get_del_p95{0}, get_del_p99{0}, get_del_mean{0};
    uint64_t get_ctrl_cnt{0};
    double get_ctrl_p50{0}, get_ctrl_p95{0}, get_ctrl_p99{0}, get_ctrl_mean{0};
    uint64_t scan_cnt{0};
    double scan_p50{0}, scan_p95{0}, scan_p99{0}, scan_mean{0}, scan_us_per_key{0}, scan_avg_keys{0};
    uint64_t del_range_cnt{0};
    double del_range_p50{0}, del_range_p95{0}, del_range_p99{0}, del_range_mean{0};
    uint64_t total_ops{0};
    double duration_sec{0};
    double iops{0};
};

class P8PhaseTracker {
public:
    P8PhaseTracker() : total_scan_keys_(0), total_scan_span_(0) {}

    void RecordPut(uint64_t lat_ns) { put_lat_.Record(lat_ns); }
    void RecordGet(int cat, uint64_t lat_ns) {
        if (cat == 0) get_aff_lat_.Record(lat_ns);
        else if (cat == 1) get_del_lat_.Record(lat_ns);
        else get_ctrl_lat_.Record(lat_ns);
    }
    void RecordScan(uint64_t lat_ns, uint64_t span, size_t returned_keys) {
        scan_lat_.Record(lat_ns);
        total_scan_keys_ += returned_keys;
        total_scan_span_ += span;
    }
    void RecordDeleteRange(uint64_t lat_ns) { del_range_lat_.Record(lat_ns); }

    PhaseMetrics ComputeMetrics(double duration_sec) {
        PhaseMetrics m;
        m.duration_sec = duration_sec;
        double dummy_max;

        put_lat_.GetQuantiles(m.put_p50, m.put_p95, m.put_p99, m.put_mean, dummy_max, m.put_cnt);
        get_aff_lat_.GetQuantiles(m.get_aff_p50, m.get_aff_p95, m.get_aff_p99, m.get_aff_mean, dummy_max, m.get_aff_cnt);
        get_del_lat_.GetQuantiles(m.get_del_p50, m.get_del_p95, m.get_del_p99, m.get_del_mean, dummy_max, m.get_del_cnt);
        get_ctrl_lat_.GetQuantiles(m.get_ctrl_p50, m.get_ctrl_p95, m.get_ctrl_p99, m.get_ctrl_mean, dummy_max, m.get_ctrl_cnt);
        scan_lat_.GetQuantiles(m.scan_p50, m.scan_p95, m.scan_p99, m.scan_mean, dummy_max, m.scan_cnt);
        del_range_lat_.GetQuantiles(m.del_range_p50, m.del_range_p95, m.del_range_p99, m.del_range_mean, dummy_max, m.del_range_cnt);

        m.total_ops = m.put_cnt + m.get_aff_cnt + m.get_del_cnt + m.get_ctrl_cnt + m.scan_cnt + m.del_range_cnt;
        m.iops = (duration_sec > 0) ? (m.total_ops / duration_sec) : 0.0;
        m.scan_avg_keys = (m.scan_cnt > 0) ? (static_cast<double>(total_scan_keys_.load()) / m.scan_cnt) : 0.0;
        m.scan_us_per_key = (m.scan_avg_keys > 0) ? (m.scan_mean / m.scan_avg_keys) : 0.0;
        return m;
    }

private:
    P8LatencyTracker put_lat_, get_aff_lat_, get_del_lat_, get_ctrl_lat_, scan_lat_, del_range_lat_;
    std::atomic<uint64_t> total_scan_keys_;
    std::atomic<uint64_t> total_scan_span_;
};

class P8StatsCollector {
public:
    P8StatsCollector()
        : total_put_ops_(0), total_get_aff_ops_(0), total_get_del_ops_(0), total_get_ctrl_ops_(0),
          total_scan_ops_(0), total_scan_returned_keys_(0), total_scan_span_requested_(0),
          total_del_range_ops_(0),
          phase_b_max_p99_scan_(0), phase_b_max_p99_get_del_(0),
          flush_start_sec_(0), flush_end_sec_(0), flush_duration_ms_(0) {}

    void RecordPut(int phase, uint64_t lat_ns) {
        total_put_ops_++;
        main_put_lat_.Record(lat_ns);
        sec_put_lat_.Record(lat_ns);
        prog_put_lat_.Record(lat_ns);

        if (phase == 0) phase_a_.RecordPut(lat_ns);
        else if (phase == 1) phase_b_.RecordPut(lat_ns);
        else if (phase == 2) phase_c_.RecordPut(lat_ns);
    }

    void RecordGet(int phase, int category, uint64_t lat_ns) {
        if (category == 0) {
            total_get_aff_ops_++;
            main_get_aff_lat_.Record(lat_ns);
        } else if (category == 1) {
            total_get_del_ops_++;
            main_get_del_lat_.Record(lat_ns);
        } else {
            total_get_ctrl_ops_++;
            main_get_ctrl_lat_.Record(lat_ns);
        }
        sec_get_ctrl_lat_.Record(lat_ns); // mapped
        if (category == 0) sec_get_aff_lat_.Record(lat_ns);
        else if (category == 1) sec_get_del_lat_.Record(lat_ns);
        else sec_get_ctrl_lat_.Record(lat_ns);

        if (category == 0) prog_get_aff_lat_.Record(lat_ns);
        else if (category == 1) prog_get_del_lat_.Record(lat_ns);
        else prog_get_ctrl_lat_.Record(lat_ns);

        if (phase == 0) phase_a_.RecordGet(category, lat_ns);
        else if (phase == 1) phase_b_.RecordGet(category, lat_ns);
        else if (phase == 2) phase_c_.RecordGet(category, lat_ns);
    }

    void RecordScan(int phase, uint64_t lat_ns, uint64_t span_req, size_t returned_keys) {
        total_scan_ops_++;
        total_scan_span_requested_ += span_req;
        total_scan_returned_keys_ += returned_keys;
        main_scan_lat_.Record(lat_ns);
        sec_scan_lat_.Record(lat_ns);
        sec_scan_keys_ += returned_keys;
        prog_scan_lat_.Record(lat_ns);
        prog_scan_keys_ += returned_keys;

        if (phase == 0) phase_a_.RecordScan(lat_ns, span_req, returned_keys);
        else if (phase == 1) phase_b_.RecordScan(lat_ns, span_req, returned_keys);
        else if (phase == 2) phase_c_.RecordScan(lat_ns, span_req, returned_keys);
    }

    void RecordDeleteRange(int phase, uint64_t lat_ns) {
        total_del_range_ops_++;
        main_del_range_lat_.Record(lat_ns);
        sec_del_range_lat_.Record(lat_ns);
        prog_del_range_lat_.Record(lat_ns);

        if (phase == 0) phase_a_.RecordDeleteRange(lat_ns);
        else if (phase == 1) phase_b_.RecordDeleteRange(lat_ns);
        else if (phase == 2) phase_c_.RecordDeleteRange(lat_ns);
    }

    void SetFlushTiming(double start_sec, double end_sec) {
        flush_start_sec_ = start_sec;
        flush_end_sec_ = end_sec;
        flush_duration_ms_ = (end_sec - start_sec) * 1000.0;
    }

    void CaptureWallClockSecond(int sec_idx, double elapsed_sec, uint64_t completed_ops, double prog_pct,
                                rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                const std::string& db_path) {
        std::string phase_lbl = (prog_pct <= 45.0) ? "PhaseA" : ((prog_pct <= 55.0) ? "PhaseB" : "PhaseC");
        P8SnapshotPoint pt = CollectSnapshot(sec_idx, phase_lbl, elapsed_sec, completed_ops, prog_pct,
                                             sec_get_ctrl_lat_, sec_get_aff_lat_, sec_get_del_lat_,
                                             sec_scan_lat_, sec_scan_keys_, sec_put_lat_, sec_del_range_lat_,
                                             wall_last_comp_read_, wall_last_comp_write_, wall_last_flush_write_,
                                             wall_last_write_stall_, wall_last_cache_hit_, wall_last_cache_miss_,
                                             wall_last_tomb_drop_, db, stats, db_path);
        if (phase_lbl == "PhaseB") {
            if (pt.scan_p99_us > phase_b_max_p99_scan_) phase_b_max_p99_scan_ = pt.scan_p99_us;
            if (pt.get_del_p99_us > phase_b_max_p99_get_del_) phase_b_max_p99_get_del_ = pt.get_del_p99_us;
        }

        std::lock_guard<std::mutex> lk(wall_mutex_);
        wallclock_records_.push_back(pt);
    }

    void CaptureProgressSnapshot(int prog_pct, double elapsed_sec, uint64_t completed_ops,
                                 rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                 const std::string& db_path) {
        std::string phase_lbl = (prog_pct <= 45) ? "PhaseA" : ((prog_pct <= 55) ? "PhaseB" : "PhaseC");
        P8SnapshotPoint pt = CollectSnapshot(prog_pct, phase_lbl, elapsed_sec, completed_ops, static_cast<double>(prog_pct),
                                             prog_get_ctrl_lat_, prog_get_aff_lat_, prog_get_del_lat_,
                                             prog_scan_lat_, prog_scan_keys_, prog_put_lat_, prog_del_range_lat_,
                                             prog_last_comp_read_, prog_last_comp_write_, prog_last_flush_write_,
                                             prog_last_write_stall_, prog_last_cache_hit_, prog_last_cache_miss_,
                                             prog_last_tomb_drop_, db, stats, db_path);
        if (phase_lbl == "PhaseB") {
            if (pt.scan_p99_us > phase_b_max_p99_scan_) phase_b_max_p99_scan_ = pt.scan_p99_us;
            if (pt.get_del_p99_us > phase_b_max_p99_get_del_) phase_b_max_p99_get_del_ = pt.get_del_p99_us;
        }

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
                          const std::string& desc, double elapsed_sec,
                          double phase_a_dur, double phase_b_dur, double phase_c_dur,
                          rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                          uint64_t union_deleted_keys, double union_cov_pct, uint64_t tombstones_cnt,
                          const std::string& sha256_hex, size_t ver_ok, size_t ver_nf, bool pass) {
        if (csv_file.empty()) return;
        std::ofstream file(csv_file, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name,desc,elapsed_sec,total_ops,overall_iops,"
                 << "phase_a_dur_sec,phase_a_iops,phase_a_scan_us_per_key,phase_a_scan_p99_us,phase_a_get_del_p95_us,"
                 << "phase_b_dur_sec,phase_b_iops,phase_b_scan_us_per_key,phase_b_scan_p99_us,phase_b_get_del_p95_us,phase_b_max_p99_scan_us,"
                 << "phase_c_dur_sec,phase_c_iops,phase_c_scan_us_per_key,phase_c_scan_p99_us,phase_c_get_del_p95_us,phase_c_get_ctrl_p99_us,"
                 << "flush_start_sec,flush_end_sec,flush_duration_ms,"
                 << "put_count,put_p50_us,put_p95_us,put_p99_us,"
                 << "scan_count,scan_ops_sec,scan_keys_sec,scan_avg_keys_returned,scan_p50_us,scan_p95_us,scan_p99_us,scan_us_per_key,"
                 << "del_range_count,del_range_p50_us,del_range_p95_us,del_range_p99_us,"
                 << "tombstones_count,union_deleted_keys,union_coverage_ratio_pct,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,"
                 << "compaction_read_mb,compaction_write_mb,flush_write_mb,stall_micros,compaction_drop_keys,"
                 << "l0_files_final,l1_files_final,l2_files_final,pending_compact_mb,total_sst_mb,sha256_checksum,"
                 << "sample_verified_ok,sample_verified_notfound,full_scan_ok\n";
        }

        PhaseMetrics ma = phase_a_.ComputeMetrics(phase_a_dur);
        PhaseMetrics mb = phase_b_.ComputeMetrics(phase_b_dur);
        PhaseMetrics mc = phase_c_.ComputeMetrics(phase_c_dur);

        uint64_t total_ops = total_put_ops_ + total_get_aff_ops_ + total_get_del_ops_ + total_get_ctrl_ops_ + total_scan_ops_ + total_del_range_ops_;
        double overall_iops = (elapsed_sec > 0) ? (total_ops / elapsed_sec) : 0.0;

        double put_p50, put_p95, put_p99, put_mean, put_max; uint64_t put_cnt;
        main_put_lat_.GetQuantiles(put_p50, put_p95, put_p99, put_mean, put_max, put_cnt);

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

        uint64_t comp_read = stats ? (stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) / (1024*1024)) : 0;
        uint64_t comp_write = stats ? (stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) / (1024*1024)) : 0;
        uint64_t flush_write = stats ? (stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) / (1024*1024)) : 0;
        uint64_t stall_us = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t drop_keys = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t l0_files = 0, l1_files = 0, l2_files = 0, pending_bytes = 0, total_sst = 0;
        if (db) {
            db->GetIntProperty("rocksdb.num-files-at-level0", &l0_files);
            db->GetIntProperty("rocksdb.num-files-at-level1", &l1_files);
            db->GetIntProperty("rocksdb.num-files-at-level2", &l2_files);
            db->GetIntProperty("rocksdb.estimate-pending-compaction-bytes", &pending_bytes);
            db->GetIntProperty("rocksdb.total-sst-files-size", &total_sst);
        }

        file << exp_id << "," << group_name << ",\"" << desc << "\"," << elapsed_sec << "," << total_ops << "," << overall_iops << ","
             << phase_a_dur << "," << ma.iops << "," << ma.scan_us_per_key << "," << ma.scan_p99 << "," << ma.get_del_p95 << ","
             << phase_b_dur << "," << mb.iops << "," << mb.scan_us_per_key << "," << mb.scan_p99 << "," << mb.get_del_p95 << "," << phase_b_max_p99_scan_ << ","
             << phase_c_dur << "," << mc.iops << "," << mc.scan_us_per_key << "," << mc.scan_p99 << "," << mc.get_del_p95 << "," << mc.get_ctrl_p99 << ","
             << flush_start_sec_ << "," << flush_end_sec_ << "," << flush_duration_ms_ << ","
             << put_cnt << "," << put_p50 << "," << put_p95 << "," << put_p99 << ","
             << scan_cnt << "," << scan_ops_sec << "," << scan_keys_sec << "," << scan_avg_returned << "," << scan_p50 << "," << scan_p95 << "," << scan_p99 << "," << scan_us_per_key << ","
             << del_cnt << "," << del_p50 << "," << del_p95 << "," << del_p99 << ","
             << tombstones_cnt << "," << union_deleted_keys << "," << union_cov_pct << ","
             << cache_hit << "," << cache_miss << "," << hit_rate << ","
             << comp_read << "," << comp_write << "," << flush_write << "," << stall_us << "," << drop_keys << ","
             << l0_files << "," << l1_files << "," << l2_files << ","
             << (pending_bytes / (1024*1024)) << "," << (total_sst / (1024*1024)) << ","
             << sha256_hex << ","
             << ver_ok << "," << ver_nf << "," << (pass ? "PASS" : "FAIL") << "\n";
    }

private:
    P8SnapshotPoint CollectSnapshot(int index, const std::string& phase_lbl, double elapsed_sec, uint64_t completed_ops, double prog_pct,
                                   P8LatencyTracker& ctrl_lat, P8LatencyTracker& aff_lat, P8LatencyTracker& del_lat,
                                   P8LatencyTracker& scan_lat, std::atomic<uint64_t>& scan_keys,
                                   P8LatencyTracker& put_lat, P8LatencyTracker& del_range_lat,
                                   uint64_t& last_comp_read, uint64_t& last_comp_write, uint64_t& last_flush_write,
                                   uint64_t& last_write_stall, uint64_t& last_cache_hit, uint64_t& last_cache_miss,
                                   uint64_t& last_tomb_drop,
                                   rocksdb::DB* db, std::shared_ptr<rocksdb::Statistics> stats,
                                   const std::string& db_path) {
        uint64_t cur_comp_read = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_READ_BYTES) : 0;
        uint64_t cur_comp_write = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACT_WRITE_BYTES) : 0;
        uint64_t cur_flush_write = stats ? stats->getTickerCount(rocksdb::Tickers::FLUSH_WRITE_BYTES) : 0;
        uint64_t cur_write_stall = stats ? stats->getTickerCount(rocksdb::Tickers::STALL_MICROS) : 0;
        uint64_t cur_cache_hit = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_HIT) : 0;
        uint64_t cur_cache_miss = stats ? stats->getTickerCount(rocksdb::Tickers::BLOCK_CACHE_MISS) : 0;
        uint64_t cur_tomb_drop = stats ? stats->getTickerCount(rocksdb::Tickers::COMPACTION_KEY_DROP_RANGE_DEL) : 0;

        uint64_t comp_read_delta = cur_comp_read - last_comp_read;
        uint64_t comp_write_delta = cur_comp_write - last_comp_write;
        uint64_t flush_write_delta = cur_flush_write - last_flush_write;
        uint64_t write_stall_delta = cur_write_stall - last_write_stall;
        uint64_t cache_hit_delta = cur_cache_hit - last_cache_hit;
        uint64_t cache_miss_delta = cur_cache_miss - last_cache_miss;
        uint64_t tomb_drop_delta = cur_tomb_drop - last_tomb_drop;

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

        P8SnapshotPoint pt;
        pt.index = index;
        pt.phase_label = phase_lbl;
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

        pt.compaction_read_mb_delta = comp_read_delta / (1024.0 * 1024.0);
        pt.compaction_write_mb_delta = comp_write_delta / (1024.0 * 1024.0);
        pt.flush_write_mb_delta = flush_write_delta / (1024.0 * 1024.0);
        pt.write_stall_micros_delta = write_stall_delta;
        pt.block_cache_hits_delta = cache_hit_delta;
        pt.block_cache_misses_delta = cache_miss_delta;
        pt.block_cache_hit_rate = hit_rate;
        pt.tombstones_dropped_delta = tomb_drop_delta;
        pt.db_dir_mb = db_dir_mb;

        return pt;
    }

    void DumpSnapshotVector(const std::string& filepath, const std::string& exp_id, const std::string& group_name,
                            const std::vector<P8SnapshotPoint>& records, const std::string& index_col_name) {
        if (filepath.empty()) return;
        std::ofstream file(filepath, std::ios::app);
        if (!file.is_open()) return;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "exp_id,group_name," << index_col_name << ",phase_label,elapsed_sec,completed_ops,progress_pct,"
                 << "get_ctrl_ops,get_ctrl_p50_us,get_ctrl_p95_us,get_ctrl_p99_us,"
                 << "get_aff_ops,get_aff_p50_us,get_aff_p95_us,get_aff_p99_us,"
                 << "get_del_ops,get_del_p50_us,get_del_p95_us,get_del_p99_us,"
                 << "scan_ops,scan_p50_us,scan_p95_us,scan_p99_us,scan_avg_keys,scan_us_per_key,"
                 << "put_ops,put_p50_us,put_p95_us,put_p99_us,"
                 << "del_range_ops,del_range_p50_us,del_range_p95_us,del_range_p99_us,total_ops_interval,instantaneous_iops,"
                 << "active_memtable_mb,immutable_memtable_count,memtable_flush_pending,l0_files,l1_files,l2_files,pending_compaction_mb,"
                 << "running_flushes,running_compactions,total_sst_mb,"
                 << "compaction_read_mb_delta,compaction_write_mb_delta,flush_write_mb_delta,write_stall_micros_delta,"
                 << "block_cache_hits,block_cache_misses,block_cache_hit_rate,tombstones_dropped_delta,db_dir_mb\n";
        }

        for (const auto& pt : records) {
            file << exp_id << "," << group_name << "," << pt.index << "," << pt.phase_label << "," << pt.elapsed_sec << "," << pt.completed_ops << "," << pt.progress_pct << ","
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
                 << pt.compaction_read_mb_delta << "," << pt.compaction_write_mb_delta << "," << pt.flush_write_mb_delta << ","
                 << pt.write_stall_micros_delta << ","
                 << pt.block_cache_hits_delta << "," << pt.block_cache_misses_delta << "," << pt.block_cache_hit_rate << ","
                 << pt.tombstones_dropped_delta << "," << pt.db_dir_mb << "\n";
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

    P8LatencyTracker main_put_lat_;
    P8LatencyTracker main_get_aff_lat_;
    P8LatencyTracker main_get_del_lat_;
    P8LatencyTracker main_get_ctrl_lat_;
    P8LatencyTracker main_scan_lat_;
    P8LatencyTracker main_del_range_lat_;

    // Phase Trackers
    P8PhaseTracker phase_a_;
    P8PhaseTracker phase_b_;
    P8PhaseTracker phase_c_;

    double phase_b_max_p99_scan_;
    double phase_b_max_p99_get_del_;

    double flush_start_sec_;
    double flush_end_sec_;
    double flush_duration_ms_;

    // Wall-clock interval trackers
    P8LatencyTracker sec_put_lat_, sec_get_aff_lat_, sec_get_del_lat_, sec_get_ctrl_lat_, sec_scan_lat_, sec_del_range_lat_;
    std::atomic<uint64_t> sec_scan_keys_{0};
    uint64_t wall_last_comp_read_{0}, wall_last_comp_write_{0}, wall_last_flush_write_{0};
    uint64_t wall_last_write_stall_{0}, wall_last_cache_hit_{0}, wall_last_cache_miss_{0}, wall_last_tomb_drop_{0};
    std::mutex wall_mutex_;
    std::vector<P8SnapshotPoint> wallclock_records_;

    // Progress interval trackers
    P8LatencyTracker prog_put_lat_, prog_get_aff_lat_, prog_get_del_lat_, prog_get_ctrl_lat_, prog_scan_lat_, prog_del_range_lat_;
    std::atomic<uint64_t> prog_scan_keys_{0};
    uint64_t prog_last_comp_read_{0}, prog_last_comp_write_{0}, prog_last_flush_write_{0};
    uint64_t prog_last_write_stall_{0}, prog_last_cache_hit_{0}, prog_last_cache_miss_{0}, prog_last_tomb_drop_{0};
    std::mutex prog_mutex_;
    std::vector<P8SnapshotPoint> progress_records_;
};

} // namespace study::p8
