#pragma once

#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "worker_state_model.h"

namespace study::formal {

struct VerificationReport {
    bool is_pass = false;
    uint64_t db_live_keys = 0;
    uint64_t model_live_keys = 0;
    uint64_t db_payload_bytes = 0;
    uint64_t model_payload_bytes = 0;
    std::string db_sha256_hex;
    std::string model_sha256_hex;
    std::string error_detail;
};

class KvVerifier {
public:
    static VerificationReport VerifyFullDatabase(
        rocksdb::DB* db,
        const std::vector<std::unique_ptr<WorkerStateModel>>& worker_models,
        uint64_t total_keys,
        size_t value_size) 
    {
        VerificationReport report;
        report.is_pass = false;

        // 1. Compute expected live keys & expected SHA-256 directly per worker partition
        SHA256_CTX model_sha_ctx;
        SHA256_Init(&model_sha_ctx);

        for (size_t w = 0; w < worker_models.size(); ++w) {
            uint64_t start_k = worker_models[w]->GetStartKey();
            uint64_t end_k = worker_models[w]->GetEndKey();

            for (uint64_t k = start_k; k < end_k; ++k) {
                if (worker_models[w]->IsKeyLive(k)) {
                    report.model_live_keys++;
                    std::string k_str = WorkerStateModel::FormatKey(k);
                    uint32_t v_ver = worker_models[w]->GetKeyVersion(k);
                    std::string v_str = WorkerStateModel::GenerateValue(k, v_ver, value_size);

                    uint32_t k_len = static_cast<uint32_t>(k_str.size());
                    uint32_t v_len = static_cast<uint32_t>(v_str.size());
                    report.model_payload_bytes += (k_len + v_len);

                    SHA256_Update(&model_sha_ctx, &k_len, sizeof(k_len));
                    SHA256_Update(&model_sha_ctx, k_str.data(), k_len);
                    SHA256_Update(&model_sha_ctx, &v_len, sizeof(v_len));
                    SHA256_Update(&model_sha_ctx, v_str.data(), v_len);
                }
            }
        }

        unsigned char model_hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(model_hash, &model_sha_ctx);
        char model_hex[65];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            sprintf(model_hex + (i * 2), "%02x", model_hash[i]);
        }
        model_hex[64] = '\0';
        report.model_sha256_hex = std::string(model_hex);

        // Precompute partition boundaries for fast binary search
        std::vector<uint64_t> partition_starts;
        partition_starts.reserve(worker_models.size());
        for (const auto& wm : worker_models) {
            partition_starts.push_back(wm->GetStartKey());
        }

        auto find_worker = [&](uint64_t k) -> int {
            auto it = std::upper_bound(partition_starts.begin(), partition_starts.end(), k);
            if (it == partition_starts.begin()) return -1;
            int idx = static_cast<int>(std::distance(partition_starts.begin(), it) - 1);
            if (k >= worker_models[idx]->GetStartKey() && k < worker_models[idx]->GetEndKey()) {
                return idx;
            }
            return -1;
        };

        // 2. Sequential iterator scan over actual RocksDB instance
        rocksdb::ReadOptions read_opts;
        read_opts.fill_cache = false; // Prevent cache pollution
        read_opts.total_order_seek = true;

        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_opts));
        SHA256_CTX db_sha_ctx;
        SHA256_Init(&db_sha_ctx);

        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            rocksdb::Slice k_slice = it->key();
            rocksdb::Slice v_slice = it->value();

            std::string k_str = k_slice.ToString();
            uint64_t k = WorkerStateModel::ParseKey(k_str);
            if (k == UINT64_MAX || k >= total_keys) {
                report.error_detail = "Unrecognized or out-of-range key format in DB: " + k_str;
                return report;
            }

            int w_idx = find_worker(k);
            if (w_idx < 0 || !worker_models[w_idx]->IsKeyLive(k)) {
                report.error_detail = "Ghost key found in DB that model marked as deleted: " + k_str;
                return report;
            }

            uint32_t expected_ver = worker_models[w_idx]->GetKeyVersion(k);
            std::string expected_val = WorkerStateModel::GenerateValue(k, expected_ver, value_size);
            if (v_slice.ToString() != expected_val) {
                report.error_detail = "Value content mismatch for key " + k_str + ": expected version " +
                                      std::to_string(expected_ver) + ", but got mismatched bytes.";
                return report;
            }

            report.db_live_keys++;
            uint32_t k_len = static_cast<uint32_t>(k_slice.size());
            uint32_t v_len = static_cast<uint32_t>(v_slice.size());
            report.db_payload_bytes += (k_len + v_len);

            SHA256_Update(&db_sha_ctx, &k_len, sizeof(k_len));
            SHA256_Update(&db_sha_ctx, k_slice.data(), k_len);
            SHA256_Update(&db_sha_ctx, &v_len, sizeof(v_len));
            SHA256_Update(&db_sha_ctx, v_slice.data(), v_len);
        }

        if (!it->status().ok()) {
            report.error_detail = "Iterator status error during full scan: " + it->status().ToString();
            return report;
        }

        unsigned char db_hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(db_hash, &db_sha_ctx);
        char db_hex[65];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            sprintf(db_hex + (i * 2), "%02x", db_hash[i]);
        }
        db_hex[64] = '\0';
        report.db_sha256_hex = std::string(db_hex);

        // 3. Bit-for-bit Equality Checks
        if (report.db_live_keys != report.model_live_keys) {
            report.error_detail = "Live key count mismatch: DB=" + std::to_string(report.db_live_keys) + 
                                  " != Model=" + std::to_string(report.model_live_keys);
            return report;
        }

        if (report.db_payload_bytes != report.model_payload_bytes) {
            report.error_detail = "Payload bytes mismatch: DB=" + std::to_string(report.db_payload_bytes) + 
                                  " != Model=" + std::to_string(report.model_payload_bytes);
            return report;
        }

        if (report.db_sha256_hex != report.model_sha256_hex) {
            report.error_detail = "SHA-256 checksum mismatch: DB=" + report.db_sha256_hex + 
                                  " != Model=" + report.model_sha256_hex;
            return report;
        }

        report.is_pass = true;
        return report;
    }
};

} // namespace study::formal
