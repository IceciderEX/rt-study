#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <memory>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/iostats_context.h"

int main(int argc, char** argv) {
    std::string mode = (argc > 1) ? argv[1] : "t0";
    uint32_t threshold = (mode == "t256") ? 256 : 0;
    std::string db_path = "/home/wam/grad/s14-range-delete-study/run-db/thesis_validation/db_perf_" + mode;

    std::filesystem::remove_all(db_path);
    std::filesystem::create_directories(db_path);

    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kNoCompression;
    options.write_buffer_size = 64 * 1024 * 1024;
    options.memtable_max_range_deletions = threshold;

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return 1;
    }

    // Preload 100k keys
    rocksdb::WriteOptions wo;
    std::string val(256, 'x');
    for (uint64_t i = 0; i < 100000; ++i) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "%016lu", i);
        db->Put(wo, kbuf, val);
    }
    // Flush preload to SST
    db->Flush(rocksdb::FlushOptions());

    // Inject 5000 RangeDeletes
    for (uint64_t i = 0; i < 5000; ++i) {
        uint64_t start = i * 20;
        uint64_t end = start + 20;
        char k1[32], k2[32];
        snprintf(k1, sizeof(k1), "%016lu", start);
        snprintf(k2, sizeof(k2), "%016lu", end);
        db->DeleteRange(wo, db->DefaultColumnFamily(), k1, k2);
    }

    // Enable PerfContext & IOStatsContext
    rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeAndCPUTimeExceptForMutex);
    rocksdb::get_perf_context()->Reset();
    rocksdb::get_iostats_context()->Reset();

    auto t0 = std::chrono::high_resolution_clock::now();

    // Perform 10,000 Scans
    rocksdb::ReadOptions ro;
    uint64_t total_keys_found = 0;
    for (uint64_t i = 0; i < 10000; ++i) {
        uint64_t start = (i * 97) % 99000;
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "%016lu", start);
        auto it = db->NewIterator(ro);
        it->Seek(kbuf);
        int count = 0;
        while (it->Valid() && count < 100) {
            total_keys_found++;
            count++;
            it->Next();
        }
        delete it;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "=========================================================\n";
    std::cout << "Perf Profiling Result for Mode: " << mode << " (T=" << threshold << ")\n";
    std::cout << "=========================================================\n";
    std::cout << "10,000 Scans Total Time: " << elapsed_ms << " ms\n";
    std::cout << "Avg Latency per Scan: " << (elapsed_ms * 100.0) << " us\n";
    std::cout << "Total Keys Returned: " << total_keys_found << "\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "RocksDB PerfContext Breakdown:\n";
    std::cout << "  user_key_comparison_count: " << rocksdb::get_perf_context()->user_key_comparison_count << "\n";
    std::cout << "  get_from_memtable_time:    " << (rocksdb::get_perf_context()->get_from_memtable_time / 1000.0) << " us\n";
    std::cout << "  seek_internal_seek_time:   " << (rocksdb::get_perf_context()->seek_internal_seek_time / 1000.0) << " us\n";
    std::cout << "  seek_child_seek_time:      " << (rocksdb::get_perf_context()->seek_child_seek_time / 1000.0) << " us\n";
    std::cout << "  seek_min_heap_time:        " << (rocksdb::get_perf_context()->seek_min_heap_time / 1000.0) << " us\n";
    std::cout << "  block_read_time:           " << (rocksdb::get_perf_context()->block_read_time / 1000.0) << " us\n";
    std::cout << "  block_read_count:          " << rocksdb::get_perf_context()->block_read_count << "\n";
    std::cout << "  block_read_byte:           " << rocksdb::get_perf_context()->block_read_byte << " B\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "RocksDB IOStatsContext Breakdown:\n";
    std::cout << "  bytes_read:                " << rocksdb::get_iostats_context()->bytes_read << " B\n";
    std::cout << "  bytes_written:             " << rocksdb::get_iostats_context()->bytes_written << " B\n";
    std::cout << "  open_nanos:                " << (rocksdb::get_iostats_context()->open_nanos / 1000.0) << " us\n";
    std::cout << "  read_nanos:                " << (rocksdb::get_iostats_context()->read_nanos / 1000.0) << " us\n";
    std::cout << "  write_nanos:               " << (rocksdb::get_iostats_context()->write_nanos / 1000.0) << " us\n";
    std::cout << "=========================================================\n";

    db.reset();
    std::filesystem::remove_all(db_path);
    return 0;
}
