#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>

namespace study::supp {

struct SuppConfig {
    std::string exp_id = "supp_test";
    std::string desc = "Supplement Test";
    std::string db_path = "./run-db/supplement/db_test";
    std::string result_dir = "./results/raw/supplement/test";
    std::string summary_csv = "./results/summary/supplement/summary.csv";
    std::string timeseries_csv = "";

    // Key space & Dataset
    uint64_t total_keys = 1000000;
    size_t value_size = 1024; // 1 KiB
    std::string preload_mode = "clean"; // "clean", "tombstone_seg_20", "tombstone_seg_200", "tombstone_seg_2000"

    // Concurrency & Ops
    int num_threads = 8;
    uint64_t total_ops = 200000;
    int duration_seconds = 0; // 0 = bound by total_ops
    uint64_t random_seed = 10001;

    // Operation ratios (%)
    double put_ratio = 0.0;
    double get_ratio = 95.0;
    double scan_ratio = 5.0;
    double delete_range_ratio = 0.0;

    // Workload Distribution
    std::string key_distribution = "uniform"; // "uniform" or "zipfian"
    double zipf_theta = 0.99;
    uint64_t scan_len = 100;

    // Compaction & Reclamation in Measurement Phase
    bool trigger_reclaim = false;
    uint64_t reclaim_trigger_second = 20;
    uint64_t reclaim_begin_key = 0;
    uint64_t reclaim_end_key = 200000;

    // RocksDB Engine Tuning (Fixed baseline)
    size_t write_buffer_size = 64 * 1024 * 1024; // 64 MB
    int max_write_buffer_number = 4;
    int level0_file_num_compaction_trigger = 4;
    int level0_slowdown_writes_trigger = 8;
    int level0_stop_writes_trigger = 12;
    size_t block_cache_size = 128 * 1024 * 1024; // 128 MB (working set 1GB >> cache 128MB)
    size_t target_file_size_base = 64 * 1024 * 1024;
    uint64_t max_bytes_for_level_base = 256 * 1024 * 1024;
    int max_background_jobs = 8;

    bool ParseIni(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << filepath << std::endl;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string key = line.substr(0, eq_pos);
            std::string val = line.substr(eq_pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            if (key == "exp_id") exp_id = val;
            else if (key == "desc") desc = val;
            else if (key == "db_path") db_path = val;
            else if (key == "result_dir") result_dir = val;
            else if (key == "summary_csv") summary_csv = val;
            else if (key == "timeseries_csv") timeseries_csv = val;
            else if (key == "total_keys") total_keys = std::stoull(val);
            else if (key == "value_size") value_size = std::stoul(val);
            else if (key == "preload_mode") preload_mode = val;
            else if (key == "num_threads") num_threads = std::stoi(val);
            else if (key == "total_ops") total_ops = std::stoull(val);
            else if (key == "duration_seconds") duration_seconds = std::stoi(val);
            else if (key == "random_seed") random_seed = std::stoull(val);
            else if (key == "put_ratio") put_ratio = std::stod(val);
            else if (key == "get_ratio") get_ratio = std::stod(val);
            else if (key == "scan_ratio") scan_ratio = std::stod(val);
            else if (key == "delete_range_ratio") delete_range_ratio = std::stod(val);
            else if (key == "key_distribution") key_distribution = val;
            else if (key == "zipf_theta") zipf_theta = std::stod(val);
            else if (key == "scan_len") scan_len = std::stoull(val);
            else if (key == "trigger_reclaim") trigger_reclaim = (val == "true" || val == "1");
            else if (key == "reclaim_trigger_second") reclaim_trigger_second = std::stoull(val);
            else if (key == "reclaim_begin_key") reclaim_begin_key = std::stoull(val);
            else if (key == "reclaim_end_key") reclaim_end_key = std::stoull(val);
            else if (key == "block_cache_size") block_cache_size = std::stoul(val);
            else if (key == "write_buffer_size") write_buffer_size = std::stoul(val);
            else if (key == "max_background_jobs") max_background_jobs = std::stoi(val);
        }
        return true;
    }
};

} // namespace study::supp
