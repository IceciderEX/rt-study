#pragma once

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
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

struct EpochReadSnapshot {
    uint64_t timestamp_us = 0;
    double duration_sec = 0.0;
    ThreadLocalHistogram hist_get_live;
    ThreadLocalHistogram hist_get_del;
    ThreadLocalHistogram hist_scan;
    ThreadLocalHistogram hist_scan_intersect;
    ThreadLocalHistogram hist_put;
};

struct ActionStateRecord {
    uint64_t action_id = 0;
    uint64_t request_time_us = 0;
    std::string action = "SEAL";
    std::string reason = "NONE";
    std::string state_before = "";
    uint64_t flush_begin_time_us = 0;
    uint64_t flush_end_time_us = 0;
    int compaction_related = 0;
    double read_relief = 0.0;
    double put_cost = 0.0;
    uint64_t flush_output_bytes = 0;
    uint64_t compaction_read_bytes = 0;
    uint64_t compaction_write_bytes = 0;
    std::string result = "REQUESTED";
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
          active_action_id_(0),
          inflight_flush_(false),
          active_memtable_generation_(0),
          last_memtable_generation_(0),
          last_active_memtable_bytes_(0),
          ewma_memtable_growth_rate_(0.0),
          last_tombstone_count_(0),
          first_tombstone_seen_time_us_(0),
          last_eval_time_us_(0),
          delayed_maintenance_debt_(0.0),
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
        {
            std::lock_guard<std::mutex> lock(mutex_actions_);
            for (auto& [id, rec] : active_actions_) {
                rec.result = "CANCELLED";
                LogActionRecord(rec);
            }
            active_actions_.clear();
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

    void NotifyFlushBegin(rocksdb::FlushReason reason, int /*job_id*/) {
        if (reason == rocksdb::FlushReason::kRangeTombstoneController) {
            uint64_t cur_action_id = active_action_id_.load(std::memory_order_acquire);
            if (cur_action_id > 0) {
                ActionStateRecord rec;
                {
                    std::lock_guard<std::mutex> lock(mutex_actions_);
                    auto it = active_actions_.find(cur_action_id);
                    if (it != active_actions_.end()) {
                        it->second.flush_begin_time_us = GetCurrentTimeUs();
                        it->second.result = "FLUSHING";
                        rec = it->second;
                    }
                }
                if (rec.action_id > 0) {
                    LogActionRecord(rec);
                }
            }
        }
    }

    void NotifyFlushCompleted(rocksdb::FlushReason reason, int /*job_id*/, uint64_t output_bytes) {
        if (reason == rocksdb::FlushReason::kRangeTombstoneController) {
            uint64_t cur_action_id = active_action_id_.load(std::memory_order_acquire);
            if (cur_action_id > 0) {
                ActionStateRecord rec;
                {
                    std::lock_guard<std::mutex> lock(mutex_actions_);
                    auto it = active_actions_.find(cur_action_id);
                    if (it != active_actions_.end()) {
                        it->second.flush_end_time_us = GetCurrentTimeUs();
                        it->second.flush_output_bytes = output_bytes;
                        it->second.result = "DONE";
                        rec = it->second;
                        active_actions_.erase(it);
                    }
                }
                if (rec.action_id > 0) {
                    LogActionRecord(rec);
                }
            }
            active_action_id_.store(0, std::memory_order_release);
            inflight_flush_.store(false, std::memory_order_release);
        }
    }

    void NotifyMemTableSealed(const rocksdb::MemTableInfo& /*info*/) {
        active_memtable_generation_.fetch_add(1, std::memory_order_release);
        uint64_t cur_action_id = active_action_id_.load(std::memory_order_acquire);
        if (cur_action_id > 0) {
            ActionStateRecord rec;
            {
                std::lock_guard<std::mutex> lock(mutex_actions_);
                auto it = active_actions_.find(cur_action_id);
                if (it != active_actions_.end() && it->second.result == "REQUESTED") {
                    it->second.result = "SWITCHED";
                    rec = it->second;
                }
            }
            if (rec.action_id > 0) {
                LogActionRecord(rec);
            }
        }
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
                windows_file_ << "exp_id,timestamp_us,phase,active_memtable_generation,active_range_tombstones,active_memtable_bytes,"
                              << "range_del_rate,estimated_capacity_flush_ms,first_tombstone_seen_time_us,getlive_samples,getlive_p99_us,"
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

    void LogActionRecord(const ActionStateRecord& rec) {
        std::lock_guard<std::mutex> lock(mutex_log_actions_);
        if (actions_file_.is_open()) {
            actions_file_ << exp_id_ << ","
                          << rec.action_id << ","
                          << rec.request_time_us << ","
                          << rec.action << ","
                          << "\"" << rec.reason << "\","
                          << "\"" << rec.state_before << "\","
                          << rec.flush_begin_time_us << ","
                          << rec.flush_end_time_us << ","
                          << rec.compaction_related << ","
                          << std::fixed << std::setprecision(4) << rec.read_relief << ","
                          << std::fixed << std::setprecision(4) << rec.put_cost << ","
                          << rec.flush_output_bytes << ","
                          << rec.compaction_read_bytes << ","
                          << rec.compaction_write_bytes << ","
                          << rec.result << "\n";
            actions_file_.flush();
        }
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

        // 1. Double-buffered snapshot aggregation across all workers for current epoch
        EpochReadSnapshot cur_snap;
        cur_snap.timestamp_us = now_us;
        cur_snap.duration_sec = dt_sec;

        for (auto& bundle : worker_bundles_) {
            bundle->hist_get_live.FreezeAndMergeInto(cur_snap.hist_get_live);
            bundle->hist_get_del.FreezeAndMergeInto(cur_snap.hist_get_del);
            bundle->hist_scan.FreezeAndMergeInto(cur_snap.hist_scan);
            bundle->hist_scan_intersect.FreezeAndMergeInto(cur_snap.hist_scan_intersect);
            bundle->hist_put.FreezeAndMergeInto(cur_snap.hist_put);
        }

        rolling_history_.push_back(std::move(cur_snap));

        // Prune older snapshots beyond rolling_window_ms, maintaining at least one
        uint64_t max_age_us = config_.rolling_window_ms * 1000;
        while (rolling_history_.size() > 1 && 
               (now_us - rolling_history_.front().timestamp_us > max_age_us)) {
            rolling_history_.pop_front();
        }

        // Aggregate across rolling history
        ThreadLocalHistogram rolling_get_live;
        ThreadLocalHistogram rolling_get_del;
        ThreadLocalHistogram rolling_scan;
        ThreadLocalHistogram rolling_scan_intersect;
        ThreadLocalHistogram rolling_put;
        double total_window_sec = 0.0;

        for (const auto& snap : rolling_history_) {
            rolling_get_live.MergeFrom(snap.hist_get_live);
            rolling_get_del.MergeFrom(snap.hist_get_del);
            rolling_scan.MergeFrom(snap.hist_scan);
            rolling_scan_intersect.MergeFrom(snap.hist_scan_intersect);
            rolling_put.MergeFrom(snap.hist_put);
            total_window_sec += snap.duration_sec;
        }
        total_window_sec = std::max(total_window_sec, 0.001);

        uint64_t getlive_samples = rolling_get_live.GetCount();
        uint64_t getdel_samples = rolling_get_del.GetCount();
        uint64_t scan_samples = rolling_scan.GetCount();
        uint64_t put_samples = rolling_put.GetCount();

        double getlive_p50 = 0.0, getlive_p90 = 0.0, getlive_p95 = 0.0, getlive_p99 = 0.0, getlive_p999 = 0.0, getlive_mean = 0.0, getlive_max = 0.0;
        if (getlive_samples >= config_.min_get_samples) {
            rolling_get_live.ComputeQuantiles(getlive_p50, getlive_p90, getlive_p95, getlive_p99, getlive_p999, getlive_mean, getlive_max);
        }

        double getdel_p50 = 0.0, getdel_p90 = 0.0, getdel_p95 = 0.0, getdel_p99 = 0.0, getdel_p999 = 0.0, getdel_mean = 0.0, getdel_max = 0.0;
        if (getdel_samples > 0) {
            rolling_get_del.ComputeQuantiles(getdel_p50, getdel_p90, getdel_p95, getdel_p99, getdel_p999, getdel_mean, getdel_max);
        }

        double scan_p50 = 0.0, scan_p90 = 0.0, scan_p95 = 0.0, scan_p99 = 0.0, scan_p999 = 0.0, scan_mean = 0.0, scan_max = 0.0;
        if (scan_samples >= config_.min_scan_samples) {
            rolling_scan.ComputeQuantiles(scan_p50, scan_p90, scan_p95, scan_p99, scan_p999, scan_mean, scan_max);
        }

        double put_p50 = 0.0, put_p90 = 0.0, put_p95 = 0.0, put_p99 = 0.0, put_p999 = 0.0, put_mean = 0.0, put_max = 0.0;
        if (put_samples >= config_.min_put_samples) {
            rolling_put.ComputeQuantiles(put_p50, put_p90, put_p95, put_p99, put_p999, put_mean, put_max);
        }

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

        // 3. Compute Rates & Tracking (Generation-aware & MemTable Growth-based)
        uint64_t cur_generation = active_memtable_generation_.load(std::memory_order_acquire);
        bool generation_changed = (cur_generation != last_memtable_generation_) || 
                                  (active_memtable_bytes < last_active_memtable_bytes_ && last_active_memtable_bytes_ > 0);
        if (generation_changed) {
            last_memtable_generation_ = cur_generation;
            first_tombstone_seen_time_us_ = (active_tombstones > 0) ? now_us : 0;
            last_active_memtable_bytes_ = active_memtable_bytes;
        } else {
            if (active_tombstones > 0) {
                if (first_tombstone_seen_time_us_ == 0) {
                    first_tombstone_seen_time_us_ = now_us;
                }
            } else {
                first_tombstone_seen_time_us_ = 0;
            }
        }

        uint64_t tombstone_residency_ms = (first_tombstone_seen_time_us_ > 0 && now_us >= first_tombstone_seen_time_us_) ?
            (now_us - first_tombstone_seen_time_us_) / 1000 : 0;

        double range_del_rate = (active_tombstones >= last_tombstone_count_) ?
            (active_tombstones - last_tombstone_count_) / dt_sec : 0.0;
        last_tombstone_count_ = active_tombstones;

        // Actual MemTable Growth Rate Estimation (accounting for all mutations & arena structures)
        if (!generation_changed && active_memtable_bytes >= last_active_memtable_bytes_) {
            uint64_t delta_bytes = active_memtable_bytes - last_active_memtable_bytes_;
            double growth_bps = static_cast<double>(delta_bytes) / dt_sec;
            if (ewma_memtable_growth_rate_ == 0.0) {
                ewma_memtable_growth_rate_ = growth_bps;
            } else {
                ewma_memtable_growth_rate_ = 0.7 * ewma_memtable_growth_rate_ + 0.3 * growth_bps;
            }
        }
        last_active_memtable_bytes_ = active_memtable_bytes;

        double estimated_capacity_flush_ms = 999999.0;
        if (active_memtable_bytes < config_.write_buffer_size) {
            uint64_t remaining_bytes = config_.write_buffer_size - active_memtable_bytes;
            if (ewma_memtable_growth_rate_ > 1024.0) {
                estimated_capacity_flush_ms = (static_cast<double>(remaining_bytes) / ewma_memtable_growth_rate_) * 1000.0;
            }
        } else {
            estimated_capacity_flush_ms = 0.0;
        }

        // 4. Compute 3 Debts
        // A. Read Debt (strictly excluding GetDeleted, using true arrival rate q_scan & q_getlive)
        double q_scan = static_cast<double>(scan_samples) / total_window_sec;
        double q_getlive = static_cast<double>(getlive_samples) / total_window_sec;

        bool has_scan_samples = (scan_samples >= config_.min_scan_samples);
        bool has_get_samples = (getlive_samples >= config_.min_get_samples);
        bool has_sufficient_read_samples = (has_scan_samples || has_get_samples);

        double scan_over = (config_.scan_slo_us > 0.0 && has_scan_samples) ?
            std::max(0.0, (scan_p99 / config_.scan_slo_us) - 1.0) : 0.0;
        double get_over = (config_.getlive_slo_us > 0.0 && has_get_samples) ?
            std::max(0.0, (getlive_p99 / config_.getlive_slo_us) - 1.0) : 0.0;

        double ref_rate = std::max(config_.ref_read_rate, 1.0);
        double read_debt = 0.0;
        if (has_sufficient_read_samples) {
            read_debt = (q_scan * scan_over + q_getlive * get_over) / ref_rate;
        }

        // Read State Classification
        enum class ReadState { kOk, kUnknown, kWarn, kCritical };
        ReadState read_state = ReadState::kOk;
        if (!has_sufficient_read_samples) {
            read_state = ReadState::kUnknown;
            consecutive_read_warn_count_ = 0;
            consecutive_read_critical_count_ = 0;
        } else if (read_debt <= 0.0) {
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

        // C. Maintenance Debt and Boolean Protection Zones
        bool has_inflight_flush = inflight_flush_.load(std::memory_order_acquire);
        bool put_slo_violated = (config_.put_slo_us > 0.0 && put_samples >= config_.min_put_samples && put_p99 > config_.put_slo_us);
        bool put_critical_violated = (config_.put_slo_us > 0.0 && put_samples >= config_.min_put_samples && put_p99 >= config_.put_slo_us * 2.0);
        bool l0_at_or_above_soft = (l0_files >= static_cast<uint64_t>(config_.l0_soft_limit));
        bool l0_at_or_above_hard = (l0_files >= static_cast<uint64_t>(config_.l0_hard_limit));
        bool pending_at_or_above_soft = (config_.pending_compaction_soft_bytes > 0 && pending_compaction_bytes >= config_.pending_compaction_soft_bytes);
        bool pending_at_or_above_hard = (config_.pending_compaction_hard_bytes > 0 && pending_compaction_bytes >= config_.pending_compaction_hard_bytes);

        bool maintenance_safe = !put_slo_violated && 
                                !l0_at_or_above_soft && 
                                !pending_at_or_above_soft && 
                                !has_inflight_flush;

        bool maintenance_critical = l0_at_or_above_hard || 
                                    pending_at_or_above_hard || 
                                    put_critical_violated ||
                                    has_inflight_flush;

        double put_risk = (config_.put_slo_us > 0.0 && put_samples >= config_.min_put_samples) ?
            std::max(0.0, (put_p99 / config_.put_slo_us) - 1.0) : 0.0;
        double l0_risk = (l0_files >= static_cast<uint64_t>(config_.l0_soft_limit)) ?
            static_cast<double>(l0_files - config_.l0_soft_limit) / std::max(1, config_.l0_hard_limit - config_.l0_soft_limit) : 0.0;
        double pending_risk = (pending_compaction_bytes >= config_.pending_compaction_soft_bytes) ?
            static_cast<double>(pending_compaction_bytes - config_.pending_compaction_soft_bytes) / 
            std::max<double>(1.0, config_.pending_compaction_hard_bytes - config_.pending_compaction_soft_bytes) : 0.0;
        double inflight_risk = has_inflight_flush ? 2.0 : 0.0;

        double maintenance_debt = put_risk + l0_risk + pending_risk + inflight_risk;

        // 5. Decision State Machine
        RtpMcDecision decision = RtpMcDecision::kHold;
        std::string decision_reason = "NONE";
        std::string conflict_state = "NORMAL";

        // Safety Deadman Bounds
        if (active_tombstones >= config_.max_range_del_deadman) {
            decision = RtpMcDecision::kSeal;
            decision_reason = "DEADMAN_COUNT";
            consecutive_yield_count_ = 0;
            conflict_state = "NORMAL";
        } else if (tombstone_residency_ms >= config_.max_tombstone_residency_ms && active_tombstones > 0) {
            decision = RtpMcDecision::kSeal;
            decision_reason = "DEADMAN_RESIDENCY";
            consecutive_yield_count_ = 0;
            conflict_state = "NORMAL";
        } else if (read_state == ReadState::kCritical) {
            if (!maintenance_critical) {
                decision = RtpMcDecision::kSeal;
                decision_reason = "READ_CRITICAL";
                consecutive_yield_count_ = 0;
                conflict_state = "NORMAL";
            } else {
                decision = RtpMcDecision::kYield;
                if (has_inflight_flush) decision_reason = "INFLIGHT_FLUSH";
                else if (l0_at_or_above_hard) decision_reason = "L0_HARD_PRESSURE";
                else if (pending_at_or_above_hard) decision_reason = "PENDING_COMPACTION_HARD_PRESSURE";
                else if (put_critical_violated) decision_reason = "PUT_CRITICAL_VIOLATION";
                else decision_reason = "MAINTENANCE_CRITICAL";

                consecutive_yield_count_++;
                conflict_state = "READ_MAINTENANCE_CONFLICT";
                total_conflicts_logged_++;
                delayed_maintenance_debt_ += read_debt;
            }
        } else if (read_state == ReadState::kWarn) {
            if (maintenance_safe) {
                decision = RtpMcDecision::kSeal;
                decision_reason = "READ_WARN";
                consecutive_yield_count_ = 0;
                conflict_state = "NORMAL";
            } else {
                decision = RtpMcDecision::kYield;
                if (has_inflight_flush) decision_reason = "INFLIGHT_FLUSH";
                else if (l0_at_or_above_soft) decision_reason = "L0_PRESSURE";
                else if (pending_at_or_above_soft) decision_reason = "PENDING_COMPACTION_PRESSURE";
                else if (put_slo_violated) decision_reason = "PUT_SLO_VIOLATION";
                else decision_reason = "MAINTENANCE_DEFER";

                consecutive_yield_count_++;
                delayed_maintenance_debt_ += read_debt;
                if (consecutive_yield_count_ >= 3 && maintenance_critical) {
                    conflict_state = "READ_MAINTENANCE_CONFLICT";
                    total_conflicts_logged_++;
                }
            }
        } else if (read_state == ReadState::kOk) {
            if (active_tombstones == 0) {
                decision = RtpMcDecision::kHold;
                decision_reason = "NO_TOMBSTONES";
            } else if (estimated_capacity_flush_ms < static_cast<double>(config_.capacity_flush_imminent_ms) && maintenance_safe) {
                decision = RtpMcDecision::kHold;
                decision_reason = "CAPACITY_FLUSH_IMMINENT";
            } else {
                decision = RtpMcDecision::kHold;
                decision_reason = "LOW_READ_DEBT";
            }
            consecutive_yield_count_ = 0;
            conflict_state = "NORMAL";
        } else { // ReadState::kUnknown
            decision = RtpMcDecision::kHold;
            decision_reason = "INSUFFICIENT_SAMPLES";
            consecutive_yield_count_ = 0;
            conflict_state = "NORMAL";
        }

        // 6. Action Execution (with exact action binding & status tracking)
        uint64_t action_id = 0;
        if (decision == RtpMcDecision::kSeal && (mode_ == RtpMcMode::kActiveV2A || mode_ == RtpMcMode::kActiveV2B)) {
            if (!inflight_flush_.load(std::memory_order_acquire)) {
                action_id = ++action_counter_;
                active_action_id_.store(action_id, std::memory_order_release);
                inflight_flush_.store(true, std::memory_order_release);
                total_seals_triggered_++;

                uint64_t req_time_us = GetCurrentTimeUs();
                std::string state_before = "read_debt=" + std::to_string(read_debt) + 
                                           ";maint_debt=" + std::to_string(maintenance_debt) + 
                                           ";tombstones=" + std::to_string(active_tombstones);

                ActionStateRecord rec;
                rec.action_id = action_id;
                rec.request_time_us = req_time_us;
                rec.action = "SEAL";
                rec.reason = decision_reason;
                rec.state_before = state_before;
                rec.result = "REQUESTED";

                // Asynchronous Flush Request via RocksDB with exact Controller Reason
                rocksdb::FlushOptions flush_opts;
                flush_opts.wait = false;
                flush_opts.allow_write_stall = false;
                flush_opts.flush_reason = rocksdb::FlushReason::kRangeTombstoneController;

                rocksdb::Status s = rocksdb::Status::OK();
                if (db_) {
                    s = db_->Flush(flush_opts);
                }

                if (!s.ok()) {
                    rec.result = "FAILED";
                    rec.reason += " (" + s.ToString() + ")";
                    inflight_flush_.store(false, std::memory_order_release);
                    active_action_id_.store(0, std::memory_order_release);
                    LogActionRecord(rec);
                } else {
                    {
                        std::lock_guard<std::mutex> lock(mutex_actions_);
                        active_actions_[action_id] = rec;
                    }
                    LogActionRecord(rec);
                }
            }
        }

        // 7. Write to controller_windows.csv
        if (windows_file_.is_open()) {
            windows_file_ << exp_id_ << ","
                          << now_us << ","
                          << current_phase_.load(std::memory_order_relaxed) << ","
                          << cur_generation << ","
                          << active_tombstones << ","
                          << active_memtable_bytes << ","
                          << std::fixed << std::setprecision(2) << range_del_rate << ","
                          << std::fixed << std::setprecision(2) << estimated_capacity_flush_ms << ","
                          << first_tombstone_seen_time_us_ << ","
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

    std::atomic<uint64_t> active_action_id_;
    std::atomic<bool> inflight_flush_;

    std::atomic<uint64_t> active_memtable_generation_;
    uint64_t last_memtable_generation_;
    uint64_t last_active_memtable_bytes_;
    double ewma_memtable_growth_rate_;

    uint64_t last_tombstone_count_;
    uint64_t first_tombstone_seen_time_us_;
    uint64_t last_eval_time_us_;

    std::deque<EpochReadSnapshot> rolling_history_;
    double delayed_maintenance_debt_;

    uint64_t total_windows_logged_;
    uint64_t total_seals_triggered_;
    uint64_t total_conflicts_logged_;

    std::mutex mutex_actions_;
    std::unordered_map<uint64_t, ActionStateRecord> active_actions_;
    std::mutex mutex_log_actions_;

    std::ofstream windows_file_;
    std::ofstream actions_file_;
};

} // namespace study::formal
