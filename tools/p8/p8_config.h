#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace study::p8 {

struct P8Config {
    std::string exp_id = "p8-default-ratio-020-r01";
    std::string group_name = "p8-default-ratio-020";
    std::string desc = "P8 Default Ratio 2.0%";
    std::string db_path = "./run-db/p8-flush-oracle/db_test";
    std::string result_dir = "./results/raw/p8-flush-oracle/test";
    std::string summary_csv = "./results/summary/p8-flush-oracle/all-runs.csv";
    std::string ts_wallclock_csv = "./results/summary/p8-flush-oracle/timeseries-wallclock.csv";
    std::string ts_progress_csv = "./results/summary/p8-flush-oracle/timeseries-progress.csv";

    std::string trace_path = "./traces/p7/p7_trace_ratio_020.bin";

    uint64_t total_keys = 500000;
    size_t value_size = 256;
    uint64_t total_ops = 200000;
    double del_range_ratio = 0.020; // 0.02, 0.05, 0.10

    bool enable_oracle_flush = false;
    uint64_t flush_del_range_milestone = 2000; // 50% of total del ranges (2000 for 2%, 5000 for 5%, 10000 for 10%)

    int num_threads = 8;
    uint64_t random_seed = 70004;

    // RocksDB Engine Tuning (Matches P7/P1)
    size_t write_buffer_size = 64 * 1024 * 1024;
    int max_write_buffer_number = 4;
    int level0_file_num_compaction_trigger = 4;
    int level0_slowdown_writes_trigger = 8;
    int level0_stop_writes_trigger = 12;
    size_t block_cache_size = 128 * 1024 * 1024;
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
            else if (key == "ts_wallclock_csv") ts_wallclock_csv = val;
            else if (key == "ts_progress_csv") ts_progress_csv = val;
            else if (key == "trace_path") trace_path = val;
            else if (key == "total_keys") total_keys = std::stoull(val);
            else if (key == "value_size") value_size = std::stoul(val);
            else if (key == "total_ops") total_ops = std::stoull(val);
            else if (key == "del_range_ratio") del_range_ratio = std::stod(val);
            else if (key == "enable_oracle_flush") enable_oracle_flush = (val == "true" || val == "1");
            else if (key == "flush_del_range_milestone") flush_del_range_milestone = std::stoull(val);
            else if (key == "num_threads") num_threads = std::stoi(val);
            else if (key == "random_seed") random_seed = std::stoull(val);
            else if (key == "block_cache_size") block_cache_size = std::stoul(val);
            else if (key == "write_buffer_size") write_buffer_size = std::stoul(val);
            else if (key == "max_background_jobs") max_background_jobs = std::stoi(val);
        }
        return true;
    }
};

} // namespace study::p8
