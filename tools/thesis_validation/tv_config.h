#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace study::tv {

struct TVConfig {
    std::string exp_id = "e2_val_0064_t000_sanity";
    std::string group_name = "e2_val_0064_t000";
    std::string desc = "E2 Value 64B Default T=0 Sanity";
    std::string db_path = "./run-db/thesis_validation/db_test";
    std::string result_dir = "./results/raw/e2/test";
    std::string summary_csv = "./results/summary/e2_all_runs.csv";
    std::string ts_wallclock_csv = "./results/summary/e2_timeseries_wallclock.csv";
    std::string ts_progress_csv = "./results/summary/e2_timeseries_progress.csv";

    std::string trace_path = "./traces/p7/p7_trace_ratio_100.bin";

    uint64_t total_keys = 500000;
    size_t value_size = 256; // 64, 256, 1024, 4096
    uint64_t total_ops = 200000;
    double del_range_ratio = 0.100;

    // Native RocksDB Range Deletion Flush Thresholds
    uint32_t memtable_max_range_deletions = 0; // 0 = disabled, 64, 128, 256, 512, 1024, 2048
    uint32_t memtable_op_scan_flush_trigger = 0;

    // Workload Ratios (if trace generated on the fly)
    double get_ratio = 0.65;
    double scan_ratio = 0.20;
    double put_ratio = 0.05;
    uint64_t scan_span = 100;
    uint64_t del_range_len = 100;

    int num_threads = 8;
    uint64_t random_seed = 70006;

    // RocksDB Engine Tuning (Matches P7/P1 standard)
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
            else if (key == "memtable_max_range_deletions") memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(val));
            else if (key == "memtable_op_scan_flush_trigger") memtable_op_scan_flush_trigger = static_cast<uint32_t>(std::stoul(val));
            else if (key == "get_ratio") get_ratio = std::stod(val);
            else if (key == "scan_ratio") scan_ratio = std::stod(val);
            else if (key == "put_ratio") put_ratio = std::stod(val);
            else if (key == "scan_span") scan_span = std::stoull(val);
            else if (key == "del_range_len") del_range_len = std::stoull(val);
            else if (key == "num_threads") num_threads = std::stoi(val);
            else if (key == "random_seed") random_seed = std::stoull(val);
            else if (key == "block_cache_size") block_cache_size = std::stoul(val);
            else if (key == "write_buffer_size") write_buffer_size = std::stoul(val);
            else if (key == "max_background_jobs") max_background_jobs = std::stoi(val);
        }
        return true;
    }
};

} // namespace study::tv
