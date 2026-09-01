#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <iomanip>
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/status.h"

namespace study {

enum GetCategory {
    GET_AFFECTED_LIVE = 0,
    GET_DELETED = 1,
    GET_CONTROL = 2
};

struct TombstoneInterval {
    uint64_t begin;
    uint64_t end; // [begin, end)
    uint64_t op_seq;
};

class ReferenceModel {
public:
    ReferenceModel(uint64_t total_keys, size_t val_size)
        : total_keys_(total_keys), val_size_(val_size),
          is_alive_(total_keys, false),
          affected_live_flags_(total_keys, 0),
          key_version_(total_keys, 0) {}

    // Formats integer key to fixed-width string (key_000000000000)
    // Ensures lexicographical order is identical to numerical order
    static std::string FormatKey(uint64_t key_id) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "key_%012llu", static_cast<unsigned long long>(key_id));
        return std::string(buf);
    }

    // Parses fixed-width string back to integer key
    static bool ParseKey(const std::string& key_str, uint64_t& key_id) {
        if (key_str.rfind("key_", 0) != 0) return false;
        try {
            key_id = std::stoull(key_str.substr(4));
            return true;
        } catch (...) {
            return false;
        }
    }

    // Generates deterministic value for key
    static std::string GenerateValue(uint64_t key_id, uint32_t version, size_t size) {
        std::string val;
        val.resize(size);
        char header[64];
        int hlen = std::snprintf(header, sizeof(header), "v%08u_k%012llu_", version, static_cast<unsigned long long>(key_id));
        if (hlen > 0 && static_cast<size_t>(hlen) < size) {
            std::memcpy(&val[0], header, hlen);
            for (size_t i = hlen; i < size; ++i) {
                val[i] = static_cast<char>('A' + ((key_id + i) % 26));
            }
        } else {
            for (size_t i = 0; i < size; ++i) {
                val[i] = static_cast<char>('A' + ((key_id + i) % 26));
            }
        }
        return val;
    }

    // Initialize all keys in [0, total_keys)
    void PopulateAll() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (uint64_t i = 0; i < total_keys_; ++i) {
            is_alive_[i] = true;
            affected_live_flags_[i] = 0;
            key_version_[i] = 1;
        }
        tombstones_.clear();
    }

    // Apply Put operation
    void ApplyPut(uint64_t key_id) {
        if (key_id >= total_keys_) return;
        std::unique_lock<std::shared_mutex> lock(mutex_);
        is_alive_[key_id] = true;
        key_version_[key_id]++;
    }

    // Apply DeleteRange [begin, end) left-closed right-open
    void ApplyDeleteRange(uint64_t begin_id, uint64_t end_id, uint64_t op_seq) {
        if (begin_id >= end_id) return;
        if (begin_id >= total_keys_) return;
        uint64_t actual_end = std::min(end_id, total_keys_);

        const uint64_t kDelta = 100;
        uint64_t mark_start = (begin_id > kDelta) ? (begin_id - kDelta) : 0;
        uint64_t mark_end = std::min(actual_end + kDelta, total_keys_);

        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (uint64_t i = begin_id; i < actual_end; ++i) {
            is_alive_[i] = false;
        }
        for (uint64_t i = mark_start; i < mark_end; ++i) {
            affected_live_flags_[i] = 1;
        }
        tombstones_.push_back({begin_id, actual_end, op_seq});
    }

    // Classify Get request into 3 independent categories in O(1) time
    GetCategory ClassifyGet(uint64_t key_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (key_id >= total_keys_) return GET_DELETED;

        if (!is_alive_[key_id]) {
            return GET_DELETED;
        }

        if (affected_live_flags_[key_id]) {
            return GET_AFFECTED_LIVE;
        }

        return GET_CONTROL; // Clean live key far from range tombstones
    }

    // Verify Get correctness against reference model
    bool VerifyGet(uint64_t key_id, const rocksdb::Status& status, const std::string& /*value*/, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (key_id >= total_keys_) {
            if (!status.IsNotFound()) {
                err_msg = "Expected NotFound for key beyond total_keys (" + std::to_string(key_id) + "), got status: " + status.ToString();
                return false;
            }
            return true;
        }

        bool expected_alive = is_alive_[key_id];
        if (expected_alive) {
            if (!status.ok()) {
                err_msg = "Expected OK for live key " + std::to_string(key_id) + ", got status: " + status.ToString();
                return false;
            }
        } else {
            if (!status.IsNotFound()) {
                err_msg = "Expected NotFound for deleted key " + std::to_string(key_id) + ", got status: " + status.ToString();
                return false;
            }
        }
        return true;
    }

    // Verify RangeScan correctness against reference model
    bool VerifyRangeScan(uint64_t begin_id, uint64_t end_id,
                         const std::vector<std::pair<uint64_t, std::string>>& returned_kvs,
                         size_t& expected_count, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t actual_end = std::min(end_id, total_keys_);

        // Count expected live keys in [begin_id, actual_end)
        std::vector<uint64_t> expected_keys;
        for (uint64_t i = begin_id; i < actual_end; ++i) {
            if (is_alive_[i]) {
                expected_keys.push_back(i);
            }
        }
        expected_count = expected_keys.size();

        if (returned_kvs.size() != expected_count) {
            err_msg = "RangeScan [" + std::to_string(begin_id) + ", " + std::to_string(end_id) +
                      ") returned count mismatch: expected " + std::to_string(expected_count) +
                      ", actual returned " + std::to_string(returned_kvs.size());
            return false;
        }

        // Verify keys order and presence
        for (size_t i = 0; i < returned_kvs.size(); ++i) {
            uint64_t k = returned_kvs[i].first;
            if (k != expected_keys[i]) {
                err_msg = "RangeScan key mismatch at index " + std::to_string(i) +
                          ": expected key " + std::to_string(expected_keys[i]) +
                          ", got " + std::to_string(k);
                return false;
            }
            if (k < begin_id || k >= end_id) {
                err_msg = "RangeScan returned key out of range: key=" + std::to_string(k) +
                          " not in [" + std::to_string(begin_id) + ", " + std::to_string(end_id) + ")";
                return false;
            }
            if (i > 0 && returned_kvs[i].first <= returned_kvs[i - 1].first) {
                err_msg = "RangeScan keys not strictly monotonically increasing: " +
                          std::to_string(returned_kvs[i - 1].first) + " >= " + std::to_string(returned_kvs[i].first);
                return false;
            }
        }

        return true;
    }

    // Post-run random sample verification
    bool SampleVerification(rocksdb::DB* db, size_t sample_count, size_t& verified_ok, size_t& verified_notfound, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (total_keys_ == 0) return true;

        std::mt19937_64 rng(1234567);
        std::uniform_int_distribution<uint64_t> dist(0, total_keys_ - 1);

        verified_ok = 0;
        verified_notfound = 0;
        size_t count = std::min(sample_count, static_cast<size_t>(total_keys_));

        for (size_t s = 0; s < count; ++s) {
            uint64_t k = dist(rng);
            std::string key_str = FormatKey(k);
            std::string val_str;
            rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key_str, &val_str);

            bool expected = is_alive_[k];
            if (expected) {
                if (!status.ok()) {
                    err_msg = "Sample Check Failed: key " + key_str + " (" + std::to_string(k) + ") expected OK, got " + status.ToString();
                    return false;
                }
                verified_ok++;
            } else {
                if (!status.IsNotFound()) {
                    err_msg = "Sample Check Failed: key " + key_str + " (" + std::to_string(k) + ") expected NotFound, got " + status.ToString();
                    return false;
                }
                verified_notfound++;
            }
        }
        return true;
    }

    // Post-run full iterator scan vs reference model
    bool FullScanVerification(rocksdb::DB* db, uint64_t& db_live_keys, uint64_t& ref_live_keys,
                              uint64_t& db_data_bytes, uint64_t& ref_data_bytes, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        ref_live_keys = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (is_alive_[i]) {
                ref_live_keys++;
            }
        }
        ref_data_bytes = ref_live_keys * (16 + val_size_);

        db_live_keys = 0;
        db_data_bytes = 0;

        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
        it->SeekToFirst();
        uint64_t prev_k = 0;
        bool first = true;

        while (it->Valid()) {
            std::string k_str = it->key().ToString();
            std::string v_str = it->value().ToString();
            uint64_t k_id = 0;
            if (!ParseKey(k_str, k_id)) {
                err_msg = "Full Scan Encountered Invalid Key format: " + k_str;
                return false;
            }

            if (!first && k_id <= prev_k) {
                err_msg = "Full Scan keys not strictly increasing: prev=" + std::to_string(prev_k) + ", cur=" + std::to_string(k_id);
                return false;
            }
            first = false;
            prev_k = k_id;

            if (k_id >= total_keys_ || !is_alive_[k_id]) {
                err_msg = "Full Scan Found Deleted/Ghost Key in DB: " + k_str + " (key_id=" + std::to_string(k_id) + ")";
                return false;
            }

            db_live_keys++;
            db_data_bytes += (k_str.size() + v_str.size());
            it->Next();
        }

        if (!it->status().ok()) {
            err_msg = "Full Scan Iterator error: " + it->status().ToString();
            return false;
        }

        if (db_live_keys != ref_live_keys) {
            err_msg = "Full Scan Live Key Count Mismatch: DB has " + std::to_string(db_live_keys) +
                      ", RefModel has " + std::to_string(ref_live_keys);
            return false;
        }

        return true;
    }

    // Tombstone metrics calculation
    void GetTombstoneStats(uint64_t& total_tombstones, uint64_t& union_deleted_keys,
                           double& union_coverage_ratio, double& overlap_factor) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        total_tombstones = tombstones_.size();
        if (total_tombstones == 0 || total_keys_ == 0) {
            union_deleted_keys = 0;
            union_coverage_ratio = 0.0;
            overlap_factor = 0.0;
            return;
        }

        std::vector<std::pair<uint64_t, uint64_t>> intervals;
        uint64_t sum_lengths = 0;
        for (const auto& ts : tombstones_) {
            intervals.push_back({ts.begin, ts.end});
            sum_lengths += (ts.end > ts.begin ? (ts.end - ts.begin) : 0);
        }

        std::sort(intervals.begin(), intervals.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (const auto& iv : intervals) {
            if (merged.empty() || merged.back().second < iv.first) {
                merged.push_back(iv);
            } else {
                merged.back().second = std::max(merged.back().second, iv.second);
            }
        }

        union_deleted_keys = 0;
        for (const auto& m : merged) {
            union_deleted_keys += (m.second - m.first);
        }

        union_coverage_ratio = static_cast<double>(union_deleted_keys) / total_keys_;
        overlap_factor = (union_deleted_keys > 0) ? (static_cast<double>(sum_lengths) / union_deleted_keys) : 1.0;
    }

    uint64_t GetLiveKeyCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (is_alive_[i]) cnt++;
        }
        return cnt;
    }

private:
    uint64_t total_keys_;
    size_t val_size_;
    std::vector<bool> is_alive_;
    std::vector<uint8_t> affected_live_flags_;
    std::vector<uint32_t> key_version_;
    std::vector<TombstoneInterval> tombstones_;
    mutable std::shared_mutex mutex_;
};

} // namespace study
