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
#include <cstring>
#include <iomanip>
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/status.h"

namespace study::supp {

enum GetCategory {
    GET_AFFECTED_LIVE = 0,
    GET_DELETED = 1,
    GET_CONTROL = 2
};

struct TombstoneSegment {
    uint64_t begin;
    uint64_t end; // [begin, end)
};

class SuppReferenceModel {
public:
    SuppReferenceModel(uint64_t total_keys, size_t val_size)
        : total_keys_(total_keys), val_size_(val_size),
          is_alive_(total_keys, false),
          affected_live_flags_(total_keys, 0) {}

    static std::string FormatKey(uint64_t key_id) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "key_%012llu", static_cast<unsigned long long>(key_id));
        return std::string(buf);
    }

    static bool ParseKey(const std::string& key_str, uint64_t& key_id) {
        if (key_str.rfind("key_", 0) != 0) return false;
        try {
            key_id = std::stoull(key_str.substr(4));
            return true;
        } catch (...) {
            return false;
        }
    }

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

    // Generate deterministic disjoint deletion intervals for union U (40% coverage)
    static std::vector<TombstoneSegment> GenerateDeletionSegments(uint64_t total_keys, int num_segments) {
        std::vector<TombstoneSegment> segs;
        if (num_segments <= 0) return segs;

        uint64_t step = total_keys / num_segments;
        uint64_t del_len = static_cast<uint64_t>(step * 0.4); // exactly 40% of each step
        uint64_t offset = (step - del_len) / 2;

        for (int i = 0; i < num_segments; ++i) {
            uint64_t b = i * step + offset;
            uint64_t e = b + del_len;
            if (e > total_keys) e = total_keys;
            if (b < e) {
                segs.push_back({b, e});
            }
        }
        return segs;
    }

    // Preload database based on mode: "clean", "tombstone_seg_20", "tombstone_seg_200", "tombstone_seg_2000"
    void SetupPreloadState(const std::string& mode, std::vector<TombstoneSegment>& out_del_segs) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        is_alive_.assign(total_keys_, true);
        affected_live_flags_.assign(total_keys_, 0);
        tombstone_segments_.clear();

        int num_segs = 0;
        if (mode == "tombstone_seg_20" || mode == "tombstone_clean_ref_20") num_segs = 20;
        else if (mode == "tombstone_seg_200") num_segs = 200;
        else if (mode == "tombstone_seg_2000") num_segs = 2000;
        else if (mode == "clean") num_segs = 20; // clean baseline matches seg_20 survival set

        out_del_segs = GenerateDeletionSegments(total_keys_, num_segs);
        tombstone_segments_ = out_del_segs;

        const uint64_t kDelta = 100;

        for (const auto& seg : out_del_segs) {
            for (uint64_t i = seg.begin; i < seg.end; ++i) {
                is_alive_[i] = false;
            }
            uint64_t mark_b = (seg.begin > kDelta) ? (seg.begin - kDelta) : 0;
            uint64_t mark_e = std::min(seg.end + kDelta, total_keys_);
            for (uint64_t i = mark_b; i < mark_e; ++i) {
                affected_live_flags_[i] = 1;
            }
        }
    }

    // Classify Get in O(1) time
    GetCategory ClassifyGet(uint64_t key_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (key_id >= total_keys_) return GET_DELETED;
        if (!is_alive_[key_id]) return GET_DELETED;
        if (affected_live_flags_[key_id]) return GET_AFFECTED_LIVE;
        return GET_CONTROL;
    }

    bool IsAlive(uint64_t key_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (key_id >= total_keys_) return false;
        return is_alive_[key_id];
    }

    void ApplyPut(uint64_t key_id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (key_id < total_keys_) {
            is_alive_[key_id] = true;
        }
    }

    uint64_t GetLiveKeyCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (is_alive_[i]) cnt++;
        }
        return cnt;
    }

    // Verify RangeScan strictly
    bool VerifyRangeScan(uint64_t begin_id, uint64_t end_id,
                         const std::vector<std::pair<uint64_t, std::string>>& returned_kvs,
                         size_t& expected_count, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t actual_end = std::min(end_id, total_keys_);

        std::vector<uint64_t> expected_keys;
        for (uint64_t i = begin_id; i < actual_end; ++i) {
            if (is_alive_[i]) expected_keys.push_back(i);
        }
        expected_count = expected_keys.size();

        if (returned_kvs.size() != expected_count) {
            err_msg = "RangeScan count mismatch: expected " + std::to_string(expected_count) +
                      ", got " + std::to_string(returned_kvs.size());
            return false;
        }

        for (size_t i = 0; i < returned_kvs.size(); ++i) {
            if (returned_kvs[i].first != expected_keys[i]) {
                err_msg = "RangeScan key mismatch at index " + std::to_string(i) +
                          ": expected " + std::to_string(expected_keys[i]) +
                          ", got " + std::to_string(returned_kvs[i].first);
                return false;
            }
        }
        return true;
    }

    // Post-run random sample verification (10,000 keys)
    bool SampleVerification(rocksdb::DB* db, size_t sample_count, size_t& verified_ok, size_t& verified_notfound, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (total_keys_ == 0) return true;

        std::mt19937_64 rng(987654321);
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
            if (is_alive_[i]) ref_live_keys++;
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
                err_msg = "Full Scan Invalid Key format: " + k_str;
                return false;
            }

            if (!first && k_id <= prev_k) {
                err_msg = "Full Scan keys not strictly increasing";
                return false;
            }
            first = false;
            prev_k = k_id;

            if (k_id >= total_keys_ || !is_alive_[k_id]) {
                err_msg = "Full Scan Found Deleted Key in DB: " + k_str;
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
            err_msg = "Full Scan Count Mismatch: DB has " + std::to_string(db_live_keys) +
                      ", Ref has " + std::to_string(ref_live_keys);
            return false;
        }

        return true;
    }

    const std::vector<TombstoneSegment>& GetTombstoneSegments() const {
        return tombstone_segments_;
    }

private:
    uint64_t total_keys_;
    size_t val_size_;
    std::vector<bool> is_alive_;
    std::vector<uint8_t> affected_live_flags_;
    std::vector<TombstoneSegment> tombstone_segments_;
    mutable std::shared_mutex mutex_;
};

} // namespace study::supp
