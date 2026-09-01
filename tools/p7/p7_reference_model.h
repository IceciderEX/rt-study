#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <cstring>
#include <iomanip>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/status.h"

namespace study::p7 {

enum P7GetCategory {
    GET_AFFECTED_LIVE = 0,
    GET_DELETED = 1,
    GET_CONTROL = 2
};

struct RangeDelRecord {
    uint64_t begin;
    uint64_t end;
};

class P7ReferenceModel {
public:
    P7ReferenceModel(uint64_t total_keys, size_t val_size)
        : total_keys_(total_keys), val_size_(val_size),
          is_alive_(total_keys, true),
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

    void ApplyDeleteRange(uint64_t b, uint64_t e) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        uint64_t actual_e = std::min(e, total_keys_);
        for (uint64_t i = b; i < actual_e; ++i) {
            is_alive_[i] = false;
        }

        const uint64_t kDelta = 25;
        uint64_t mark_b = (b > kDelta) ? (b - kDelta) : 0;
        uint64_t mark_e = std::min(actual_e + kDelta, total_keys_);
        for (uint64_t i = mark_b; i < mark_e; ++i) {
            if (is_alive_[i]) {
                affected_live_flags_[i] = 1;
            }
        }
        tombstones_count_++;
    }

    P7GetCategory ClassifyGet(uint64_t key_id) const {
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

    uint64_t GetLiveKeyCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (is_alive_[i]) cnt++;
        }
        return cnt;
    }

    uint64_t GetUnionDeletedKeys() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        uint64_t cnt = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (!is_alive_[i]) cnt++;
        }
        return cnt;
    }

    double GetUnionCoverageRatio() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (total_keys_ == 0) return 0.0;
        uint64_t del_cnt = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (!is_alive_[i]) del_cnt++;
        }
        return static_cast<double>(del_cnt) / total_keys_;
    }

    uint64_t GetTombstonesCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return tombstones_count_;
    }

    bool SampleVerification(rocksdb::DB* db, size_t sample_count, size_t& verified_ok, size_t& verified_notfound, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (total_keys_ == 0) return true;

        std::mt19937_64 rng(123456789);
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
                    err_msg = "Sample Check Failed: key " + key_str + " expected OK, got " + status.ToString();
                    return false;
                }
                verified_ok++;
            } else {
                if (!status.IsNotFound()) {
                    err_msg = "Sample Check Failed: key " + key_str + " expected NotFound, got " + status.ToString();
                    return false;
                }
                verified_notfound++;
            }
        }
        return true;
    }

    bool FullScanAndComputeSha256(rocksdb::DB* db, uint64_t& db_live_keys, uint64_t& db_data_bytes,
                                  std::string& sha256_hex, std::string& err_msg) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        db_live_keys = 0;
        db_data_bytes = 0;

        SHA256_CTX sha_ctx;
        SHA256_Init(&sha_ctx);

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

            SHA256_Update(&sha_ctx, k_str.data(), k_str.size());
            SHA256_Update(&sha_ctx, v_str.data(), v_str.size());

            it->Next();
        }

        if (!it->status().ok()) {
            err_msg = "Full Scan Iterator error: " + it->status().ToString();
            return false;
        }

        uint8_t hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha_ctx);

        char hex_buf[65];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            std::snprintf(&hex_buf[i * 2], 3, "%02x", hash[i]);
        }
        sha256_hex = std::string(hex_buf);

        uint64_t expected_live = 0;
        for (uint64_t i = 0; i < total_keys_; ++i) {
            if (is_alive_[i]) expected_live++;
        }

        if (db_live_keys != expected_live) {
            err_msg = "Live keys count mismatch: expected " + std::to_string(expected_live) + ", got " + std::to_string(db_live_keys);
            return false;
        }

        return true;
    }

private:
    uint64_t total_keys_;
    size_t val_size_;
    std::vector<bool> is_alive_;
    std::vector<uint8_t> affected_live_flags_;
    uint64_t tombstones_count_{0};
    mutable std::shared_mutex mutex_;
};

} // namespace study::p7
