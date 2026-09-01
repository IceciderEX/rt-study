#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/write_batch.h"

#pragma pack(push, 1)
struct FormalTraceRecord {
    uint8_t  phase_id;      // 0=A, 1=B, 2=C
    uint8_t  op_type;       // 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=No-op
    uint8_t  scan_mode;     // 0=Range, 1=Limit
    uint8_t  flags;         // bit 0: affected/intersect, bit 1: inject, bit 2: postburst
    uint32_t op_id;
    uint64_t key1;
    uint64_t key2;
};
#pragma pack(pop)

static_assert(sizeof(FormalTraceRecord) == 24, "FormalTraceRecord must be exactly 24 bytes");

struct E8Config {
    std::string db_path;
    std::string trace_dir;
    std::string profile_wait_dir;
    int profile_window_sec = 45;
    std::string profile_case_name = "t0_scan_intersect";
    uint32_t threshold = 0;
    bool disable_auto_compactions = false;
    int num_workers = 1;
    uint64_t total_keys = 500000;
    size_t value_size = 256;
    uint64_t write_buffer_size = 64 * 1024 * 1024;
    uint64_t range_delete_count = 20000;
    bool is_clean = false;
    bool post_flush = false;
};

static uint64_t GetCurrentTimeNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t GetThreadCpuTimeNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static std::string FormatKey(uint64_t k) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lu", k);
    return std::string(buf);
}

static bool WriteAtomicJsonWithDirSync(const std::string& parent_dir, const std::string& file_name, const std::string& content) {
    std::string final_path = parent_dir + "/" + file_name;
    std::string tmp_path = final_path + ".tmp";

    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        std::cerr << "[DRIVER ERROR] open tmp file failed: " << tmp_path << " (" << strerror(errno) << ")\n";
        return false;
    }

    size_t written = 0;
    while (written < content.size()) {
        ssize_t n = write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[DRIVER ERROR] write failed: " << tmp_path << " (" << strerror(errno) << ")\n";
            close(fd);
            unlink(tmp_path.c_str());
            return false;
        }
        written += n;
    }

    if (fsync(fd) != 0) {
        std::cerr << "[DRIVER ERROR] fsync file failed: " << tmp_path << " (" << strerror(errno) << ")\n";
        close(fd);
        unlink(tmp_path.c_str());
        return false;
    }

    if (close(fd) != 0) {
        std::cerr << "[DRIVER ERROR] close file failed: " << tmp_path << " (" << strerror(errno) << ")\n";
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        std::cerr << "[DRIVER ERROR] rename failed: " << tmp_path << " -> " << final_path << " (" << strerror(errno) << ")\n";
        unlink(tmp_path.c_str());
        return false;
    }

    // Sync parent directory
    int dfd = open(parent_dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

class E8MicroDriver {
public:
    explicit E8MicroDriver(const E8Config& config)
        : config_(config),
          worker_ready_(false),
          start_signaled_(false),
          window_finished_(false),
          release_signaled_(false),
          worker_tid_(0),
          worker_window_start_ns_(0),
          worker_window_end_ns_(0),
          worker_cpu_time_ns_(0),
          worker_completed_ops_(0),
          worker_api_seek_calls_(0),
          worker_api_next_calls_(0),
          worker_returned_visible_keys_(0) {}

    ~E8MicroDriver() {
        if (db_) {
            db_->Close();
            db_.reset();
        }
    }

    int Run() {
        std::cout << "========================================================\n"
                  << "  Formal V2 - E8 Micro-Benchmark Wait-Point Driver v5\n"
                  << "========================================================\n"
                  << "DB Path:        " << config_.db_path << "\n"
                  << "Trace Dir:      " << config_.trace_dir << "\n"
                  << "Wait Dir:       " << config_.profile_wait_dir << "\n"
                  << "Window Sec:     " << config_.profile_window_sec << "s\n"
                  << "Case Name:      " << config_.profile_case_name << "\n"
                  << "Threshold:      " << config_.threshold << "\n"
                  << "DisableAutoComp:" << (config_.disable_auto_compactions ? "true" : "false") << "\n"
                  << "========================================================\n";

        // 1. Open Database
        if (!OpenDB()) {
            std::cerr << "[DRIVER FATAL] OpenDB failed.\n";
            return 1;
        }

        // 2. Preload 500,000 keys
        if (!PreloadData()) {
            std::cerr << "[DRIVER FATAL] PreloadData failed.\n";
            return 2;
        }

        // 3. Inject 20,000 DeleteRanges (Phase B)
        if (!InjectDeleteRanges()) {
            std::cerr << "[DRIVER FATAL] InjectDeleteRanges failed.\n";
            return 3;
        }

        // 4. Load Micro-Traces (Phase C)
        std::vector<FormalTraceRecord> micro_records;
        if (!LoadMicroTrace(micro_records)) {
            std::cerr << "[DRIVER FATAL] LoadMicroTrace failed.\n";
            return 4;
        }

        // 5. Execute Warmup (10s) on Coordinator
        std::cout << "[DRIVER] Starting 10s warmup...\n";
        ExecuteWarmup(micro_records, 10);
        std::cout << "[DRIVER] Warmup completed.\n";

        // 6. Wait-Point Profiling Handshake with Isolated Worker Thread
        if (!config_.profile_wait_dir.empty()) {
            if (!ExecuteCoordinatedProfiling(micro_records)) {
                std::cerr << "[DRIVER FATAL] Coordinated Profiling failed.\n";
                return 5;
            }
        } else {
            ExecuteDirectWindow(micro_records);
        }

        // 7. Full KV Verification & SHA-256 Calculation
        std::cout << "[DRIVER] Running Full-DB Key-Value Reconciliation on Coordinator...\n";
        if (!VerifyDatabase()) {
            std::cerr << "[DRIVER FATAL] Database state verification failed!\n";
            return 6;
        }

        std::cout << "[DRIVER SUCCESS] E8 run completed successfully with full state verification.\n";
        return 0;
    }

private:
    bool OpenDB() {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = false;
        options.write_buffer_size = config_.write_buffer_size;
        options.max_write_buffer_number = 6;
        options.min_write_buffer_number_to_merge = 1;
        options.memtable_max_range_deletions = config_.threshold;
        options.disable_auto_compactions = config_.disable_auto_compactions;
        options.compression = rocksdb::kNoCompression;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_size = 4 * 1024;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        std::filesystem::create_directories(config_.db_path);

        rocksdb::Status s = rocksdb::DB::Open(options, config_.db_path, &db_);
        if (!s.ok()) {
            std::cerr << "[DRIVER ERROR] RocksDB Open failed: " << s.ToString() << "\n";
            return false;
        }
        return true;
    }

    bool PreloadData() {
        std::cout << "[DRIVER] Preloading " << config_.total_keys << " keys...\n";
        std::string val_payload(config_.value_size, 'V');

        const size_t kBatchSize = 1000;
        for (uint64_t i = 0; i < config_.total_keys; i += kBatchSize) {
            rocksdb::WriteBatch batch;
            for (uint64_t j = 0; j < kBatchSize && (i + j) < config_.total_keys; ++j) {
                batch.Put(FormatKey(i + j), val_payload);
            }
            rocksdb::WriteOptions wopt;
            wopt.disableWAL = true;
            rocksdb::Status s = db_->Write(wopt, &batch);
            if (!s.ok()) {
                std::cerr << "[DRIVER ERROR] Preload write batch failed: " << s.ToString() << "\n";
                return false;
            }
        }

        // Flush preloaded keys to establish background baseline
        rocksdb::FlushOptions fopt;
        fopt.wait = true;
        rocksdb::Status s = db_->Flush(fopt);
        if (!s.ok()) {
            std::cerr << "[DRIVER ERROR] Preload Flush failed: " << s.ToString() << "\n";
            return false;
        }

        std::cout << "[DRIVER] Preload flushed. Recording LSM state:\n";
        std::string sst_props;
        db_->GetProperty("rocksdb.num-files-at-level0", &sst_props);
        std::cout << "  - Level 0 files: " << sst_props << "\n";
        db_->GetProperty("rocksdb.total-sst-files-size", &sst_props);
        std::cout << "  - Total SST size: " << sst_props << " bytes\n";
        return true;
    }

    bool InjectDeleteRanges() {
        std::cout << "[DRIVER] Injecting RangeDeletes from Phase B traces...\n";
        rocksdb::WriteOptions wopt;
        wopt.disableWAL = true;
        uint64_t del_count = 0;

        for (int w = 0; w < 8; ++w) {
            char fname_buf[64];
            snprintf(fname_buf, sizeof(fname_buf), "phase_b-worker-%02d.bin", w);
            std::string fname = config_.trace_dir + "/" + fname_buf;
            if (!std::filesystem::exists(fname)) {
                if (w == 0) {
                    std::cerr << "[DRIVER ERROR] Phase B trace file missing: " << fname << "\n";
                    return false;
                }
                break;
            }

            auto fsize = std::filesystem::file_size(fname);
            size_t num_records = fsize / sizeof(FormalTraceRecord);
            std::vector<FormalTraceRecord> records(num_records);

            std::ifstream fin(fname, std::ios::binary);
            if (!fin.read(reinterpret_cast<char*>(records.data()), fsize)) {
                std::cerr << "[DRIVER ERROR] Reading " << fname << " failed.\n";
                return false;
            }

            for (const auto& rec : records) {
                if (rec.op_type == 3) { // DeleteRange
                    std::string k1 = FormatKey(rec.key1);
                    std::string k2 = FormatKey(rec.key2);
                    rocksdb::Status s = db_->DeleteRange(wopt, db_->DefaultColumnFamily(), k1, k2);
                    if (!s.ok()) {
                        std::cerr << "[DRIVER ERROR] DeleteRange failed: " << s.ToString() << "\n";
                        return false;
                    }
                    del_count++;
                }
            }
        }

        std::cout << "[DRIVER] Injected " << del_count << " DeleteRanges into active MemTable.\n";

        if (config_.post_flush) {
            std::cout << "[DRIVER] Executing explicit Flush to migrate RangeDeletes to L0 SST (PostFlush)...\n";
            rocksdb::FlushOptions fopt;
            fopt.wait = true;
            rocksdb::Status s = db_->Flush(fopt);
            if (!s.ok()) {
                std::cerr << "[DRIVER ERROR] Post-Inject Flush failed: " << s.ToString() << "\n";
                return false;
            }
            std::string sst_props;
            db_->GetProperty("rocksdb.num-files-at-level0", &sst_props);
            std::cout << "  - Level 0 files after PostFlush: " << sst_props << "\n";
        }

        return true;
    }

    bool LoadMicroTrace(std::vector<FormalTraceRecord>& micro_records) {
        std::string fname = config_.trace_dir + "/phase_c-worker-00.bin";
        if (!std::filesystem::exists(fname)) {
            std::cerr << "[DRIVER ERROR] Phase C trace missing: " << fname << "\n";
            return false;
        }
        auto fsize = std::filesystem::file_size(fname);
        size_t num_records = fsize / sizeof(FormalTraceRecord);
        micro_records.resize(num_records);

        std::ifstream fin(fname, std::ios::binary);
        if (!fin.read(reinterpret_cast<char*>(micro_records.data()), fsize)) {
            std::cerr << "[DRIVER ERROR] Reading Phase C failed.\n";
            return false;
        }
        std::cout << "[DRIVER] Loaded " << num_records << " micro-trace records.\n";
        return true;
    }

    void ExecuteSingleOp(const FormalTraceRecord& rec, uint64_t& seek_calls, uint64_t& next_calls, uint64_t& visible_keys) {
        rocksdb::ReadOptions ropt;
        ropt.total_order_seek = true;

        if (rec.op_type == 0) { // Get
            std::string k = FormatKey(rec.key1);
            std::string val;
            rocksdb::Status s = db_->Get(ropt, k, &val);
            seek_calls++;
            if (s.ok()) {
                visible_keys++;
            }
        } else if (rec.op_type == 1) { // Scan [key1, key2)
            std::string start_k = FormatKey(rec.key1);
            std::string end_k = FormatKey(rec.key2);

            auto iter = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ropt));
            iter->Seek(start_k);
            seek_calls++;

            while (iter->Valid()) {
                if (iter->key().ToString() >= end_k) {
                    break;
                }
                visible_keys++;
                iter->Next();
                next_calls++;
            }
        }
    }

    void ExecuteWarmup(const std::vector<FormalTraceRecord>& records, int seconds) {
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::seconds(seconds);
        size_t idx = 0;
        uint64_t dummy_seek = 0, dummy_next = 0, dummy_keys = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            ExecuteSingleOp(records[idx], dummy_seek, dummy_next, dummy_keys);
            idx = (idx + 1) % records.size();
        }
    }

    // Worker thread loop: Strict Kernel Blocking before and after 45s window
    void WorkerThreadFunc(const std::vector<FormalTraceRecord>* records) {
        worker_tid_ = syscall(SYS_gettid);

        // 1. Notify Coordinator that worker is ready
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_ready_ = true;
        }
        worker_cv_.notify_one();

        // 2. Wait-Point 1: Kernel Blocking until Coordinator signals start
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this]() { return start_signaled_.load(); });
        }

        // 3. Execution Window: Exact 45s Single-Op Deadline Loop
        uint64_t cpu_start_ns = GetThreadCpuTimeNanos();
        const auto window_start = std::chrono::steady_clock::now();
        worker_window_start_ns_ = GetCurrentTimeNanos();
        const auto deadline = window_start + std::chrono::seconds(config_.profile_window_sec);

        uint64_t completed_ops = 0;
        uint64_t api_seek = 0;
        uint64_t api_next = 0;
        uint64_t vis_keys = 0;
        size_t trace_idx = 0;
        const size_t num_traces = records->size();

        while (std::chrono::steady_clock::now() < deadline) {
            ExecuteSingleOp((*records)[trace_idx], api_seek, api_next, vis_keys);
            trace_idx = (trace_idx + 1) % num_traces;
            completed_ops++;
        }

        const auto window_end = std::chrono::steady_clock::now();
        worker_window_end_ns_ = GetCurrentTimeNanos();
        uint64_t cpu_end_ns = GetThreadCpuTimeNanos();

        worker_cpu_time_ns_ = (cpu_end_ns - cpu_start_ns);
        worker_completed_ops_ = completed_ops;
        worker_api_seek_calls_ = api_seek;
        worker_api_next_calls_ = api_next;
        worker_returned_visible_keys_ = vis_keys;

        // 4. Notify Coordinator that 45s window has finished
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            window_finished_ = true;
        }
        coordinator_cv_.notify_one();

        // 5. Wait-Point 2: Immediate Kernel Blocking until Coordinator signals release
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this]() { return release_signaled_.load(); });
        }
    }

    bool ExecuteCoordinatedProfiling(const std::vector<FormalTraceRecord>& records) {
        pid_t pid = getpid();

        // 1. Spawn Worker Thread
        std::thread worker(&E8MicroDriver::WorkerThreadFunc, this, &records);

        // 2. Coordinator waits for worker to be ready in kernel sleep
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this]() { return worker_ready_.load(); });
        }

        pid_t fg_tid = worker_tid_.load();
        std::cout << "[COORDINATOR] Worker ready. Process PID=" << pid 
                  << ", Foreground Worker TID=" << fg_tid << "\n";

        // 3. Coordinator writes PROFILE_READY.json atomically
        std::stringstream ss;
        ss << "{\n"
           << "  \"pid\": " << pid << ",\n"
           << "  \"foreground_tid\": " << fg_tid << ",\n"
           << "  \"profile_case\": \"" << config_.profile_case_name << "\",\n"
           << "  \"state\": \"ready\",\n"
           << "  \"range_delete_count\": " << config_.range_delete_count << ",\n"
           << "  \"auto_compactions_disabled\": " << (config_.disable_auto_compactions ? "true" : "false") << ",\n"
           << "  \"timestamp_ns\": " << GetCurrentTimeNanos() << "\n"
           << "}\n";

        if (!WriteAtomicJsonWithDirSync(config_.profile_wait_dir, "PROFILE_READY.json", ss.str())) {
            std::cerr << "[COORDINATOR ERROR] Failed writing PROFILE_READY.json\n";
            start_signaled_ = true;
            release_signaled_ = true;
            worker_cv_.notify_all();
            worker.join();
            return false;
        }

        std::cout << "[COORDINATOR] PROFILE_READY.json written. Waiting for PROFILE_START (timeout 120s)...\n";

        // 4. Coordinator polls for PROFILE_START (Worker remains in kernel sleep)
        std::string start_file = config_.profile_wait_dir + "/PROFILE_START";
        auto wait_start = std::chrono::steady_clock::now();
        bool received_start = false;

        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - wait_start).count() < 120) {
            if (std::filesystem::exists(start_file)) {
                received_start = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!received_start) {
            std::cerr << "[COORDINATOR ERROR] Wait timed out (120s) without PROFILE_START!\n";
            std::stringstream fss;
            fss << "{\"state\": \"failed\", \"reason\": \"wait_start_timeout_120s\", \"timestamp_ns\": " << GetCurrentTimeNanos() << "}\n";
            WriteAtomicJsonWithDirSync(config_.profile_wait_dir, "PROFILE_FAILED.json", fss.str());
            start_signaled_ = true;
            release_signaled_ = true;
            worker_cv_.notify_all();
            worker.join();
            return false;
        }

        std::cout << "[COORDINATOR] PROFILE_START detected! Waking worker thread into 45s window...\n";

        // 5. Wake worker thread
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            start_signaled_ = true;
        }
        worker_cv_.notify_one();

        // 6. Coordinator waits for worker to finish 45s window and re-enter kernel block
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            coordinator_cv_.wait(lock, [this]() { return window_finished_.load(); });
        }

        double elapsed_sec = (double)(worker_window_end_ns_ - worker_window_start_ns_) / 1e9;
        double cpu_sec = (double)worker_cpu_time_ns_ / 1e9;

        std::cout << "[COORDINATOR] Worker finished 45s loop and entered Wait-Point 2 kernel sleep:\n"
                  << "  - Elapsed Wall: " << std::fixed << std::setprecision(6) << elapsed_sec << " s\n"
                  << "  - Worker CPU:   " << std::fixed << std::setprecision(6) << cpu_sec << " s\n"
                  << "  - Completed Ops:" << worker_completed_ops_ << "\n"
                  << "  - True IOPS:    " << (worker_completed_ops_ / elapsed_sec) << " ops/sec\n";

        // 7. Coordinator writes PROFILE_DONE.json atomically
        std::stringstream dss;
        dss << "{\n"
            << "  \"state\": \"done\",\n"
            << "  \"pid\": " << pid << ",\n"
            << "  \"foreground_tid\": " << fg_tid << ",\n"
            << "  \"profile_case\": \"" << config_.profile_case_name << "\",\n"
            << "  \"window_start_ns\": " << worker_window_start_ns_ << ",\n"
            << "  \"window_end_ns\": " << worker_window_end_ns_ << ",\n"
            << "  \"worker_cpu_time_ns\": " << worker_cpu_time_ns_ << ",\n"
            << "  \"elapsed_sec\": " << std::fixed << std::setprecision(6) << elapsed_sec << ",\n"
            << "  \"completed_ops\": " << worker_completed_ops_ << ",\n"
            << "  \"api_seek_calls\": " << worker_api_seek_calls_ << ",\n"
            << "  \"api_next_calls\": " << worker_api_next_calls_ << ",\n"
            << "  \"returned_visible_keys\": " << worker_returned_visible_keys_ << ",\n"
            << "  \"physical_span\": 50,\n"
            << "  \"true_phase_iops\": " << (worker_completed_ops_ / elapsed_sec) << "\n"
            << "}\n";

        if (!WriteAtomicJsonWithDirSync(config_.profile_wait_dir, "PROFILE_DONE.json", dss.str())) {
            std::cerr << "[COORDINATOR ERROR] Failed writing PROFILE_DONE.json\n";
            release_signaled_ = true;
            worker_cv_.notify_all();
            worker.join();
            return false;
        }

        // 8. Coordinator waits for PROFILE_RELEASE (Worker remains kernel blocked)
        std::string release_file = config_.profile_wait_dir + "/PROFILE_RELEASE";
        auto rel_wait_start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - rel_wait_start).count() < 30) {
            if (std::filesystem::exists(release_file)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // 9. Release and Join Worker cleanly
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            release_signaled_ = true;
        }
        worker_cv_.notify_all();
        worker.join();

        std::cout << "[COORDINATOR] Worker thread joined cleanly.\n";
        return true;
    }

    void ExecuteDirectWindow(const std::vector<FormalTraceRecord>& records) {
        std::cout << "[DRIVER] Running standalone " << config_.profile_window_sec << "s window...\n";
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::seconds(config_.profile_window_sec);
        uint64_t ops = 0, seeks = 0, nexts = 0, keys = 0;
        size_t idx = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ExecuteSingleOp(records[idx], seeks, nexts, keys);
            idx = (idx + 1) % records.size();
            ops++;
        }
        auto end = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(end - start).count();
        std::cout << "[DRIVER] Standalone completed: " << ops << " ops in " << el << "s (" << (ops/el) << " IOPS)\n";
    }

    bool VerifyDatabase() {
        rocksdb::ReadOptions ropt;
        ropt.total_order_seek = true;
        auto iter = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ropt));
        iter->SeekToFirst();

        uint64_t count = 0;
        SHA256_CTX sha;
        SHA256_Init(&sha);

        while (iter->Valid()) {
            std::string k = iter->key().ToString();
            std::string v = iter->value().ToString();
            SHA256_Update(&sha, k.data(), k.size());
            SHA256_Update(&sha, v.data(), v.size());
            count++;
            iter->Next();
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha);

        char hex[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }

        std::string expected_sha = config_.is_clean 
            ? "7b13ea6f3920955137a0333c9a8bf56600f467e9dc4a7e129aadc9bdf57c0260"
            : "398b2acc08a8e9de563cf241c0115811dd8fed32e2f02c88bab26aa360329159";

        uint64_t expected_keys = config_.is_clean ? config_.total_keys : (config_.total_keys - 200000);
        std::cout << "[DRIVER RECONCILIATION] Total Visible Keys: " << count 
                  << " (Expected: " << expected_keys << ")\n"
                  << "  - DB SHA-256:       " << hex << "\n"
                  << "  - Model SHA-256:    " << expected_sha << "\n";

        if (count != expected_keys) {
            std::cerr << "[DRIVER ERROR] Visible key count mismatch! Found=" << count << ", Expected=" << expected_keys << "\n";
            return false;
        }

        if (std::string(hex) != expected_sha) {
            std::cerr << "[DRIVER ERROR] Full-DB SHA-256 mismatch against model! DB=" << hex << ", Expected=" << expected_sha << "\n";
            return false;
        }
        std::cout << "[DRIVER RECONCILIATION] State Reconciliation PASSED (Keys & SHA-256 100% matched).\n";
        return true;
    }

    E8Config config_;
    std::unique_ptr<rocksdb::DB> db_;

    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::condition_variable coordinator_cv_;

    std::atomic<bool> worker_ready_;
    std::atomic<bool> start_signaled_;
    std::atomic<bool> window_finished_;
    std::atomic<bool> release_signaled_;
    std::atomic<pid_t> worker_tid_;

    uint64_t worker_window_start_ns_;
    uint64_t worker_window_end_ns_;
    uint64_t worker_cpu_time_ns_;
    uint64_t worker_completed_ops_;
    uint64_t worker_api_seek_calls_;
    uint64_t worker_api_next_calls_;
    uint64_t worker_returned_visible_keys_;
};

int main(int argc, char** argv) {
    E8Config config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
        else if (arg == "--profile_wait_dir" && i + 1 < argc) config.profile_wait_dir = argv[++i];
        else if (arg == "--profile_window_sec" && i + 1 < argc) config.profile_window_sec = std::stoi(argv[++i]);
        else if (arg == "--profile_case" && i + 1 < argc) config.profile_case_name = argv[++i];
        else if (arg == "--threshold" && i + 1 < argc) config.threshold = std::stoul(argv[++i]);
        else if (arg == "--disable_auto_compactions" && i + 1 < argc) config.disable_auto_compactions = (std::string(argv[++i]) == "true");
        else if (arg == "--is_clean" && i + 1 < argc) config.is_clean = (std::string(argv[++i]) == "true");
        else if (arg == "--post_flush" && i + 1 < argc) config.post_flush = (std::string(argv[++i]) == "true");
        else if (arg == "--total_keys" && i + 1 < argc) config.total_keys = std::stoull(argv[++i]);
    }

    if (config.db_path.empty() || config.trace_dir.empty()) {
        std::cerr << "Usage: " << argv[0] << " --db_path <path> --trace_dir <dir> [options]\n";
        return 1;
    }

    E8MicroDriver driver(config);
    return driver.Run();
}
