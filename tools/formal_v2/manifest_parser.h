#pragma once

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <openssl/sha.h>
#include <iomanip>

#include "json_parser.h"

namespace study::formal {

struct PayloadManifestInfo {
    std::string filename;
    int worker_id = 0;
    int phase_id = 0;
    std::string phase_name;
    uint64_t ops_count = 0;
    std::string sha256_hex;
};

struct TraceManifest {
    std::string magic;
    std::string version;
    std::string endianness;
    std::string generator_commit;
    std::string workload_id;
    std::string desc;
    uint64_t total_keys = 0;
    int num_workers = 0;
    size_t value_size = 0;
    std::string scan_mode;
    uint64_t total_ops_count = 0;
    uint64_t cross_worker_ops = 0;

    std::map<std::string, PayloadManifestInfo> payload_files;

    static std::string ComputeFileSha256(const std::filesystem::path& file_path) {
        std::ifstream fin(file_path, std::ios::binary);
        if (!fin.is_open()) return "";

        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        char buffer[65536];
        while (fin.read(buffer, sizeof(buffer))) {
            SHA256_Update(&ctx, buffer, fin.gcount());
        }
        if (fin.gcount() > 0) {
            SHA256_Update(&ctx, buffer, fin.gcount());
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &ctx);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    static bool ParseAndValidate(
        const std::string& trace_dir, 
        uint64_t expected_total_keys, 
        int expected_num_workers,
        size_t expected_value_size,
        TraceManifest& out_manifest,
        std::string& out_error) 
    {
        std::filesystem::path manifest_path = std::filesystem::path(trace_dir) / "manifest.json";
        if (!std::filesystem::exists(manifest_path)) {
            out_error = "manifest.json does not exist in trace directory: " + trace_dir;
            return false;
        }

        std::ifstream fin(manifest_path);
        if (!fin.is_open()) {
            out_error = "Failed opening manifest.json: " + manifest_path.string();
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());

        JsonValue root;
        std::string json_err;
        if (!JsonParser::Parse(content, root, json_err)) {
            out_error = "Strict JSON Parse Error in manifest.json: " + json_err;
            return false;
        }

        out_manifest.magic = root["magic"].AsString();
        out_manifest.version = root["version"].AsString();
        out_manifest.endianness = root["endianness"].AsString();
        out_manifest.generator_commit = root["generator_git_commit"].AsString();
        out_manifest.workload_id = root["workload_id"].AsString();
        out_manifest.desc = root["description"].AsString();
        out_manifest.scan_mode = root["scan_mode"].AsString();

        out_manifest.total_keys = root["total_keys"].AsUInt64();
        out_manifest.num_workers = static_cast<int>(root["num_workers"].AsInt64());
        out_manifest.value_size = static_cast<size_t>(root["value_size"].AsUInt64());
        out_manifest.total_ops_count = root["actual_metrics"]["total_ops_count"].AsUInt64();
        out_manifest.cross_worker_ops = root["actual_metrics"]["cross_worker_ops_count"].AsUInt64();

        // 1. Validate Core Manifest Attributes
        if (out_manifest.magic != "0x54524143455632") {
            out_error = "Manifest magic mismatch: expected '0x54524143455632', got '" + out_manifest.magic + "'";
            return false;
        }
        if (out_manifest.version != "2.0") {
            out_error = "Manifest version mismatch: expected '2.0', got '" + out_manifest.version + "'";
            return false;
        }
        if (out_manifest.endianness != "little") {
            out_error = "Manifest endianness mismatch: expected 'little', got '" + out_manifest.endianness + "'";
            return false;
        }
        if (out_manifest.generator_commit.empty()) {
            out_error = "Manifest generator_git_commit is missing or empty";
            return false;
        }
        if (out_manifest.total_keys != expected_total_keys) {
            out_error = "Manifest total_keys (" + std::to_string(out_manifest.total_keys) + 
                        ") != driver config (" + std::to_string(expected_total_keys) + ")";
            return false;
        }
        if (out_manifest.num_workers != expected_num_workers) {
            out_error = "Manifest num_workers (" + std::to_string(out_manifest.num_workers) + 
                        ") != driver config (" + std::to_string(expected_num_workers) + ")";
            return false;
        }
        if (out_manifest.value_size != expected_value_size) {
            out_error = "Manifest value_size (" + std::to_string(out_manifest.value_size) + 
                        ") != driver config (" + std::to_string(expected_value_size) + ")";
            return false;
        }
        if (out_manifest.cross_worker_ops != 0) {
            out_error = "Manifest contains non-zero cross_worker_ops: " + std::to_string(out_manifest.cross_worker_ops);
            return false;
        }

        // 2. Validate all 24 Payload Files and Sum Total Operations
        const auto& payloads_obj = root["payload_files"];
        if (!payloads_obj.IsObject()) {
            out_error = "Manifest missing 'payload_files' object";
            return false;
        }

        uint64_t calculated_sum_ops = 0;
        std::vector<std::string> p_names = {"phase_a", "phase_b", "phase_c"};

        for (int p = 0; p < 3; ++p) {
            for (int w = 0; w < expected_num_workers; ++w) {
                char fname[64];
                snprintf(fname, sizeof(fname), "%s-worker-%02d.bin", p_names[p].c_str(), w);
                std::string fname_str(fname);

                if (!payloads_obj.Has(fname_str)) {
                    out_error = "Manifest missing payload declaration for: " + fname_str;
                    return false;
                }

                const auto& p_entry = payloads_obj[fname_str];
                std::string expected_sha = p_entry["sha256"].AsString();
                uint64_t expected_ops = p_entry["ops_count"].AsUInt64();
                calculated_sum_ops += expected_ops;

                std::filesystem::path fpath = std::filesystem::path(trace_dir) / fname_str;
                if (!std::filesystem::exists(fpath)) {
                    out_error = "Payload file declared in manifest does not exist on disk: " + fpath.string();
                    return false;
                }

                auto disk_size = std::filesystem::file_size(fpath);
                if (disk_size != expected_ops * 24) {
                    out_error = "File size mismatch for " + fname_str + ": disk=" + 
                                std::to_string(disk_size) + " != expected " + std::to_string(expected_ops * 24);
                    return false;
                }

                std::string actual_sha = ComputeFileSha256(fpath);
                if (actual_sha != expected_sha) {
                    out_error = "SHA-256 verification failed for " + fname_str + ": disk=" + 
                                actual_sha + " != manifest=" + expected_sha;
                    return false;
                }

                PayloadManifestInfo p_info;
                p_info.filename = fname_str;
                p_info.worker_id = w;
                p_info.phase_id = p;
                p_info.phase_name = p_names[p];
                p_info.ops_count = expected_ops;
                p_info.sha256_hex = actual_sha;

                out_manifest.payload_files[fname_str] = p_info;
            }
        }

        // 3. Validate Total Ops Quota Closure
        if (out_manifest.total_ops_count != calculated_sum_ops) {
            out_error = "Manifest total_ops_count (" + std::to_string(out_manifest.total_ops_count) + 
                        ") does not equal sum of 24 payloads (" + std::to_string(calculated_sum_ops) + ")";
            return false;
        }

        return true;
    }
};

} // namespace study::formal
