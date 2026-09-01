#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdint>

namespace study {

struct BenchmarkConfig {
    // Database and path configs
    std::string db_path = "/home/wam/grad/s14-range-delete-study/run-db/smoke-test-db";
    std::string result_dir = "/home/wam/grad/s14-range-delete-study/results/raw/smoke-test";
    std::string summary_csv = "/home/wam/grad/s14-range-delete-study/results/summary/smoke-test.csv";
    std::string exp_id = "smoke-test";
    std::string desc = "Smoke Test Run";

    // Dataset configs
    uint64_t total_keys = 50000;
    size_t value_size = 128;
    bool populate_db = true;

    // Concurrency & duration
    uint32_t num_threads = 4;
    uint64_t total_ops = 20000;
    uint32_t duration_seconds = 0; // 0 = run by total_ops
    uint64_t random_seed = 42;

    // Operation ratios (in percentage, sum to 100)
    double put_ratio = 5.0;
    double get_ratio = 70.0;
    double scan_ratio = 23.0;
    double delete_range_ratio = 2.0;

    // Distribution
    std::string key_distribution = "uniform"; // "uniform" or "zipfian"
    double zipf_theta = 0.99;

    // Range deletion parameters
    uint64_t delete_range_len = 200;
    double range_overlap_ratio = 0.0; // 0.0 = discrete, 0.8 = high overlap
    double hotspot_overlap_ratio = 0.0; // 0.0 = cold, 0.5 = medium, 1.0 = hot
    std::string tombstone_pattern = "uniform"; // "short-fragmented", "long-contiguous", etc.

    // Range scan parameters
    uint64_t scan_len = 100;

    // Correctness checking
    bool enable_online_verification = true;
    size_t post_run_sample_keys = 10000;
    bool enable_full_scan_verification = true;

    // RocksDB Engine Options
    size_t write_buffer_size = 64 * 1024 * 1024; // 64MB
    int max_write_buffer_number = 3;
    int level0_file_num_compaction_trigger = 4;
    int level0_slowdown_writes_trigger = 8;
    int level0_stop_writes_trigger = 12;
    size_t block_cache_size = 128 * 1024 * 1024; // 128MB
    bool disable_wal = false;
    bool sync_writes = false;
    size_t target_file_size_base = 64 * 1024 * 1024;
    uint64_t max_bytes_for_level_base = 256 * 1024 * 1024;
    double max_bytes_for_level_multiplier = 10;
    int max_background_jobs = 4;

    // P5 Controlled Compaction
    bool trigger_compact_range = false;
    uint64_t compact_range_begin = 0;
    uint64_t compact_range_end = 0;
    uint32_t compact_trigger_op_percent = 50; // Trigger at 50% ops
};

class ConfigParser {
public:
    static bool ParseCommandLine(int argc, char** argv, BenchmarkConfig& config) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) {
                if (!LoadFromFile(argv[++i], config)) return false;
            } else if (arg == "--db_path" && i + 1 < argc) {
                config.db_path = argv[++i];
            } else if (arg == "--result_dir" && i + 1 < argc) {
                config.result_dir = argv[++i];
            } else if (arg == "--summary_csv" && i + 1 < argc) {
                config.summary_csv = argv[++i];
            } else if (arg == "--exp_id" && i + 1 < argc) {
                config.exp_id = argv[++i];
            } else if (arg == "--total_keys" && i + 1 < argc) {
                config.total_keys = std::stoull(argv[++i]);
            } else if (arg == "--value_size" && i + 1 < argc) {
                config.value_size = std::stoul(argv[++i]);
            } else if (arg == "--threads" && i + 1 < argc) {
                config.num_threads = std::stoul(argv[++i]);
            } else if (arg == "--ops" && i + 1 < argc) {
                config.total_ops = std::stoull(argv[++i]);
            } else if (arg == "--duration" && i + 1 < argc) {
                config.duration_seconds = std::stoul(argv[++i]);
            } else if (arg == "--seed" && i + 1 < argc) {
                config.random_seed = std::stoull(argv[++i]);
            } else if (arg == "--del_range_ratio" && i + 1 < argc) {
                config.delete_range_ratio = std::stod(argv[++i]);
            } else if (arg == "--get_ratio" && i + 1 < argc) {
                config.get_ratio = std::stod(argv[++i]);
            } else if (arg == "--scan_ratio" && i + 1 < argc) {
                config.scan_ratio = std::stod(argv[++i]);
            } else if (arg == "--put_ratio" && i + 1 < argc) {
                config.put_ratio = std::stod(argv[++i]);
            } else if (arg == "--del_range_len" && i + 1 < argc) {
                config.delete_range_len = std::stoull(argv[++i]);
            } else if (arg == "--scan_len" && i + 1 < argc) {
                config.scan_len = std::stoull(argv[++i]);
            } else if (arg == "--distribution" && i + 1 < argc) {
                config.key_distribution = argv[++i];
            } else if (arg == "--zipf_theta" && i + 1 < argc) {
                config.zipf_theta = std::stod(argv[++i]);
            } else if (arg == "--hotspot_overlap" && i + 1 < argc) {
                config.hotspot_overlap_ratio = std::stod(argv[++i]);
            } else if (arg == "--pattern" && i + 1 < argc) {
                config.tombstone_pattern = argv[++i];
            } else if (arg == "--help") {
                PrintUsage();
                return false;
            }
        }
        return true;
    }

    static bool LoadFromFile(const std::string& filename, BenchmarkConfig& config) {
        std::ifstream fin(filename);
        if (!fin.is_open()) {
            std::cerr << "[ConfigParser] Error opening config file: " << filename << std::endl;
            return false;
        }
        std::string line;
        while (std::getline(fin, line)) {
            // Trim and skip comments
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == '#' || line[first] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "db_path") config.db_path = val;
            else if (key == "result_dir") config.result_dir = val;
            else if (key == "summary_csv") config.summary_csv = val;
            else if (key == "exp_id") config.exp_id = val;
            else if (key == "desc") config.desc = val;
            else if (key == "total_keys") config.total_keys = std::stoull(val);
            else if (key == "value_size") config.value_size = std::stoul(val);
            else if (key == "populate_db") config.populate_db = (val == "true" || val == "1");
            else if (key == "num_threads") config.num_threads = std::stoul(val);
            else if (key == "total_ops") config.total_ops = std::stoull(val);
            else if (key == "duration_seconds") config.duration_seconds = std::stoul(val);
            else if (key == "random_seed") config.random_seed = std::stoull(val);
            else if (key == "put_ratio") config.put_ratio = std::stod(val);
            else if (key == "get_ratio") config.get_ratio = std::stod(val);
            else if (key == "scan_ratio") config.scan_ratio = std::stod(val);
            else if (key == "delete_range_ratio") config.delete_range_ratio = std::stod(val);
            else if (key == "key_distribution") config.key_distribution = val;
            else if (key == "zipf_theta") config.zipf_theta = std::stod(val);
            else if (key == "delete_range_len") config.delete_range_len = std::stoull(val);
            else if (key == "range_overlap_ratio") config.range_overlap_ratio = std::stod(val);
            else if (key == "hotspot_overlap_ratio") config.hotspot_overlap_ratio = std::stod(val);
            else if (key == "tombstone_pattern") config.tombstone_pattern = val;
            else if (key == "scan_len") config.scan_len = std::stoull(val);
            else if (key == "enable_online_verification") config.enable_online_verification = (val == "true" || val == "1");
            else if (key == "post_run_sample_keys") config.post_run_sample_keys = std::stoul(val);
            else if (key == "enable_full_scan_verification") config.enable_full_scan_verification = (val == "true" || val == "1");
            else if (key == "write_buffer_size") config.write_buffer_size = std::stoull(val);
            else if (key == "max_write_buffer_number") config.max_write_buffer_number = std::stoi(val);
            else if (key == "level0_file_num_compaction_trigger") config.level0_file_num_compaction_trigger = std::stoi(val);
            else if (key == "level0_slowdown_writes_trigger") config.level0_slowdown_writes_trigger = std::stoi(val);
            else if (key == "level0_stop_writes_trigger") config.level0_stop_writes_trigger = std::stoi(val);
            else if (key == "block_cache_size") config.block_cache_size = std::stoull(val);
            else if (key == "target_file_size_base") config.target_file_size_base = std::stoull(val);
            else if (key == "max_bytes_for_level_base") config.max_bytes_for_level_base = std::stoull(val);
            else if (key == "max_bytes_for_level_multiplier") config.max_bytes_for_level_multiplier = std::stod(val);
            else if (key == "max_background_jobs") config.max_background_jobs = std::stoi(val);
            else if (key == "trigger_compact_range") config.trigger_compact_range = (val == "true" || val == "1");
            else if (key == "compact_range_begin") config.compact_range_begin = std::stoull(val);
            else if (key == "compact_range_end") config.compact_range_end = std::stoull(val);
            else if (key == "compact_trigger_op_percent") config.compact_trigger_op_percent = std::stoul(val);
        }
        return true;
    }

    static void PrintUsage() {
        std::cout << "Usage: workload_driver [options]\n"
                  << "  --config <path>           Path to configuration file\n"
                  << "  --db_path <path>          Path to RocksDB database directory\n"
                  << "  --result_dir <path>       Directory to save raw results & logs\n"
                  << "  --summary_csv <path>      Path to summary CSV file\n"
                  << "  --exp_id <id>             Experiment identifier\n"
                  << "  --total_keys <N>          Initial key count\n"
                  << "  --value_size <bytes>      Value size in bytes\n"
                  << "  --threads <num>           Number of worker threads\n"
                  << "  --ops <num>               Total number of benchmark operations\n"
                  << "  --del_range_ratio <%>     Percentage of DeleteRange operations\n"
                  << "  --get_ratio <%>           Percentage of Get operations\n"
                  << "  --scan_ratio <%>          Percentage of RangeScan operations\n"
                  << "  --put_ratio <%>           Percentage of Put operations\n"
                  << "  --del_range_len <len>     Span of each DeleteRange operation\n"
                  << "  --scan_len <len>          Span of each RangeScan operation\n"
                  << "  --distribution <type>     Access distribution: 'uniform' or 'zipfian'\n"
                  << "  --hotspot_overlap <ratio> Overlap between hotspot and deletion (0.0 to 1.0)\n"
                  << "  --pattern <pattern>       Tombstone pattern: short-fragmented, long-contiguous, etc.\n";
    }

private:
    static std::string Trim(const std::string& s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, (last - first + 1));
    }
};

} // namespace study
