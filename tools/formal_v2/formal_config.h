#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <filesystem>

namespace study::formal {

struct FormalConfig {
    std::string exp_id = "formal_b0_t000_rep01";
    std::string group_name = "formal_b0_t000";
    std::string desc = "Formal V2 Baseline T=0";
    std::string db_path = "./run-db/formal_v2/db_test";
    std::string result_dir = "./results/formal_v2/raw/test";
    std::string summary_csv = "./results/formal_v2/summary/formal_summary.csv";
    std::string events_csv = "./results/formal_v2/summary/formal_events.csv";
    std::string phases_csv = "./results/formal_v2/summary/formal_phases.csv";

    std::string trace_dir = "./traces/formal_v2/small_dynamic_500k_limit";

    uint64_t total_keys = 500000;
    size_t value_size = 256;
    int num_workers = 8;
    uint64_t random_seed = 90001;

    // Native RocksDB Range Deletion Flush Thresholds
    uint32_t memtable_max_range_deletions = 0; // 0 = disabled, 64, 128, 256, 512, 1024, 2048
    uint32_t memtable_op_scan_flush_trigger = 0; // Fixed 0 (DISABLED) to prevent confounding

    // Experimental Range Tombstone Controller. These options are independent
    // of the native fixed threshold above. Keep the native threshold at zero
    // when evaluating the controller so that a flush has one clear cause.
    bool enable_range_tombstone_controller = false;
    bool range_tombstone_controller_observe_only = true;
    uint32_t range_tombstone_controller_min_range_deletions = 512;
    uint64_t range_tombstone_controller_min_memtable_bytes = 8 * 1024 * 1024;
    uint64_t range_tombstone_controller_cooldown_micros = 1000000;

    // RocksDB Engine Tuning
    size_t write_buffer_size = 64 * 1024 * 1024;
    int max_write_buffer_number = 4;
    int level0_file_num_compaction_trigger = 4;
    int level0_slowdown_writes_trigger = 8;
    int level0_stop_writes_trigger = 12;
    size_t block_cache_size = 128 * 1024 * 1024;
    size_t target_file_size_base = 64 * 1024 * 1024;
    uint64_t max_bytes_for_level_base = 256 * 1024 * 1024;
    int max_background_jobs = 8;

    // Oracle Flush Control
    bool oracle_flush_after_phase_b = false;

    bool ParseIni(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "[FormalConfig ERROR] Failed to open config file: " << filepath << std::endl;
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
            else if (key == "events_csv") events_csv = val;
            else if (key == "phases_csv") phases_csv = val;
            else if (key == "trace_dir") trace_dir = val;
            else if (key == "total_keys") total_keys = std::stoull(val);
            else if (key == "value_size") value_size = std::stoul(val);
            else if (key == "num_workers") num_workers = std::stoi(val);
            else if (key == "memtable_max_range_deletions") memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(val));
            else if (key == "memtable_op_scan_flush_trigger") memtable_op_scan_flush_trigger = static_cast<uint32_t>(std::stoul(val));
            else if (key == "enable_range_tombstone_controller") enable_range_tombstone_controller = ParseBool(val);
            else if (key == "range_tombstone_controller_observe_only") range_tombstone_controller_observe_only = ParseBool(val);
            else if (key == "range_tombstone_controller_min_range_deletions") range_tombstone_controller_min_range_deletions = static_cast<uint32_t>(std::stoul(val));
            else if (key == "range_tombstone_controller_min_memtable_bytes") range_tombstone_controller_min_memtable_bytes = std::stoull(val);
            else if (key == "range_tombstone_controller_cooldown_micros") range_tombstone_controller_cooldown_micros = std::stoull(val);
            else if (key == "random_seed") random_seed = std::stoull(val);
            else if (key == "write_buffer_size") write_buffer_size = std::stoull(val);
            else if (key == "block_cache_size") block_cache_size = std::stoull(val);
            else if (key == "max_write_buffer_number") max_write_buffer_number = std::stoi(val);
            else if (key == "level0_file_num_compaction_trigger") level0_file_num_compaction_trigger = std::stoi(val);
            else if (key == "max_background_jobs") max_background_jobs = std::stoi(val);
            else if (key == "oracle_flush_after_phase_b") oracle_flush_after_phase_b = ParseBool(val);
        }
        return true;
    }

private:
    static bool ParseBool(const std::string& value) {
        return value == "true" || value == "1" || value == "yes";
    }
};

} // namespace study::formal
