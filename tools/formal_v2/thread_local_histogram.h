#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <thread>

namespace study::formal {

class alignas(64) ThreadLocalHistogram {
public:
    static constexpr size_t kLinearBuckets = 1000; // 0 ns to 999 ns (1 ns resolution)
    static constexpr size_t kLogBuckets = 30000;   // 1 us to 60 s (log scale: ~0.05% bin resolution)
    static constexpr size_t kTotalBuckets = kLinearBuckets + kLogBuckets;

    ThreadLocalHistogram() : count_(0), sum_ns_(0), max_ns_(0), min_ns_(UINT64_MAX) {
        buckets_.assign(kTotalBuckets, 0);
    }

    inline void Record(uint64_t lat_ns) noexcept {
        count_++;
        sum_ns_ += lat_ns;
        if (lat_ns > max_ns_) max_ns_ = lat_ns;
        if (lat_ns < min_ns_) min_ns_ = lat_ns;

        if (lat_ns < kLinearBuckets) {
            buckets_[lat_ns]++;
        } else {
            // Logarithmic mapping: bucket = 1000 + 3800 * log10(lat_ns / 1000.0)
            double us = static_cast<double>(lat_ns) / 1000.0;
            size_t log_idx = static_cast<size_t>(std::log10(us) * 3800.0);
            size_t b = kLinearBuckets + std::min(log_idx, kLogBuckets - 1);
            buckets_[b]++;
        }
    }

    void MergeFrom(const ThreadLocalHistogram& other) {
        count_ += other.count_;
        sum_ns_ += other.sum_ns_;
        if (other.max_ns_ > max_ns_) max_ns_ = other.max_ns_;
        if (other.min_ns_ < min_ns_) min_ns_ = other.min_ns_;

        for (size_t i = 0; i < kTotalBuckets; ++i) {
            buckets_[i] += other.buckets_[i];
        }
    }

    void Reset() {
        count_ = 0;
        sum_ns_ = 0;
        max_ns_ = 0;
        min_ns_ = UINT64_MAX;
        std::fill(buckets_.begin(), buckets_.end(), 0);
    }

    uint64_t GetCount() const noexcept { return count_; }
    uint64_t GetSumNs() const noexcept { return sum_ns_; }
    uint64_t GetMaxNs() const noexcept { return max_ns_; }
    uint64_t GetMinNs() const noexcept { return count_ > 0 ? min_ns_ : 0; }

    void ComputeQuantiles(
        double& p50_us, double& p90_us, double& p95_us, 
        double& p99_us, double& p999_us, double& mean_us, double& max_us) const 
    {
        if (count_ == 0) {
            p50_us = p90_us = p95_us = p99_us = p999_us = mean_us = max_us = 0.0;
            return;
        }

        mean_us = (static_cast<double>(sum_ns_) / count_) / 1000.0;
        max_us = static_cast<double>(max_ns_) / 1000.0;

        uint64_t target_50 = static_cast<uint64_t>(std::ceil(count_ * 0.50));
        uint64_t target_90 = static_cast<uint64_t>(std::ceil(count_ * 0.90));
        uint64_t target_95 = static_cast<uint64_t>(std::ceil(count_ * 0.95));
        uint64_t target_99 = static_cast<uint64_t>(std::ceil(count_ * 0.99));
        uint64_t target_999 = static_cast<uint64_t>(std::ceil(count_ * 0.999));

        uint64_t cum = 0;
        p50_us = p90_us = p95_us = p99_us = p999_us = 0.0;

        for (size_t b = 0; b < kTotalBuckets; ++b) {
            cum += buckets_[b];
            if (cum == 0) continue;

            double val_us = BucketToMicroseconds(b);

            if (p50_us == 0.0 && cum >= target_50) p50_us = val_us;
            if (p90_us == 0.0 && cum >= target_90) p90_us = val_us;
            if (p95_us == 0.0 && cum >= target_95) p95_us = val_us;
            if (p99_us == 0.0 && cum >= target_99) p99_us = val_us;
            if (p999_us == 0.0 && cum >= target_999) {
                p999_us = val_us;
                break;
            }
        }
        if (p50_us == 0.0) p50_us = max_us;
        if (p90_us == 0.0) p90_us = max_us;
        if (p95_us == 0.0) p95_us = max_us;
        if (p99_us == 0.0) p99_us = max_us;
        if (p999_us == 0.0) p999_us = max_us;
    }

private:
    static inline double BucketToMicroseconds(size_t bucket) noexcept {
        if (bucket < kLinearBuckets) {
            return static_cast<double>(bucket) / 1000.0;
        } else {
            size_t log_idx = bucket - kLinearBuckets;
            double us = std::pow(10.0, static_cast<double>(log_idx) / 3800.0);
            return us;
        }
    }

    uint64_t count_;
    uint64_t sum_ns_;
    uint64_t max_ns_;
    uint64_t min_ns_;
    std::vector<uint64_t> buckets_;
};

class alignas(64) DoubleBufferedHistogram {
public:
    DoubleBufferedHistogram() {
        epoch_.store(0, std::memory_order_relaxed);
        writers_[0].store(0, std::memory_order_relaxed);
        writers_[1].store(0, std::memory_order_relaxed);
    }

    DoubleBufferedHistogram(const DoubleBufferedHistogram&) = delete;
    DoubleBufferedHistogram& operator=(const DoubleBufferedHistogram&) = delete;

    inline void Record(uint64_t lat_ns) noexcept {
        while (true) {
            uint64_t epoch = epoch_.load(std::memory_order_acquire);
            uint32_t slot = static_cast<uint32_t>(epoch & 1);

            writers_[slot].fetch_add(1, std::memory_order_acquire);

            if (epoch_.load(std::memory_order_acquire) == epoch) {
                buffers_[slot].Record(lat_ns);
                writers_[slot].fetch_sub(1, std::memory_order_release);
                break;
            }

            writers_[slot].fetch_sub(1, std::memory_order_release);
        }
    }

    // Called by Controller thread at epoch boundary.
    // Advances epoch to freeze current active slot, waits for all in-flight writers on the frozen slot
    // to finish, merges the frozen buffer into target, and resets the frozen buffer.
    inline void FreezeAndMergeInto(ThreadLocalHistogram& target) {
        uint64_t old_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel);
        uint32_t frozen_slot = static_cast<uint32_t>(old_epoch & 1);

        while (writers_[frozen_slot].load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        target.MergeFrom(buffers_[frozen_slot]);
        buffers_[frozen_slot].Reset();
    }

    template <typename Func>
    inline void FreezeExtractAndReset(Func&& fn) {
        uint64_t old_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel);
        uint32_t frozen_slot = static_cast<uint32_t>(old_epoch & 1);

        while (writers_[frozen_slot].load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        fn(buffers_[frozen_slot]);
        buffers_[frozen_slot].Reset();
    }

    void ResetAll() {
        while (writers_[0].load(std::memory_order_acquire) != 0 || 
               writers_[1].load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        buffers_[0].Reset();
        buffers_[1].Reset();
        epoch_.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<uint64_t> epoch_{0};
    alignas(64) std::atomic<uint32_t> writers_[2]{{0}, {0}};
    ThreadLocalHistogram buffers_[2];
};

} // namespace study::formal
