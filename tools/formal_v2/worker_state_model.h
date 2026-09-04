#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace study::formal {

enum class ExpectedState : uint8_t {
    kExpectedLive = 0,
    kExpectedDeleted = 1
};

class WorkerStateModel {
public:
    WorkerStateModel(uint64_t start_k, uint64_t end_k, size_t value_size)
        : start_k_(start_k),
          end_k_(end_k),
          num_keys_(end_k >= start_k ? end_k - start_k : 0),
          value_size_(value_size) 
    {
        // Preload baseline: all keys live with version 1
        is_live_.assign(num_keys_, 1);
        version_.assign(num_keys_, 1);
    }

    inline ExpectedState ClassifyGet(uint64_t k) const noexcept {
        assert(k >= start_k_ && k < end_k_);
        return is_live_[k - start_k_] ? ExpectedState::kExpectedLive : ExpectedState::kExpectedDeleted;
    }

    inline void ApplyPut(uint64_t k) noexcept {
        assert(k >= start_k_ && k < end_k_);
        size_t idx = k - start_k_;
        is_live_[idx] = 1;
        version_[idx]++;
    }

    inline void ApplyDeleteRange(uint64_t b, uint64_t e) noexcept {
        assert(b >= start_k_ && e <= end_k_ && b < e);
        size_t idx_b = b - start_k_;
        size_t idx_e = e - start_k_;
        std::fill(is_live_.begin() + idx_b, is_live_.begin() + idx_e, 0);
    }

    uint64_t GetStartKey() const noexcept { return start_k_; }
    uint64_t GetEndKey() const noexcept { return end_k_; }
    size_t GetNumKeys() const noexcept { return num_keys_; }

    inline uint64_t CountExpectedLiveKeys(uint64_t b, uint64_t e) const noexcept {
        if (b < start_k_) b = start_k_;
        if (e > end_k_) e = end_k_;
        if (b >= e) return 0;
        uint64_t cnt = 0;
        size_t idx_b = b - start_k_;
        size_t idx_e = e - start_k_;
        for (size_t i = idx_b; i < idx_e; ++i) {
            if (is_live_[i]) cnt++;
        }
        return cnt;
    }

    inline bool IsKeyLive(uint64_t k) const noexcept {
        assert(k >= start_k_ && k < end_k_);
        return is_live_[k - start_k_] != 0;
    }

    inline uint32_t GetKeyVersion(uint64_t k) const noexcept {
        assert(k >= start_k_ && k < end_k_);
        return version_[k - start_k_];
    }

    static std::string FormatKey(uint64_t k) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%016llu", static_cast<unsigned long long>(k));
        return std::string(buf);
    }

    static uint64_t ParseKey(const std::string& key_str) {
        if (key_str.rfind("key_", 0) == 0) {
            return std::stoull(key_str.substr(4));
        }
        return UINT64_MAX;
    }

    static std::string GenerateValue(uint64_t k, uint32_t version, size_t size) {
        std::string val;
        val.reserve(size);
        char header[64];
        int hlen = snprintf(header, sizeof(header), "v%u_k%016llu_", version, static_cast<unsigned long long>(k));
        val.append(header, hlen);
        if (val.size() < size) {
            val.append(size - val.size(), 'x');
        }
        if (val.size() > size) {
            val.resize(size);
        }
        return val;
    }

private:
    uint64_t start_k_;
    uint64_t end_k_;
    size_t num_keys_;
    size_t value_size_;
    std::vector<uint8_t> is_live_;
    std::vector<uint32_t> version_;
};

} // namespace study::formal
