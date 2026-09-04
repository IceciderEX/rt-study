#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <functional>

#include "rocksdb/listener.h"
#include "rocksdb/db.h"
#include "rocksdb/compaction_job_stats.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/customizable.h"

namespace study::formal {

enum class EventStage : uint8_t {
    kPreload = 0,
    kForeground = 1,
    kCooldown = 2,
    kVerification = 3
};

struct RawFlushEvent {
    int job_id;
    EventStage stage;
    rocksdb::FlushReason flush_reason;
    uint64_t smallest_seqno;
    uint64_t largest_seqno;
    uint64_t engine_out_bytes;
    double timestamp_sec;
    std::string file_path;
};

struct RawCompactionEvent {
    int job_id;
    EventStage stage;
    int base_input_level;
    int output_level;
    uint64_t total_input_bytes;
    uint64_t total_output_bytes;
    rocksdb::CompactionReason compaction_reason;
    double timestamp_sec;
};

class FormalEventListener : public rocksdb::EventListener {
public:
    FormalEventListener() 
        : current_stage_(EventStage::kPreload),
          init_time_(std::chrono::steady_clock::now()),
          exp_start_time_(init_time_),
          cooldown_start_time_(init_time_),
          verification_start_time_(init_time_),
          fg_flush_bytes_(0), 
          fg_compaction_read_bytes_(0), 
          fg_compaction_write_bytes_(0),
          cooldown_flush_bytes_(0),
          cooldown_compaction_read_bytes_(0),
          cooldown_compaction_write_bytes_(0),
          ver_flush_bytes_(0),
          ver_compaction_read_bytes_(0),
          ver_compaction_write_bytes_(0)
    {
        events_flush_.reserve(2048);
        events_compaction_.reserve(2048);
    }

    ~FormalEventListener() override = default;

    const char* Name() const override {
        return "FormalEventListener";
    }

    // 1. Called after Preload + WaitForCompact settle to begin foreground experiment window
    void StartForegroundExperiment() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_stage_ = EventStage::kForeground;
        exp_start_time_ = std::chrono::steady_clock::now();
    }

    // 2. Called when Phase C barrier finishes to begin post-experiment cooldown observation window
    void StartCooldownObservation() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_stage_ = EventStage::kCooldown;
        cooldown_start_time_ = std::chrono::steady_clock::now();
    }

    // 3. Called when strict 10s cooldown expires, before starting full KV verification
    void StartVerificationStage() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_stage_ = EventStage::kVerification;
        verification_start_time_ = std::chrono::steady_clock::now();
    }

    void SetFlushBeginCallback(std::function<void(rocksdb::FlushReason, int)> cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_flush_begin_cb_ = cb;
    }

    void SetFlushCompletedCallback(std::function<void(rocksdb::FlushReason, int, uint64_t)> cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_flush_completed_cb_ = cb;
    }

    void SetMemTableSealedCallback(std::function<void(const rocksdb::MemTableInfo&)> cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_memtable_sealed_cb_ = cb;
    }

    void OnFlushBegin(rocksdb::DB* /*db*/, const rocksdb::FlushJobInfo& info) override {
        std::function<void(rocksdb::FlushReason, int)> cb_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb_copy = on_flush_begin_cb_;
        }
        if (cb_copy) {
            cb_copy(info.flush_reason, info.job_id);
        }
    }

    void OnMemTableSealed(const rocksdb::MemTableInfo& info) override {
        std::function<void(const rocksdb::MemTableInfo&)> cb_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sealed_memtable_count_++;
            cb_copy = on_memtable_sealed_cb_;
        }
        if (cb_copy) {
            cb_copy(info);
        }
    }

    // Ultra-lightweight callback: zero filesystem I/O, zero string conversions
    void OnFlushCompleted(rocksdb::DB* /*db*/, const rocksdb::FlushJobInfo& info) override {
        auto now = std::chrono::steady_clock::now();

        uint64_t out_bytes = (info.table_properties.data_size > 0) ? 
            info.table_properties.data_size : 
            (info.table_properties.raw_key_size + info.table_properties.raw_value_size);

        std::function<void(rocksdb::FlushReason, int, uint64_t)> cb_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            double elapsed = 0.0;
            if (current_stage_ == EventStage::kPreload) {
                elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - init_time_).count();
            } else if (current_stage_ == EventStage::kForeground) {
                elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
                fg_flush_bytes_ += out_bytes;
            } else if (current_stage_ == EventStage::kCooldown) {
                elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
                cooldown_flush_bytes_ += out_bytes;
            } else { // kVerification
                elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
                ver_flush_bytes_ += out_bytes;
            }

            RawFlushEvent rec;
            rec.job_id = info.job_id;
            rec.stage = current_stage_;
            rec.flush_reason = info.flush_reason;
            rec.smallest_seqno = info.smallest_seqno;
            rec.largest_seqno = info.largest_seqno;
            rec.engine_out_bytes = out_bytes;
            rec.timestamp_sec = elapsed;
            rec.file_path = info.file_path;

            events_flush_.push_back(rec);
            cb_copy = on_flush_completed_cb_;
        }

        if (cb_copy) {
            cb_copy(info.flush_reason, info.job_id, out_bytes);
        }
    }

    // Ultra-lightweight callback: zero filesystem I/O, zero string conversions
    void OnCompactionCompleted(rocksdb::DB* /*db*/, const rocksdb::CompactionJobInfo& info) override {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mutex_);

        double elapsed = 0.0;
        if (current_stage_ == EventStage::kPreload) {
            elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - init_time_).count();
        } else if (current_stage_ == EventStage::kForeground) {
            elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
            fg_compaction_read_bytes_ += info.stats.total_input_bytes;
            fg_compaction_write_bytes_ += info.stats.total_output_bytes;
        } else if (current_stage_ == EventStage::kCooldown) {
            elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
            cooldown_compaction_read_bytes_ += info.stats.total_input_bytes;
            cooldown_compaction_write_bytes_ += info.stats.total_output_bytes;
        } else { // kVerification
            elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - exp_start_time_).count();
            ver_compaction_read_bytes_ += info.stats.total_input_bytes;
            ver_compaction_write_bytes_ += info.stats.total_output_bytes;
        }

        RawCompactionEvent rec;
        rec.job_id = info.job_id;
        rec.stage = current_stage_;
        rec.base_input_level = info.base_input_level;
        rec.output_level = info.output_level;
        rec.total_input_bytes = info.stats.total_input_bytes;
        rec.total_output_bytes = info.stats.total_output_bytes;
        rec.compaction_reason = info.compaction_reason;
        rec.timestamp_sec = elapsed;

        events_compaction_.push_back(rec);
    }

    // Foreground Metric Queries
    uint64_t GetForegroundFlushCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (const auto& f : events_flush_) {
            if (f.stage == EventStage::kForeground) cnt++;
        }
        return cnt;
    }

    uint64_t GetForegroundFlushCountByReason(rocksdb::FlushReason reason) const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (const auto& f : events_flush_) {
            if (f.stage == EventStage::kForeground && f.flush_reason == reason) {
                ++cnt;
            }
        }
        return cnt;
    }

    uint64_t GetForegroundFlushBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_flush_bytes_;
    }

    uint64_t GetForegroundCompactionWriteBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_compaction_write_bytes_;
    }

    uint64_t GetForegroundCompactionReadBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_compaction_read_bytes_;
    }

    // Strictly Foreground + 10s Cooldown Queries (Excludes Verification)
    uint64_t GetTotalExperimentFlushCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (const auto& f : events_flush_) {
            if (f.stage == EventStage::kForeground || f.stage == EventStage::kCooldown) cnt++;
        }
        return cnt;
    }

    uint64_t GetTotalExperimentFlushCountByReason(
        rocksdb::FlushReason reason) const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (const auto& f : events_flush_) {
            if ((f.stage == EventStage::kForeground ||
                 f.stage == EventStage::kCooldown) &&
                f.flush_reason == reason) {
                ++cnt;
            }
        }
        return cnt;
    }

    uint64_t GetTotalExperimentFlushBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_flush_bytes_ + cooldown_flush_bytes_;
    }

    uint64_t GetTotalExperimentCompactionWriteBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_compaction_write_bytes_ + cooldown_compaction_write_bytes_;
    }

    uint64_t GetTotalExperimentCompactionReadBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fg_compaction_read_bytes_ + cooldown_compaction_read_bytes_;
    }

    // Offline post-run CSV export
    void DumpEventsCsv(const std::string& csv_path, const std::string& exp_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        bool write_header = false;
        {
            std::ifstream check(csv_path);
            if (!check.good()) write_header = true;
        }

        std::ofstream fout(csv_path, std::ios::app);
        if (!fout.is_open()) return;

        if (write_header) {
            fout << "exp_id,stage,event_type,job_id,timestamp_sec,reason,size_or_out_bytes,extra_info\n";
        }

        auto stage_str = [](EventStage st) {
            switch (st) {
                case EventStage::kPreload: return "PRELOAD";
                case EventStage::kForeground: return "FOREGROUND";
                case EventStage::kCooldown: return "COOLDOWN";
                case EventStage::kVerification: return "VERIFICATION";
                default: return "UNKNOWN";
            }
        };

        for (const auto& f : events_flush_) {
            const char* reason_str = rocksdb::GetFlushReasonString(f.flush_reason);
            fout << exp_id << "," << stage_str(f.stage) << ",FLUSH," << f.job_id << "," 
                 << std::fixed << std::setprecision(4) << f.timestamp_sec
                 << ",\"" << (reason_str ? reason_str : "kUnknown") << "\"," << f.engine_out_bytes << ",\"" << f.file_path << "\"\n";
        }

        for (const auto& c : events_compaction_) {
            fout << exp_id << "," << stage_str(c.stage) << ",COMPACTION," << c.job_id << "," 
                 << std::fixed << std::setprecision(4) << c.timestamp_sec
                 << ",\"" << CompactionReasonToString(c.compaction_reason) << "\"," << c.total_output_bytes << ",\"in_level=" 
                 << c.base_input_level << ";out_level=" << c.output_level << ";in_bytes=" << c.total_input_bytes << "\"\n";
        }
    }

    uint64_t GetSealedMemTableCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sealed_memtable_count_;
    }

private:
    static std::string CompactionReasonToString(rocksdb::CompactionReason reason) {
        switch (reason) {
            case rocksdb::CompactionReason::kLevelL0FilesNum: return "kLevelL0FilesNum";
            case rocksdb::CompactionReason::kLevelMaxLevelSize: return "kLevelMaxLevelSize";
            case rocksdb::CompactionReason::kManualCompaction: return "kManualCompaction";
            case rocksdb::CompactionReason::kFilesMarkedForCompaction: return "kFilesMarkedForCompaction";
            default: return "kOther(" + std::to_string(static_cast<int>(reason)) + ")";
        }
    }

    mutable std::mutex mutex_;
    EventStage current_stage_;
    std::chrono::steady_clock::time_point init_time_;
    std::chrono::steady_clock::time_point exp_start_time_;
    std::chrono::steady_clock::time_point cooldown_start_time_;
    std::chrono::steady_clock::time_point verification_start_time_;

    std::vector<RawFlushEvent> events_flush_;
    std::vector<RawCompactionEvent> events_compaction_;

    uint64_t fg_flush_bytes_;
    uint64_t fg_compaction_read_bytes_;
    uint64_t fg_compaction_write_bytes_;

    uint64_t cooldown_flush_bytes_;
    uint64_t cooldown_compaction_read_bytes_;
    uint64_t cooldown_compaction_write_bytes_;

    uint64_t ver_flush_bytes_;
    uint64_t ver_compaction_read_bytes_;
    uint64_t ver_compaction_write_bytes_;

    uint64_t sealed_memtable_count_ = 0;

    std::function<void(rocksdb::FlushReason, int)> on_flush_begin_cb_;
    std::function<void(rocksdb::FlushReason, int, uint64_t)> on_flush_completed_cb_;
    std::function<void(const rocksdb::MemTableInfo&)> on_memtable_sealed_cb_;
};

} // namespace study::formal
