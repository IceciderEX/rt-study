#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <algorithm>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "formal_config.h"
#include "thread_local_histogram.h"
#include "formal_event_listener.h"

namespace study::formal {

enum class RtpMcMode : uint8_t {
    kDisabled = 0,
    kObserve = 1,
    kShadow = 2,
    kActiveV2A = 3,
    kActiveV2B = 4
};

enum class RtpMcDecision : uint8_t {
    kHold = 0,
    kSeal = 1,
    kPromote = 2,
    kYield = 3
};

struct WorkerHistogramBundle {
    DoubleBufferedHistogram hist_get_live;
    DoubleBufferedHistogram hist_get_del;
    DoubleBufferedHistogram hist_scan;
    DoubleBufferedHistogram hist_scan_intersect;
    DoubleBufferedHistogram hist_put;
};

class RtpMcController {
public:
    RtpMcController(rocksdb::DB* db, const FormalConfig& config, 
                    const std::string& exp_id,
                    std::vector<std::unique_ptr<WorkerHistogramBundle>>& worker_bundles,
                    std::shared_ptr<FormalEventListener> event_listener)
        : db_(db),
          config_(config),
          exp_id_(exp_id),
          worker_bundles_(worker_bundles),
          event_listener_(event_listener),
          running_(false),
          current_phase_(0),
          action_counter_(0),
          consecutive_read_warn_count_(0),
          consecutive_read_critical_count_(0),
          consecutive_yield_count_(0),
          inflight_flush_(false),
          last_tombstone_count_(0),
          first_tombstone_seen_time_us_(0),
          last_eval_time_us_(0),
          last_put_bytes_(0),
          ewma_put_bytes_per_sec_(0.0),
          total_windows_logged_(0),
          total_seals_triggered_(0),
          total_conflicts_logged_(0)
    {
        if (config_.rtp_mc_mode == "observe") mode_ = RtpMcMode::kObserve;
        else if (config_.rtp_mc_mode == "shadow") mode_ = RtpMcMode::kShadow;
        else if (config_.rtp_mc_mode == "active_v2a" || config_.rtp_mc_mode == "active") mode_ = RtpMcMode::kActiveV2A;
        else if (config_.rtp_mc_mode == "active_v2b") mode_ = RtpMcMode::kActiveV2B;
        else mode_ = RtpMcMode::kDisabled;

        InitLogs();
    }

    ~RtpMcController() {
        Stop();
    }

    void Start() {
        if (mode_ == RtpMcMode::kDisabled) return;
        running_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();
        last_eval_time_us_ = GetCurrentTimeUs();
        controller_thread_ = std::thread(&RtpMcController::ControllerLoop, this);
    }

    void Stop() {
        if (!running_.load(std::memory_order_acquire)) return;
        running_.store(false, std::memory_order_release);
        cv_wakeup_.notify_all();
        if (controller_thread_.joinable()) {
            controller_thread_.join();
        }
        FlushLogs();
    }

    void SetCurrentPhase(int phase) {
        current_phase_.store(phase, std::memory_order_release);
    }

    void NotifyDeleteRangeCheckpoint() {
        if (mode_ == RtpMcMode::kDisabled) return;
        cv_wakeup_.notify_one();
    }

    void NotifyFlushCompleted(uint64_t /*output_bytes*/) {
        inflight_flush_.store(false, std::memory_order_release);
    }

    uint64_t GetTotalWindowsLogged() const { return total_windows_logged_; }
    uint64_t GetTotalSealsTriggered() const { return total_seals_triggered_; }
    uint64_t GetTotalConflictsLogged() const { return total_conflicts_logged_; }

private:
    uint64_t GetCurrentTimeUs() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - start_time_).count();
    }

    void InitLogs() {
        if (mode_ == RtpMcMode::kDisabled) return;
        if (!config_.windows_csv.empty()) {
            std::filesystem::create_directories(std::filesystem::path(config_.windows_csv).parent_path());
            bool exists = std::filesystem::exists(config_.windows_csv) && std::filesystem::file_size(config_.windows_csv) > 0;
            windows_file_.open(config_.windows_csv, std::ios::app);
            if (!exists) {
                windows_file_ << "exp_id,timestamp_us,phase,active_range_tombstones,active_memtable_bytes,"
                              << "range_del_rate,estimated_capacity_flush_ms,getlive_samples,getlive_p99_us,"
                              << "getdel_samples,getdel_p99_us,scan_samples,scan_p99_us,put_samples,put_p99_us,"
                              << "l0_files,immutable_memtables,pending_compaction_bytes,read_debt,reclaim_debt,"
                              << "maintenance_debt,decision,decision_reason,conflict_state\n";
            }
        }
        if (!config_.actions_csv.empty()) {
            std::filesystem::create_directories(std::filesystem::path(config_.actions_csv).parent_path());
            bool exists = std::filesystem::exists(config_.actions_csv) && std::filesystem::file_size(config_.actions_csv) > 0;
            actions_file_.open(config_.actions_csv, std::ios::app);
            if (!exists) {
                actions_file_ << "exp_id,action_id,request_time_us,action,reason,state_before,"
                              << "flush_begin_time_us,flush_end_time_us,compaction_related,read_relief,put_cost,"
                              << "flush_output_bytes,compaction_read_bytes,compaction_write_bytes,result\n";
            }
        }
    }

    void FlushLogs() {
        if (windows_file_.is_open()) windows_file_.flush();
        if (actions_file_.is_open()) actions_file_.flush();
    }

    void ControllerLoop() {
        while (running_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(mutex_wakeup_);
                cv_wakeup_.wait_for(lock, std::chrono::milliseconds(config_.control_epoch_ms), [this]() {
                    return !running_.load(std::memory_order_acquire);
                });
            }

            if (!running_.load(std::memory_order_acquire)) break;
            EvaluateAndAct();
        }
    }

    void EvaluateAndAct() {
        uint64_t now_us = GetCurrentTimeUs();
        double dt_sec = std::max((now_us - last_eval_time_us_) / 1000000.0, 0.001);
        last_eval_time_us_ = now_us;

        // 1. Double-buffered snapshot aggregation across all workers
        ThreadLocalHistogram agg_get_live;
        ThreadLocalHistogram agg_get_del;
        ThreadLocalHistogram agg_scan;
        ThreadLocalHistogram agg_put;

        for (auto& bundle : worker_bundles_) {
            uint8_t f_gl = bundle->hist_get_live.SwapAndGetFrozenIndex();
            agg_get_live.MergeFrom(bundle->hist_get_live.GetBuffer(f_gl));

            uint8_t f_gd = bundle->hist_get_del.SwapAndGetFrozenIndex();
            agg_get_del.MergeFrom(bundle->hist_get_del.GetBuffer(f_gd));

            uint8_t f_sc = bundle->hist_scan.SwapAndGetFrozenIndex();
            agg_scan.MergeFrom(bundle->hist_scan.GetBuffer(f_sc));

            uint8_t f_pt = bundle->hist_put.SwapAndGetFrozenIndex();
            agg_put.MergeFrom(bundle->hist_put.GetBuffer(f_pt));
        }

        double getlive_p50, getlive_p90, getlive_p95, getlive_p99, getlive_p999, getlive_mean, getlive_max;
        agg_get_live.ComputeQuantiles(getlive_p50, getlive_p90, getlive_p95, getlive_p99, getlive_p999, getlive_mean, getlive_max);

        double getdel_p50, getdel_p90, getdel_p95, getdel_p99, getdel_p999, getdel_mean, getdel_max;
        agg_get_del.ComputeQuantiles(getdel_p50, getdel_p90, getdel_p95, getdel_p99, getdel_p999, getdel_mean, getdel_max);

        double scan_p50, scan_p90, scan_p95, scan_p99, scan_p999, scan_mean, scan_max;
        agg_scan.ComputeQuantiles(scan_p50, scan_p90, scan_p95, scan_p99, scan_p999, scan_mean, scan_max);

        double put_p50, put_p90, put_p95, put_p99, put_p999, put_mean, put_max;
        agg_put.ComputeQuantiles(put_p50, put_p90, put_p95, put_p99, put_p999, put_mean, put_max);

        uint64_t getlive_samples = agg_get_live.GetCount();
        uint64_t getdel_samples = agg_get_del.GetCount();
        uint64_t scan_samples = agg_scan.GetCount();
        uint64_t put_samples = agg_put.GetCount();

        // 2. Query RocksDB Properties
        uint64_t active_tombstones = 0;
        uint64_t active_memtable_bytes = 0;
        uint64_t l0_files = 0;
        uint64_t imm_memtables = 0;
        uint64_t pending_compaction_bytes = 0;

        if (db_) {
            db_->GetIntProperty(rocksdb::DB::Properties::kNumRangeDeletionsActiveMemTable, &active_tombstones);
            db_->GetIntProperty(rocksdb::DB::Properties::kCurSizeActiveMemTable, &active_memtable_bytes);
            db_->GetIntProperty(rocksdb::DB::Properties::kNumFilesAtLevelPrefix + "0", &l0_files);
            db_->GetIntProperty(rocksdb::DB::Properties::kNumImmutableMemTable, &imm_memtables);
            db_->GetIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes, &pending_compaction_bytes);
        }

        // 3. Compute Rates & Tracking
        if (active_tombstones > 0) {
            if (first_tombstone_seen_time_us_ == 0) {
                first_tombstone_seen_time_us_ = now_us;
            }
        } else {
            first_tombstone_seen_time_us_ = 0;
        }

        uint64_t tombstone_residency_ms = (first_tombstone_seen_time_us_ > 0 && now_us >= first_tombstone_seen_time_us_) ?
            (now_us - first_tombstone_seen_time_us_) / 1000 : 0;

        double range_del_rate = (active_tombstones >= last_tombstone_count_) ?
            (active_tombstones - last_tombstone_count_) / dt_sec : 0.0;
        last_tombstone_count_ = active_tombstones;

        uint64_t current_put_bytes = put_samples * config_.value_size;
        double current_put_bps = current_put_bytes / dt_sec;
        if (ewma_put_bytes_per_sec_ == 0.0) {
            ewma_put_bytes_per_sec_ = current_put_bps;
        } else {
            ewma_put_bytes_per_sec_ = 0.7 * ewma_put_bytes_per_sec_ + 0.3 * current_put_bps;
        }

        double estimated_capacity_flush_ms = 999999.0;
        if (active_memtable_bytes < config_.write_buffer_size) {
            uint64_t remaining_bytes = config_.write_buffer_size - active_memtable_bytes;
            if (ewma_put_bytes_per_sec_ > 1024.0) {
                estimated_capacity_flush_ms = (static_cast<double>(remaining_bytes) / ewma_put_bytes_per_sec_) * 1000.0;
            }
        } else {
            estimated_capacity_flush_ms = 0.0;
        }

        // 4. Compute 3 Debts
        // A. Read Debt (strictly excluding GetDeleted)
        double read_debt = 0.0;
        bool has_sufficient_read_samples = (scan_samples >= config_.min_scan_samples || getlive_samples >= config_.min_get_samples);
        if (has_sufficient_read_samples) {
            double q_scan = static_cast<double>(scan_samples) / std::max<double>(scan_samples + getlive_samples, 1.0);
            double q_get = static_cast<double>(getlive_samples) / std::max<double>(scan_samples + getlive_samples, 1.0);

            double scan_over = (config_.scan_slo_us > 0.0 && scan_samples >= 5) ?
                std::max(0.0, (scan_p99 / config_.scan_slo_us) - 1.0) : 0.0;
            double get_over = (config_.getlive_slo_us > 0.0 && getlive_samples >= 10) ?
                std::max(0.0, (getlive_p99 / config_.getlive_slo_us) - 1.0) : 0.0;

            read_debt = q_scan * scan_over + q_get * get_over;
        }

        // Read State Classification
        enum class ReadState { kOk, kUnknown, kWarn, kCritical };
        ReadState read_state = ReadState::kOk;
        if (!has_sufficient_read_samples) {
            read_state = ReadState::kUnknown;
        } else if (read_debt == 0.0) {
            read_state = ReadState::kOk;
            consecutive_read_warn_count_ = 0;
            consecutive_read_critical_count_ = 0;
        } else {
            consecutive_read_warn_count_++;
            if (read_debt >= config_.read_critical_multiplier || 
                consecutive_read_warn_count_ >= config_.read_critical_consecutive_windows) {
                read_state = ReadState::kCritical;
                consecutive_read_critical_count_++;
            } else {
                read_state = ReadState::kWarn;
            }
        }

        // B. Reclaim Debt (online based on tombstones, residency, rate)
        double residency_ratio = std::min(1.0, static_cast<double>(tombstone_residency_ms) / std::max<double>(config_.max_tombstone_residency_ms, 1.0));
        double reclaim_debt = static_cast<double>(active_tombstones) * (1.0 + residency_ratio) + 0.1 * range_del_rate;

        // C. Maintenance Debt
        double put_risk = (config_.put_slo_us > 0.0 && put_samples >= 10) ?
            std::max(0.0, (put_p99 / config_.put_slo_us) - 1.0) : 0.0;
        double l0_risk = (l0_files >= static_cast<uint64_t>(config_.l0_soft_limit)) ?
            static_cast<double>(l0_files - config_.l0_soft_limit) / std::max(1, config_.l0_hard_limit - config_.l0_soft_limit) : 0.0;
        double pending_risk = (pending_compaction_bytes >= config_.pending_compaction_soft_bytes) ?
            static_cast<double>(pending_compaction_bytes - config_.pending_compaction_soft_bytes) / 
            std::max<double>(1.0, config_.pending_compaction_hard_bytes - config_.pending_compaction_soft_bytes) : 0.0;
        double inflight_risk = inflight_flush_.load(std::memory_order_acquire) ? 2.0 : 0.0;

        double maintenance_debt = put_risk + l0_risk + pending_risk + inflight_risk;

        // 5. Decision State Machine
        RtpMcDecision decision = RtpMcDecision::kHold;
        std::string decision_reason = "NONE";
        std::string conflict_state = "NORMAL";

        // Safety Deadman Bounds
        if (active_tombstones >= config_.max_range_del_deadman) {
            decision = RtpMcDecision::kSeal;
            decision_reason = "DEADMAN_COUNT";
        } else if (tombstone_residency_ms >= config_.max_tombstone_residency_ms && active_tombstones > 0) {
            decision = RtpMcDecision::kSeal;
            decision_reason = "DEADMAN_RESIDENCY";
        } else if (read_state == ReadState::kCritical) {
            if (l0_files < static_cast<uint64_t>(config_.l0_hard_limit) && !inflight_flush_.load(std::memory_order_acquire)) {
                decision = RtpMcDecision::kSeal;
                decision_reason = "READ_CRITICAL";
                consecutive_yield_count_ = 0;
            } else {
                decision = RtpMcDecision::kYield;
                if (inflight_flush_.load(std::memory_order_acquire)) {
                    decision_reason = "INFLIGHT_FLUSH";
                } else {
                    decision_reason = "L0_PRESSURE";
                }
                consecutive_yield_count_++;
                if (consecutive_yield_count_ >= 3) {
                    conflict_state = "READ_MAINTENANCE_CONFLICT";
                    total_conflicts_logged_++;
                }
            }
        } else if (read_state == ReadState::kWarn) {
            if (maintenance_debt == 0.0 && !inflight_flush_.load(std::memory_order_acquire)) {
                decision = RtpMcDecision::kSeal;
                decision_reason = "READ_WARN";
                consecutive_yield_count_ = 0;
            } else {
                decision = RtpMcDecision::kYield;
                if (inflight_flush_.load(std::memory_order_acquire)) decision_reason = "INFLIGHT_FLUSH";
                else if (l0_files >= static_cast<uint64_t>(config_.l0_soft_limit)) decision_reason = "L0_PRESSURE";
                else if (put_risk > 0.0) decision_reason = "PUT_SLO_VIOLATION";
                else decision_reason = "PENDING_COMPACTION_PRESSURE";
            }
        } else { // ReadState::kOk or ReadState::kUnknown
            if (active_tombstones == 0) {
                decision = RtpMcDecision::kHold;
                decision_reason = "NO_TOMBSTONES";
            } else if (estimated_capacity_flush_ms < static_cast<double>(config_.capacity_flush_imminent_ms) && maintenance_debt == 0.0) {
                decision = RtpMcDecision::kHold;
                decision_reason = "CAPACITY_FLUSH_IMMINENT";
            } else if (!has_sufficient_read_samples) {
                decision = RtpMcDecision::kHold;
                decision_reason = "INSUFFICIENT_SAMPLES";
            } else {
                decision = RtpMcDecision::kHold;
                decision_reason = "LOW_READ_DEBT";
            }
            consecutive_yield_count_ = 0;
        }

        // 6. Action Execution
        uint64_t action_id = 0;
        if (decision == RtpMcDecision::kSeal && (mode_ == RtpMcMode::kActiveV2A || mode_ == RtpMcMode::kActiveV2B)) {
            if (!inflight_flush_.load(std::memory_order_acquire)) {
                action_id = ++action_counter_;
                inflight_flush_.store(true, std::memory_order_release);
                total_seals_triggered_++;

                uint64_t req_time_us = GetCurrentTimeUs();
                // Asynchronous Flush Request via RocksDB
                rocksdb::FlushOptions flush_opts;
                flush_opts.wait = false;
                flush_opts.allow_write_stall = false;

                if (db_) {
                    rocksdb::Status s = db_->Flush(flush_opts);
                    if (!s.ok()) {
                        inflight_flush_.store(false, std::memory_order_release);
                    }
                }

                if (actions_file_.is_open()) {
                    actions_file_ << exp_id_ << "," << action_id << "," << req_time_us << ","
                                  << "SEAL," << decision_reason << ","
                                  << "read_debt=" << std::fixed << std::setprecision(4) << read_debt
                                  << ";maint_debt=" << maintenance_debt << ";tombstones=" << active_tombstones << ","
                                  << req_time_us << ",0,0,0,0,0,0,0,REQUESTED\n";
                    actions_file_.flush();
                }
            }
        }

        // 7. Write to controller_windows.csv
        if (windows_file_.is_open()) {
            windows_file_ << exp_id_ << ","
                          << now_us << ","
                          << current_phase_.load(std::memory_order_relaxed) << ","
                          << active_tombstones << ","
                          << active_memtable_bytes << ","
                          << std::fixed << std::setprecision(2) << range_del_rate << ","
                          << std::fixed << std::setprecision(2) << estimated_capacity_flush_ms << ","
                          << getlive_samples << ","
                          << std::fixed << std::setprecision(2) << getlive_p99 << ","
                          << getdel_samples << ","
                          << std::fixed << std::setprecision(2) << getdel_p99 << ","
                          << scan_samples << ","
                          << std::fixed << std::setprecision(2) << scan_p99 << ","
                          << put_samples << ","
                          << std::fixed << std::setprecision(2) << put_p99 << ","
                          << l0_files << ","
                          << imm_memtables << ","
                          << pending_compaction_bytes << ","
                          << std::fixed << std::setprecision(4) << read_debt << ","
                          << std::fixed << std::setprecision(4) << reclaim_debt << ","
                          << std::fixed << std::setprecision(4) << maintenance_debt << ","
                          << DecisionToString(decision) << ","
                          << decision_reason << ","
                          << conflict_state << "\n";
            windows_file_.flush();
        }
        total_windows_logged_++;
    }

    static const char* DecisionToString(RtpMcDecision decision) {
        switch (decision) {
            case RtpMcDecision::kHold: return "HOLD";
            case RtpMcDecision::kSeal: return "SEAL";
            case RtpMcDecision::kPromote: return "PROMOTE";
            case RtpMcDecision::kYield: return "YIELD";
            default: return "UNKNOWN";
        }
    }

    rocksdb::DB* db_;
    FormalConfig config_;
    std::string exp_id_;
    std::vector<std::unique_ptr<WorkerHistogramBundle>>& worker_bundles_;
    std::shared_ptr<FormalEventListener> event_listener_;

    RtpMcMode mode_;
    std::atomic<bool> running_;
    std::atomic<int> current_phase_;
    std::thread controller_thread_;
    std::mutex mutex_wakeup_;
    std::condition_variable cv_wakeup_;

    std::chrono::steady_clock::time_point start_time_;
    uint64_t action_counter_;
    int consecutive_read_warn_count_;
    int consecutive_read_critical_count_;
    int consecutive_yield_count_;
    std::atomic<bool> inflight_flush_;

    uint64_t last_tombstone_count_;
    uint64_t first_tombstone_seen_time_us_;
    uint64_t last_eval_time_us_;
    uint64_t last_put_bytes_;
    double ewma_put_bytes_per_sec_;

    uint64_t total_windows_logged_;
    uint64_t total_seals_triggered_;
    uint64_t total_conflicts_logged_;

    std::ofstream windows_file_;
    std::ofstream actions_file_;
};

} // namespace study::formal
