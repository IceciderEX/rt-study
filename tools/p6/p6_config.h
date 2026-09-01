#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace study::p6 {

struct P6Config {
    std::string exp_id = "p6_d0_clean_rep1";
    std::string group_name = "D0"; // "D0", "D1", "D2"
    std::string desc = "P6 Clean Final State Baseline";
    std::string db_path = "./run-db/p6-dynamic-origin/db_test";
    std::string result_dir = "./results/raw/p6-dynamic-origin/test";
    std::string summary_csv = "./results/summary/p6-dynamic-origin/all-runs.csv";
    std::string timeseries_csv = "./results/summary/p6-dynamic-origin/timeseries.csv";

    std::string deleterange_trace_path = "./traces/p6/deleterange_trace.bin";
    std::string frontend_trace_path = "./traces/p6/frontend_ops_trace.bin";

    uint64_t total_keys = 1000000;
    size_t value_size = 1024; // 1 KiB
    std::string preload_mode = "d0_clean"; // "d0_clean", "d1_static", "d2_dynamic"

    int num_threads = 8;
    int warmup_seconds = 10;
    int main_seconds = 60;
    int cooldown_seconds = 10;
    uint64_t random_seed = 60001;

    // RocksDB Engine Tuning (Fixed baseline matching S1)
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
            else if (key == "group_name") group_name = val;
            else if (key == "desc") desc = val;
            else if (key == "db_path") db_path = val;
            else if (key == "result_dir") result_dir = val;
            else if (key == "summary_csv") summary_csv = val;
            else if (key == "timeseries_csv") timeseries_csv = val;
            else if (key == "deleterange_trace_path") deleterange_trace_path = val;
            else if (key == "frontend_trace_path") frontend_trace_path = val;
            else if (key == "total_keys") total_keys = std::stoull(val);
            else if (key == "value_size") value_size = std::stoul(val);
            else if (key == "preload_mode") preload_mode = val;
            else if (key == "num_threads") num_threads = std::stoi(val);
            else if (key == "warmup_seconds") warmup_seconds = std::stoi(val);
            else if (key == "main_seconds") main_seconds = std::stoi(val);
            else if (key == "cooldown_seconds") cooldown_seconds = std::stoi(val);
            else if (key == "random_seed") random_seed = std::stoull(val);
            else if (key == "block_cache_size") block_cache_size = std::stoul(val);
            else if (key == "write_buffer_size") write_buffer_size = std::stoul(val);
            else if (key == "max_background_jobs") max_background_jobs = std::stoi(val);
        }
        return true;
    }
};

} // namespace study::p6
