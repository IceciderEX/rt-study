#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <barrier>
#include <cassert>
#include <memory>
#include <unordered_set>
#include <numeric>
#include <sched.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <openssl/sha.h>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/write_batch.h"

#ifdef ROCKSDB_READ_PATH_AUDIT
#include "db/read_path_audit.h"
#endif

#include "formal_config.h"
#include "manifest_parser.h"
#include "thread_local_histogram.h"
#include "formal_event_listener.h"
#include "worker_state_model.h"
#include "kv_verifier.h"
#include "rtp_mc_controller.h"

#ifndef ROCKSDB_GIT_COMMIT
#define ROCKSDB_GIT_COMMIT "unknown"
#endif
#ifndef STUDY_GIT_COMMIT
#define STUDY_GIT_COMMIT "unknown"
#endif

using namespace study::formal;

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

struct PhaseStatsAgg {
    std::string phase_name;
    double elapsed_sec = 0.0;
    uint64_t completed_ops = 0;
    double true_phase_iops = 0.0;
    double get_live_p50 = 0, get_live_p90 = 0, get_live_p95 = 0, get_live_p99 = 0, get_live_p999 = 0;
    double get_del_p50 = 0, get_del_p90 = 0, get_del_p95 = 0, get_del_p99 = 0, get_del_p999 = 0;
    double scan_p50 = 0, scan_p90 = 0, scan_p95 = 0, scan_p99 = 0, scan_p999 = 0;
    double scan_intersect_p99 = 0;
    double scan_non_intersect_p99 = 0;
    double put_p50 = 0, put_p90 = 0, put_p95 = 0, put_p99 = 0, put_p999 = 0;
    uint64_t scan_total_keys_found = 0;
    double scan_us_per_key = 0.0;
    uint64_t scan_limit_truncated_count = 0;
    uint64_t num_l0_files = 0;
    uint64_t pending_compaction_bytes = 0;
};

// =========================================================================
// E9-DIA: Read Path Dynamic Audit Data Structures & Helpers
// =========================================================================

enum class AuditOpClass : uint8_t {
    kGetLive = 0,
    kGetDeleted = 1,
    kScanIntersect = 2,
    kScanNonIntersect = 3,
    kPut = 4,
    kDeleteRange = 5,
    kCount = 6
};

inline const char* AuditOpClassName(AuditOpClass cls) {
    switch (cls) {
        case AuditOpClass::kGetLive: return "GetLive";
        case AuditOpClass::kGetDeleted: return "GetDeleted";
        case AuditOpClass::kScanIntersect: return "ScanIntersect";
        case AuditOpClass::kScanNonIntersect: return "ScanNonIntersect";
        case AuditOpClass::kPut: return "Put";
        case AuditOpClass::kDeleteRange: return "DeleteRange";
        default: return "Unknown";
    }
}

struct MaterializationEvent {
    std::string run_id;
    int rep = 1;
    int phase = 0;
    int worker = 0;
    uint32_t op_id = 0;
    std::string op_class;
    double latency_us = 0.0;
    int materialized = 0;
    int lock_contended = 0;
    int both_materialized_and_lock_contended = 0;
    int affected_union = 0;
    uint64_t active_mem_id = 0;
    uint64_t active_mem_tombstones = 0;
    double materialization_us = 0.0;
    double lock_wait_us = 0.0;
    double active_mem_prep_us = 0.0;
    double active_mem_lookup_us = 0.0;
    double sst_iter_construct_us = 0.0;
};

struct OpClassAuditStats {
    uint64_t op_count = 0;
    uint64_t total_endpoint_nanos = 0;

    uint64_t materialized_op_count = 0;
    uint64_t lock_contended_op_count = 0;
    uint64_t both_materialized_and_lock_op_count = 0;
    uint64_t materialization_or_lock_affected_reads = 0; // affected_union

    uint64_t view_materialization_nanos = 0;
    uint64_t lock_wait_nanos = 0;
    uint64_t lock_attempt_count = 0;
    uint64_t lock_contended_count = 0;
    uint64_t cache_race_hit_count = 0;
    uint64_t memtable_cache_invalidation_count = 0;

    uint64_t active_mem_tombstone_iter_prep_nanos = 0;
    uint64_t active_mem_tombstone_cover_lookup_nanos = 0;
    uint64_t imm_mem_tombstone_iter_prep_nanos = 0;
    uint64_t imm_mem_tombstone_cover_lookup_nanos = 0;
    uint64_t active_mem_iter_construct_nanos = 0;
    uint64_t imm_mem_iter_construct_nanos = 0;
    uint64_t sst_iter_construct_nanos = 0;

    uint64_t scan_range_del_reseek_count = 0;
    uint64_t scan_boundary_advance_count = 0;
    uint64_t scan_range_del_child_next_count = 0;
    uint64_t scan_covered_skip_count = 0;

    ThreadLocalHistogram endpoint_hist;
    ThreadLocalHistogram affected_read_hist;

#ifdef ROCKSDB_READ_PATH_AUDIT
    void AddDelta(const rocksdb::ReadPathAuditStats& d, uint64_t endpoint_lat_ns, bool is_read) {
        op_count++;
        total_endpoint_nanos += endpoint_lat_ns;
        endpoint_hist.Record(endpoint_lat_ns);

        bool mat = (d.range_tombstone_view_materialization_count > 0);
        bool lock_contended = (d.fragment_build_lock_contended_wait_nanos > 0 || d.fragment_build_lock_contended_count > 0);
        bool both = (mat && lock_contended);
        bool union_aff = (mat || lock_contended);

        if (mat) materialized_op_count++;
        if (lock_contended) lock_contended_op_count++;
        if (both) both_materialized_and_lock_op_count++;

        if (is_read && union_aff) {
            materialization_or_lock_affected_reads++;
            affected_read_hist.Record(endpoint_lat_ns);
        }

        view_materialization_nanos += d.range_tombstone_view_materialization_nanos;
        lock_wait_nanos += d.fragment_build_lock_contended_wait_nanos;
        lock_attempt_count += d.fragment_build_lock_attempt_count;
        lock_contended_count += d.fragment_build_lock_contended_count;
        cache_race_hit_count += d.fragment_build_cache_race_hit_count;
        memtable_cache_invalidation_count += d.memtable_cache_invalidation_count;

        active_mem_tombstone_iter_prep_nanos += d.active_mem_tombstone_iter_prepare_nanos;
        active_mem_tombstone_cover_lookup_nanos += d.active_mem_tombstone_cover_lookup_nanos;
        imm_mem_tombstone_iter_prep_nanos += d.imm_mem_tombstone_iter_prepare_nanos;
        imm_mem_tombstone_cover_lookup_nanos += d.imm_mem_tombstone_cover_lookup_nanos;
        active_mem_iter_construct_nanos += d.active_mem_iter_construct_nanos;
        imm_mem_iter_construct_nanos += d.imm_mem_iter_construct_nanos;
        sst_iter_construct_nanos += d.sst_iter_construct_nanos;

        scan_range_del_reseek_count += d.scan_range_del_reseek_count;
        scan_boundary_advance_count += d.scan_boundary_advance_count;
        scan_range_del_child_next_count += d.scan_range_del_child_next_count;
        scan_covered_skip_count += d.scan_covered_skip_count;
    }
#endif

    void MergeFrom(const OpClassAuditStats& o) {
        op_count += o.op_count;
        total_endpoint_nanos += o.total_endpoint_nanos;
        materialized_op_count += o.materialized_op_count;
        lock_contended_op_count += o.lock_contended_op_count;
        both_materialized_and_lock_op_count += o.both_materialized_and_lock_op_count;
        materialization_or_lock_affected_reads += o.materialization_or_lock_affected_reads;

        view_materialization_nanos += o.view_materialization_nanos;
        lock_wait_nanos += o.lock_wait_nanos;
        lock_attempt_count += o.lock_attempt_count;
        lock_contended_count += o.lock_contended_count;
        cache_race_hit_count += o.cache_race_hit_count;
        memtable_cache_invalidation_count += o.memtable_cache_invalidation_count;

        active_mem_tombstone_iter_prep_nanos += o.active_mem_tombstone_iter_prep_nanos;
        active_mem_tombstone_cover_lookup_nanos += o.active_mem_tombstone_cover_lookup_nanos;
        imm_mem_tombstone_iter_prep_nanos += o.imm_mem_tombstone_iter_prep_nanos;
        imm_mem_tombstone_cover_lookup_nanos += o.imm_mem_tombstone_cover_lookup_nanos;
        active_mem_iter_construct_nanos += o.active_mem_iter_construct_nanos;
        imm_mem_iter_construct_nanos += o.imm_mem_iter_construct_nanos;
        sst_iter_construct_nanos += o.sst_iter_construct_nanos;

        scan_range_del_reseek_count += o.scan_range_del_reseek_count;
        scan_boundary_advance_count += o.scan_boundary_advance_count;
        scan_range_del_child_next_count += o.scan_range_del_child_next_count;
        scan_covered_skip_count += o.scan_covered_skip_count;

        endpoint_hist.MergeFrom(o.endpoint_hist);
        affected_read_hist.MergeFrom(o.affected_read_hist);
    }
};

static std::string ComputeFileSha256(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) return "FILE_NOT_FOUND";
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        SHA256_Update(&ctx, buf, f.gcount());
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}

static bool AssertSafeAuditDbPath(const std::string& db_path, const std::string& exp_id) {
    if (db_path.empty()) return false;
    std::filesystem::path abs_p = std::filesystem::weakly_canonical(std::filesystem::absolute(db_path));
    std::filesystem::path allowed_root = std::filesystem::weakly_canonical(std::filesystem::absolute("run-db/e9_dynamic_audit"));

    std::string p_str = abs_p.string();
    std::string root_str = allowed_root.string();

    if (p_str.compare(0, root_str.length(), root_str) != 0 || p_str == root_str) {
        std::cerr << "[FormalDriver SAFETY VIOLATION] DB path '" << p_str
                  << "' is NOT within pre-registered root directory '" << root_str << "'!" << std::endl;
        return false;
    }
    if (!exp_id.empty() && p_str.find(exp_id) == std::string::npos) {
        std::cerr << "[FormalDriver SAFETY VIOLATION] DB path '" << p_str
                  << "' does NOT contain run_id/exp_id '" << exp_id << "'!" << std::endl;
        return false;
    }
    return true;
}

static bool SafeCleanAuditDbDir(const std::string& db_path, const std::string& exp_id) {
    if (!AssertSafeAuditDbPath(db_path, exp_id)) {
        return false;
    }
    std::filesystem::path abs_p = std::filesystem::weakly_canonical(std::filesystem::absolute(db_path));
    if (std::filesystem::exists(abs_p)) {
        std::cout << "[FormalDriver Safety] Cleaning existing pre-registered DB directory: " << abs_p.string() << std::endl;
        std::error_code ec;
        std::filesystem::remove_all(abs_p, ec);
        if (ec) {
            std::cerr << "[FormalDriver ERROR] Failed to remove DB directory: " << ec.message() << std::endl;
            return false;
        }
    }
    return true;
}

static void DumpRunMetaJson(const std::string& output_dir, const FormalConfig& config, const std::string& config_file_path) {
    std::filesystem::create_directories(output_dir);
    std::string meta_path = output_dir + "/run_meta.json";
    std::ofstream out(meta_path);
    if (!out.is_open()) {
        std::cerr << "[FormalDriver ERROR] Failed to create run_meta.json at " << meta_path << std::endl;
        return;
    }

    std::string binary_sha = ComputeFileSha256("/proc/self/exe");
    std::string manifest_sha = ComputeFileSha256(config.trace_dir + "/manifest.json");
    std::string config_sha = config_file_path.empty() ? "N/A" : ComputeFileSha256(config_file_path);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
    std::string cpu_affinity_str = "[";
    bool first = true;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
            if (!first) cpu_affinity_str += ",";
            cpu_affinity_str += std::to_string(i);
            first = false;
        }
    }
    cpu_affinity_str += "]";

    struct utsname uts;
    std::string os_info = "Linux";
    if (uname(&uts) == 0) {
        os_info = std::string(uts.sysname) + " " + uts.release + " " + uts.version + " " + uts.machine;
    }

    out << "{\n"
        << "  \"exp_id\": \"" << config.exp_id << "\",\n"
        << "  \"rep\": " << config.rep << ",\n"
        << "  \"rocksdb_commit\": \"" << ROCKSDB_GIT_COMMIT << "\",\n"
        << "  \"study_commit\": \"" << STUDY_GIT_COMMIT << "\",\n"
        << "  \"binary_sha256\": \"" << binary_sha << "\",\n"
        << "  \"trace_manifest_sha256\": \"" << manifest_sha << "\",\n"
        << "  \"config_sha256\": \"" << config_sha << "\",\n"
        << "  \"compile_command_and_macros\": \"g++ -O2 -g -std=c++20 -fno-rtti -DNDEBUG -Wall -Wextra -pthread -DROCKSDB_READ_PATH_AUDIT -I/home/wam/grad/rocksdb-v11.8.0/include -I/home/wam/grad/rocksdb-v11.8.0 -I.\",\n"
        << "  \"cpu_affinity\": " << cpu_affinity_str << ",\n"
        << "  \"environment_snapshot\": \"" << os_info << "\",\n"
        << "  \"run_order\": \"Phase A (Read Sensitive) -> Phase B (Write Burst) -> Phase C (Read Recovery), 8 Workers strictly partitioned with start/end barriers\"\n"
        << "}\n";
    out.close();
    std::cout << "[FormalDriver] Dumped run metadata to " << meta_path << "\n";
}

static void FormatStatsCsvLine(std::ostream& os, const std::string& prefix, const OpClassAuditStats& st) {
    double total_lat_ms = st.total_endpoint_nanos / 1e6;
    double p50_us = 0, p90_us = 0, p95_us = 0, p99_us = 0, p999_us = 0, mean_us = 0, max_us = 0;
    st.endpoint_hist.ComputeQuantiles(p50_us, p90_us, p95_us, p99_us, p999_us, mean_us, max_us);

    double aff_p50_us = 0, aff_p90_us = 0, aff_p95_us = 0, aff_p99_us = 0, aff_p999_us = 0, aff_mean_us = 0, aff_max_us = 0;
    st.affected_read_hist.ComputeQuantiles(aff_p50_us, aff_p90_us, aff_p95_us, aff_p99_us, aff_p999_us, aff_mean_us, aff_max_us);

    double mat_rate_per_1k = (st.op_count > 0) ? (1000.0 * st.materialized_op_count / st.op_count) : 0.0;
    double mat_and_lock_ratio = (st.total_endpoint_nanos > 0) ?
        (static_cast<double>(st.view_materialization_nanos + st.lock_wait_nanos) / st.total_endpoint_nanos) : 0.0;

    os << prefix << ","
       << st.op_count << ","
       << std::fixed << std::setprecision(4) << total_lat_ms << ","
       << std::setprecision(2) << p50_us << ","
       << p95_us << ","
       << p99_us << ","
       << st.materialized_op_count << ","
       << std::setprecision(4) << mat_rate_per_1k << ","
       << st.lock_contended_op_count << ","
       << st.both_materialized_and_lock_op_count << ","
       << st.materialization_or_lock_affected_reads << ","
       << std::setprecision(2) << aff_p50_us << ","
       << aff_p95_us << ","
       << aff_p99_us << ","
       << std::setprecision(4) << (st.view_materialization_nanos / 1e6) << ","
       << (st.lock_wait_nanos / 1e6) << ","
       << std::setprecision(6) << mat_and_lock_ratio << ","
       << std::setprecision(4) << (st.active_mem_tombstone_iter_prep_nanos / 1e6) << ","
       << (st.active_mem_tombstone_cover_lookup_nanos / 1e6) << ","
       << (st.imm_mem_tombstone_iter_prep_nanos / 1e6) << ","
       << (st.imm_mem_tombstone_cover_lookup_nanos / 1e6) << ","
       << (st.active_mem_iter_construct_nanos / 1e6) << ","
       << (st.imm_mem_iter_construct_nanos / 1e6) << ","
       << (st.sst_iter_construct_nanos / 1e6) << ","
       << st.scan_range_del_reseek_count << ","
       << st.scan_boundary_advance_count << ","
       << st.scan_range_del_child_next_count << ","
       << st.scan_covered_skip_count << ","
       << st.memtable_cache_invalidation_count << "\n";
}

// =========================================================================
// FormalDriver Class Implementation
// =========================================================================

class FormalDriver {
public:
    FormalDriver(const FormalConfig& config, const std::string& config_file_path = "")
        : config_(config),
          config_file_path_(config_file_path),
          num_workers_(config.num_workers > 0 ? config.num_workers : 8),
          experiment_failed_(false)
    {
        worker_ranges_.resize(num_workers_);
        uint64_t base_k = config_.total_keys / num_workers_;
        uint64_t rem = config_.total_keys % num_workers_;

        uint64_t curr = 0;
        for (int w = 0; w < num_workers_; ++w) {
            uint64_t count = base_k + (w == num_workers_ - 1 ? rem : 0);
            worker_ranges_[w] = {curr, curr + count};
            worker_models_.push_back(std::make_unique<WorkerStateModel>(curr, curr + count, config_.value_size));
            curr += count;
        }

        event_listener_ = std::make_shared<FormalEventListener>();
    }

    bool LoadAllWorkerTraces() {
        TraceManifest manifest;
        std::string manifest_err;
        if (!TraceManifest::ParseAndValidate(config_.trace_dir, config_.total_keys, num_workers_, config_.value_size, manifest, manifest_err)) {
            std::cerr << "[FormalDriver AUDIT ERROR] Trace Manifest validation failed:\n  " << manifest_err << std::endl;
            return false;
        }

        std::cout << "[FormalDriver] Trace Manifest Audit Passed: Workload=" << manifest.workload_id
                  << ", GeneratorCommit=" << manifest.generator_commit
                  << ", ValueSize=" << manifest.value_size
                  << ", TotalOps=" << manifest.total_ops_count << "\n";

        std::unordered_set<uint32_t> global_seen_op_ids;
        global_seen_op_ids.reserve(manifest.total_ops_count);

        worker_traces_.resize(num_workers_);
        for (int w = 0; w < num_workers_; ++w) {
            worker_traces_[w].resize(3);
            uint64_t w_start = worker_ranges_[w].first;
            uint64_t w_end = worker_ranges_[w].second;

            for (int p = 0; p < 3; ++p) {
                std::string p_name = (p == 0 ? "phase_a" : (p == 1 ? "phase_b" : "phase_c"));
                char fname_buf[128];
                snprintf(fname_buf, sizeof(fname_buf), "%s-worker-%02d.bin", p_name.c_str(), w);
                std::string fname = fname_buf;

                auto it = manifest.payload_files.find(fname);
                if (it == manifest.payload_files.end()) {
                    std::cerr << "[FormalDriver AUDIT ERROR] Missing payload file definition: " << fname << std::endl;
                    return false;
                }

                std::string full_path = config_.trace_dir + "/" + fname;
                std::ifstream f(full_path, std::ios::binary);
                if (!f.is_open()) {
                    std::cerr << "[FormalDriver AUDIT ERROR] Failed to open trace payload file: " << full_path << std::endl;
                    return false;
                }

                size_t num_records = it->second.ops_count;
                worker_traces_[w][p].resize(num_records);
                f.read(reinterpret_cast<char*>(worker_traces_[w][p].data()), num_records * sizeof(FormalTraceRecord));
                if (!f) {
                    std::cerr << "[FormalDriver AUDIT ERROR] Incomplete read of " << full_path << std::endl;
                    return false;
                }

                for (size_t i = 0; i < num_records; ++i) {
                    const auto& rec = worker_traces_[w][p][i];
                    if (rec.phase_id != p) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Phase ID mismatch in " << fname << ": expected " << p << " got " << (int)rec.phase_id << std::endl;
                        return false;
                    }
                    if (rec.op_type > 4) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Invalid op_type " << (int)rec.op_type << " in " << fname << std::endl;
                        return false;
                    }
                    if (rec.scan_mode > 1) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Invalid scan_mode " << (int)rec.scan_mode << " in " << fname << std::endl;
                        return false;
                    }
                    if (!global_seen_op_ids.insert(rec.op_id).second) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Duplicate op_id " << rec.op_id << " detected in " << fname << std::endl;
                        return false;
                    }
                    if (rec.key1 < w_start || rec.key1 >= w_end) {
                        std::cerr << "[FormalDriver AUDIT ERROR] Key1 " << rec.key1 << " out of partition ["
                                  << w_start << ", " << w_end << ") in " << fname << std::endl;
                        return false;
                    }
                    if (rec.op_type == 3) { // DeleteRange
                        if (rec.key1 >= rec.key2 || rec.key2 > w_end) {
                            std::cerr << "[FormalDriver AUDIT ERROR] DeleteRange [" << rec.key1 << ", " << rec.key2
                                      << ") exceeds partition bounds [" << w_start << ", " << w_end << ") in " << fname << std::endl;
                            return false;
                        }
                    }
                    if (rec.op_type == 1 && rec.scan_mode == 0) { // SCAN_RANGE
                        if (rec.key1 >= rec.key2 || rec.key2 > w_end) {
                            std::cerr << "[FormalDriver AUDIT ERROR] SCAN_RANGE [" << rec.key1 << ", " << rec.key2
                                      << ") exceeds partition bounds [" << w_start << ", " << w_end << ") in " << fname << std::endl;
                            return false;
                        }
                    }
                }
            }
        }
        std::cout << "[FormalDriver] Passed 100% Comprehensive Admission Audit for 24 payload traces (Unique OpIds: "
                  << global_seen_op_ids.size() << ") from " << config_.trace_dir << "\n";
        return true;
    }

    bool InitializeDB() {
        if (!config_.audit_output_dir.empty() || config_.db_path.find("run-db/e9_dynamic_audit") != std::string::npos) {
            if (!SafeCleanAuditDbDir(config_.db_path, config_.exp_id)) {
                return false;
            }
        } else if (std::filesystem::exists(config_.db_path)) {
            std::cerr << "[FormalDriver ERROR] Target DB directory already exists! Refusing to run on dirty DB: "
                      << config_.db_path << std::endl;
            return false;
        }

        std::filesystem::create_directories(config_.db_path);
        std::filesystem::create_directories(config_.result_dir);
        if (!config_.audit_output_dir.empty()) {
            std::filesystem::create_directories(config_.audit_output_dir);
        }

        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true; // Reject existing dirty DB
        options.compression = rocksdb::kNoCompression;

        options.write_buffer_size = config_.write_buffer_size;
        options.max_write_buffer_number = config_.max_write_buffer_number;
        options.level0_file_num_compaction_trigger = config_.level0_file_num_compaction_trigger;
        options.level0_slowdown_writes_trigger = config_.level0_slowdown_writes_trigger;
        options.level0_stop_writes_trigger = config_.level0_stop_writes_trigger;
        options.target_file_size_base = config_.target_file_size_base;
        options.max_bytes_for_level_base = config_.max_bytes_for_level_base;
        options.max_background_jobs = config_.max_background_jobs;

        options.memtable_max_range_deletions = config_.memtable_max_range_deletions;
        options.memtable_op_scan_flush_trigger = 0; // Fixed 0 to eliminate confounding
        options.enable_range_tombstone_controller = config_.enable_range_tombstone_controller;
        options.range_tombstone_controller_observe_only = config_.range_tombstone_controller_observe_only;
        options.range_tombstone_controller_min_range_deletions = config_.range_tombstone_controller_min_range_deletions;
        options.range_tombstone_controller_min_memtable_bytes = config_.range_tombstone_controller_min_memtable_bytes;
        options.range_tombstone_controller_cooldown_micros = config_.range_tombstone_controller_cooldown_micros;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_size = 4 * 1024;
        table_options.block_cache = rocksdb::NewLRUCache(config_.block_cache_size);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

        db_stats_ = rocksdb::CreateDBStatistics();
        options.statistics = db_stats_;

        options.listeners.push_back(event_listener_);
        options_ = options;

        rocksdb::Status status = rocksdb::DB::Open(options_, config_.db_path, &db_);
        if (!status.ok()) {
            std::cerr << "[FormalDriver ERROR] RocksDB::Open failed: " << status.ToString() << std::endl;
            return false;
        }
        return true;
    }

    bool PreloadDatabase() {
        std::cout << "[Preload] Preloading " << config_.total_keys << " keys (Value size="
                  << config_.value_size << " B) using " << num_workers_ << " concurrent workers...\n";
        auto t0 = std::chrono::steady_clock::now();

        std::vector<std::thread> workers;
        std::atomic<bool> preload_failed(false);

        for (int w = 0; w < num_workers_; ++w) {
            uint64_t start_k = worker_ranges_[w].first;
            uint64_t end_k = worker_ranges_[w].second;

            workers.emplace_back([&, start_k, end_k]() {
                rocksdb::WriteOptions write_opts;
                write_opts.disableWAL = false;
                const size_t kBatchSize = 4096;

                for (uint64_t k = start_k; k < end_k && !preload_failed.load(); k += kBatchSize) {
                    rocksdb::WriteBatch batch;
                    uint64_t batch_end = std::min(k + kBatchSize, end_k);
                    for (uint64_t i = k; i < batch_end; ++i) {
                        std::string key = WorkerStateModel::FormatKey(i);
                        std::string val = WorkerStateModel::GenerateValue(i, 1, config_.value_size);
                        batch.Put(key, val);
                    }
                    rocksdb::Status s = db_->Write(write_opts, &batch);
                    if (!s.ok()) {
                        std::cerr << "[Preload ERROR] WriteBatch failed: " << s.ToString() << std::endl;
                        preload_failed.store(true);
                        break;
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        if (preload_failed.load()) return false;

        std::cout << "[Preload] Flushing preloaded data to SST...\n";
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;
        rocksdb::Status fs = db_->Flush(flush_opts);
        if (!fs.ok()) {
            std::cerr << "[Preload ERROR] Flush failed: " << fs.ToString() << std::endl;
            return false;
        }

        std::cout << "[Preload] Waiting for background compaction to settle...\n";
        rocksdb::WaitForCompactOptions wait_opts;
        rocksdb::Status ws = db_->WaitForCompact(wait_opts);
        if (!ws.ok()) {
            std::cerr << "[Preload ERROR] WaitForCompact failed: " << ws.ToString() << std::endl;
            return false;
        }

        auto t1 = std::chrono::steady_clock::now();
        double preload_sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        std::cout << "[Preload] Preload completed and settled in " << std::fixed << std::setprecision(2) << preload_sec << " s.\n";

        return true;
    }

    bool ExecuteExperiment() {
        std::cout << "\n=========================================================\n";
        std::cout << "  Starting Formal V2 Thesis Experiment: " << config_.exp_id << "\n";
        std::cout << "  Group: " << config_.group_name << ", Threshold: " << config_.memtable_max_range_deletions << "\n";
        std::cout << "  Range Tombstone Controller: "
                  << (config_.enable_range_tombstone_controller ? "enabled" : "disabled")
                  << ", mode="
                  << (config_.range_tombstone_controller_observe_only ? "observe" : "active")
                  << ", min_tombstones="
                  << config_.range_tombstone_controller_min_range_deletions
                  << ", min_memtable_bytes="
                  << config_.range_tombstone_controller_min_memtable_bytes
                  << ", cooldown_us="
                  << config_.range_tombstone_controller_cooldown_micros << "\n";
        std::cout << "  Workers: " << num_workers_ << ", Key Space: " << config_.total_keys << "\n";
        std::cout << "  Audit Mode: " << (!config_.audit_output_dir.empty() ? "ENABLED" : "DISABLED") << "\n";
        std::cout << "=========================================================\n";

        experiment_failed_.store(false);
        db_stats_->Reset();

        // 1. One-time Global Audit Switch (Prior to spawning any workers)
#ifdef ROCKSDB_READ_PATH_AUDIT
        bool audit_active = !config_.audit_output_dir.empty();
        rocksdb::SetReadPathAuditEnabled(audit_active);
#else
        bool audit_active = false;
#endif

        // Instantiate Double-buffered bundles for RTP-MC V2
        std::vector<std::unique_ptr<WorkerHistogramBundle>> worker_rtp_bundles;
        for (int w = 0; w < num_workers_; ++w) {
            worker_rtp_bundles.push_back(std::make_unique<WorkerHistogramBundle>());
        }

        auto rtp_controller = std::make_unique<RtpMcController>(
            db_.get(), config_, config_.exp_id, worker_rtp_bundles, event_listener_);
        event_listener_->SetFlushBeginCallback([&](rocksdb::FlushReason reason, int job_id) {
            rtp_controller->NotifyFlushBegin(reason, job_id);
        });
        event_listener_->SetFlushCompletedCallback([&](rocksdb::FlushReason reason, int job_id, uint64_t out_bytes) {
            rtp_controller->NotifyFlushCompleted(reason, job_id, out_bytes);
        });
        event_listener_->SetMemTableSealedCallback([&](const rocksdb::MemTableInfo& info) {
            rtp_controller->NotifyMemTableSealed(info);
        });
        rtp_controller->Start();

        // Switch EventListener stage to FOREGROUND
        event_listener_->StartForegroundExperiment();
        auto fg_wallclock_t0 = std::chrono::steady_clock::now();

        // Barrier synchronization pairs across 8 workers + 1 coordinator thread = 9
        std::vector<std::unique_ptr<std::barrier<>>> phase_start_barriers;
        std::vector<std::unique_ptr<std::barrier<>>> phase_end_barriers;
        for (int p = 0; p < 3; ++p) {
            phase_start_barriers.push_back(std::make_unique<std::barrier<>>(num_workers_ + 1));
            phase_end_barriers.push_back(std::make_unique<std::barrier<>>(num_workers_ + 1));
        }

        struct SubphaseTracking {
            ThreadLocalHistogram hist_get_live;
            ThreadLocalHistogram hist_get_del;
            ThreadLocalHistogram hist_scan;
            ThreadLocalHistogram hist_scan_intersect;
            ThreadLocalHistogram hist_scan_non_intersect;
            ThreadLocalHistogram hist_put;
            uint64_t scan_keys_found = 0;
            uint64_t scan_limit_truncated_count = 0;
            uint64_t completed_ops = 0;
        };

        struct WorkerThreadStats {
            ThreadLocalHistogram hist_get_live;
            ThreadLocalHistogram hist_get_del;
            ThreadLocalHistogram hist_scan;
            ThreadLocalHistogram hist_scan_intersect;
            ThreadLocalHistogram hist_scan_non_intersect;
            ThreadLocalHistogram hist_put;
            ThreadLocalHistogram hist_del;
            uint64_t scan_keys_found = 0;
            uint64_t scan_limit_truncated_count = 0;
            uint64_t completed_ops = 0;
            uint64_t db_api_calls = 0;
            uint64_t logical_put_bytes = 0;
            SubphaseTracking sub_inj;
            SubphaseTracking sub_post;
        };

        std::vector<std::vector<WorkerThreadStats>> worker_phase_stats(num_workers_);
        for (int w = 0; w < num_workers_; ++w) {
            worker_phase_stats[w].resize(3);
        }

        // Preallocated arrays for E9 audit stats & events
        std::vector<std::vector<std::vector<OpClassAuditStats>>> worker_op_stats(num_workers_);
        for (int w = 0; w < num_workers_; ++w) {
            worker_op_stats[w].resize(3);
            for (int p = 0; p < 3; ++p) {
                worker_op_stats[w][p].resize(static_cast<size_t>(AuditOpClass::kCount));
            }
        }

#ifdef ROCKSDB_READ_PATH_AUDIT
        std::vector<std::vector<rocksdb::ReadPathAuditStats>> worker_phase_tls_snapshot(
            num_workers_, std::vector<rocksdb::ReadPathAuditStats>(3));
#endif
        std::vector<std::vector<MaterializationEvent>> worker_mat_events(num_workers_);

        std::vector<std::thread> workers;
        workers.reserve(num_workers_);
        static std::atomic<uint64_t> global_del_ops{0};

        for (int w = 0; w < num_workers_; ++w) {
            uint64_t w_end = worker_ranges_[w].second;

            workers.emplace_back([&, w, w_end]() {
                rocksdb::ReadOptions read_opts;
                rocksdb::WriteOptions write_opts;
                auto& model = *worker_models_[w];
                std::string w_end_key = WorkerStateModel::FormatKey(w_end);
                rocksdb::Slice w_end_slice(w_end_key);

                for (int p = 0; p < 3; ++p) {
                    const auto& trace = worker_traces_[w][p];
                    auto& stats = worker_phase_stats[w][p];

                    // 1. Wait for coordinator start barrier
                    phase_start_barriers[p]->arrive_and_wait();

                    for (const auto& op : trace) {
                        if (experiment_failed_.load()) break;

#ifdef ROCKSDB_READ_PATH_AUDIT
                        rocksdb::ReadPathAuditStats snap_before;
                        if (audit_active) {
                            snap_before = rocksdb::g_read_path_audit_stats;
                        }
#endif
                        uint64_t lat_ns = 0;
                        AuditOpClass op_cls = AuditOpClass::kGetLive;
                        bool is_read = false;

                        if (op.op_type == 0) { // Get
                            is_read = true;
                            ExpectedState exp_state = model.ClassifyGet(op.key1);

                            if (exp_state == ExpectedState::kExpectedDeleted) {
                                op_cls = AuditOpClass::kGetDeleted;
                            } else {
                                op_cls = AuditOpClass::kGetLive;
                                if ((op.flags & 1) != 0) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] GetLive op " << op.op_id
                                              << " key " << op.key1 << " has trace flag bit 0 set (marked deleted), but model is Live!\n";
                                    experiment_failed_.store(true);
                                    break;
                                }
                            }

                            std::string key = WorkerStateModel::FormatKey(op.key1);
                            std::string val;

                            // Pure DB Call Timing Envelope
                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->Get(read_opts, key, &val);
                            auto t_end = std::chrono::steady_clock::now();
                            lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();

                            // Explicit verification without naked asserts
                            if (op_cls == AuditOpClass::kGetLive) {
                                if (!s.ok()) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Expected Live key "
                                              << key << " NOT FOUND! Status: " << s.ToString() << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                                uint32_t exp_ver = model.GetKeyVersion(op.key1);
                                std::string exp_val = WorkerStateModel::GenerateValue(op.key1, exp_ver, config_.value_size);
                                if (val != exp_val) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Value corruption for key " << key << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                            } else {
                                if (!s.IsNotFound()) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Expected Deleted key "
                                              << key << " was FOUND! Status: " << s.ToString() << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                            }
                            stats.db_api_calls++;

                            if (op_cls == AuditOpClass::kGetDeleted) {
                                stats.hist_get_del.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_get_del.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_get_del.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_get_del.Record(lat_ns);
                            } else {
                                stats.hist_get_live.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_get_live.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_get_live.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_get_live.Record(lat_ns);
                            }

                        } else if (op.op_type == 1) { // Scan
                            is_read = true;
                            uint64_t total_in_range = (op.scan_mode == 0) ? (std::min(op.key2, w_end) - op.key1) : 0;
                            uint64_t exp_keys = (op.scan_mode == 0) ? model.CountExpectedLiveKeys(op.key1, std::min(op.key2, w_end)) : 0;

                            if (op.scan_mode == 0) {
                                if (exp_keys < total_in_range || (op.flags & 1) != 0) {
                                    op_cls = AuditOpClass::kScanIntersect;
                                } else {
                                    op_cls = AuditOpClass::kScanNonIntersect;
                                }
                                if ((op.flags & 1) != 0 && exp_keys == total_in_range && total_in_range > 0) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Scan op " << op.op_id
                                              << " has trace flag bit 0 set (marked intersect), but model has NO deleted keys!\n";
                                    experiment_failed_.store(true);
                                    break;
                                }
                            } else {
                                op_cls = ((op.flags & 1) != 0) ? AuditOpClass::kScanIntersect : AuditOpClass::kScanNonIntersect;
                            }

                            std::string start_key = WorkerStateModel::FormatKey(op.key1);
                            uint64_t keys_found = 0;
                            uint64_t limit_k = op.key2;
                            std::unique_ptr<rocksdb::Iterator> it;

                            // Pure DB Scan Timing Envelope
                            auto t_start = std::chrono::steady_clock::now();
                            it.reset(db_->NewIterator(read_opts));
                            it->Seek(start_key);

                            if (op.scan_mode == 0) { // SCAN_RANGE
                                std::string end_key = WorkerStateModel::FormatKey(op.key2);
                                rocksdb::Slice end_slice(end_key);
                                while (it->Valid() && it->key().compare(end_slice) < 0 && it->key().compare(w_end_slice) < 0) {
                                    keys_found++;
                                    it->Next();
                                }
                            } else { // SCAN_LIMIT
                                while (it->Valid() && keys_found < limit_k && it->key().compare(w_end_slice) < 0) {
                                    keys_found++;
                                    it->Next();
                                }
                            }
                            auto t_end = std::chrono::steady_clock::now();
                            lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();

                            // Explicit iterator error check
                            if (!it->status().ok()) {
                                std::cerr << "[Worker " << w << " CRITICAL ERROR] Scan iterator error: " << it->status().ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }

                            // Explicit returned keys check against state model
                            if (op.scan_mode == 0) {
                                if (keys_found != exp_keys) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] Scan keys mismatch for op " << op.op_id
                                              << " [" << op.key1 << ", " << op.key2 << "): expected "
                                              << exp_keys << ", actual " << keys_found << ", flags=" << (int)op.flags << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }

                                if (op_cls == AuditOpClass::kScanNonIntersect && keys_found != total_in_range) {
                                    std::cerr << "[Worker " << w << " CRITICAL ERROR] ScanNonIntersect op " << op.op_id
                                              << " expected full live " << total_in_range << " but returned "
                                              << keys_found << std::endl;
                                    experiment_failed_.store(true);
                                    break;
                                }
                            }

                            stats.hist_scan.Record(lat_ns);
                            worker_rtp_bundles[w]->hist_scan.Record(lat_ns);
                            stats.scan_keys_found += keys_found;

                            if (op_cls == AuditOpClass::kScanIntersect) {
                                stats.hist_scan_intersect.Record(lat_ns);
                                worker_rtp_bundles[w]->hist_scan_intersect.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_scan_intersect.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_scan_intersect.Record(lat_ns);
                            } else {
                                stats.hist_scan_non_intersect.Record(lat_ns);
                                if (op.flags & 2) stats.sub_inj.hist_scan_non_intersect.Record(lat_ns);
                                if (op.flags & 4) stats.sub_post.hist_scan_non_intersect.Record(lat_ns);
                            }

                            if (op.flags & 2) {
                                stats.sub_inj.hist_scan.Record(lat_ns);
                                stats.sub_inj.scan_keys_found += keys_found;
                            }
                            if (op.flags & 4) {
                                stats.sub_post.hist_scan.Record(lat_ns);
                                stats.sub_post.scan_keys_found += keys_found;
                            }

                            if (op.scan_mode == 1 && keys_found < limit_k) {
                                stats.scan_limit_truncated_count++;
                                if (op.flags & 2) stats.sub_inj.scan_limit_truncated_count++;
                                if (op.flags & 4) stats.sub_post.scan_limit_truncated_count++;
                            }
                            stats.db_api_calls++;

                        } else if (op.op_type == 2) { // Put
                            op_cls = AuditOpClass::kPut;
                            uint32_t next_ver = model.GetKeyVersion(op.key1) + 1;
                            std::string key = WorkerStateModel::FormatKey(op.key1);
                            std::string val = WorkerStateModel::GenerateValue(op.key1, next_ver, config_.value_size);

                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->Put(write_opts, key, val);
                            auto t_end = std::chrono::steady_clock::now();
                            lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();

                            stats.hist_put.Record(lat_ns);
                            worker_rtp_bundles[w]->hist_put.Record(lat_ns);
                            if (op.flags & 2) stats.sub_inj.hist_put.Record(lat_ns);
                            if (op.flags & 4) stats.sub_post.hist_put.Record(lat_ns);

                            if (!s.ok()) {
                                std::cerr << "[Worker " << w << " CRITICAL ERROR] Put failed: " << s.ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }
                            model.ApplyPut(op.key1);
                            stats.logical_put_bytes += config_.value_size;
                            stats.db_api_calls++;

                        } else if (op.op_type == 3) { // DeleteRange
                            op_cls = AuditOpClass::kDeleteRange;
                            std::string start_key = WorkerStateModel::FormatKey(op.key1);
                            std::string end_key = WorkerStateModel::FormatKey(op.key2);

                            auto t_start = std::chrono::steady_clock::now();
                            rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), start_key, end_key);
                            auto t_end = std::chrono::steady_clock::now();
                            lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();

                            stats.hist_del.Record(lat_ns);

                            if (!s.ok()) {
                                std::cerr << "[Worker " << w << " CRITICAL ERROR] DeleteRange failed: " << s.ToString() << std::endl;
                                experiment_failed_.store(true);
                                break;
                            }
                            model.ApplyDeleteRange(op.key1, op.key2);
                            stats.db_api_calls++;

                            if ((global_del_ops.fetch_add(1, std::memory_order_relaxed) + 1) % config_.range_del_checkpoint == 0) {
                                rtp_controller->NotifyDeleteRangeCheckpoint();
                            }
                        }

                        stats.completed_ops++;
                        if (op.flags & 2) stats.sub_inj.completed_ops++;
                        if (op.flags & 4) stats.sub_post.completed_ops++;

#ifdef ROCKSDB_READ_PATH_AUDIT
                        if (audit_active) {
                            rocksdb::ReadPathAuditStats snap_after = rocksdb::g_read_path_audit_stats;
                            rocksdb::ReadPathAuditStats delta;
                            delta.range_tombstone_view_materialization_count = snap_after.range_tombstone_view_materialization_count - snap_before.range_tombstone_view_materialization_count;
                            delta.range_tombstone_view_materialization_nanos = snap_after.range_tombstone_view_materialization_nanos - snap_before.range_tombstone_view_materialization_nanos;
                            delta.memtable_cache_invalidation_count = snap_after.memtable_cache_invalidation_count - snap_before.memtable_cache_invalidation_count;
                            delta.fragment_build_lock_attempt_count = snap_after.fragment_build_lock_attempt_count - snap_before.fragment_build_lock_attempt_count;
                            delta.fragment_build_lock_contended_count = snap_after.fragment_build_lock_contended_count - snap_before.fragment_build_lock_contended_count;
                            delta.fragment_build_lock_contended_wait_nanos = snap_after.fragment_build_lock_contended_wait_nanos - snap_before.fragment_build_lock_contended_wait_nanos;
                            delta.fragment_build_cache_race_hit_count = snap_after.fragment_build_cache_race_hit_count - snap_before.fragment_build_cache_race_hit_count;

                            delta.active_mem_tombstone_iter_prepare_count = snap_after.active_mem_tombstone_iter_prepare_count - snap_before.active_mem_tombstone_iter_prepare_count;
                            delta.active_mem_tombstone_iter_prepare_nanos = snap_after.active_mem_tombstone_iter_prepare_nanos - snap_before.active_mem_tombstone_iter_prepare_nanos;
                            delta.active_mem_tombstone_cover_lookup_count = snap_after.active_mem_tombstone_cover_lookup_count - snap_before.active_mem_tombstone_cover_lookup_count;
                            delta.active_mem_tombstone_cover_lookup_nanos = snap_after.active_mem_tombstone_cover_lookup_nanos - snap_before.active_mem_tombstone_cover_lookup_nanos;

                            delta.imm_mem_tombstone_iter_prepare_count = snap_after.imm_mem_tombstone_iter_prepare_count - snap_before.imm_mem_tombstone_iter_prepare_count;
                            delta.imm_mem_tombstone_iter_prepare_nanos = snap_after.imm_mem_tombstone_iter_prepare_nanos - snap_before.imm_mem_tombstone_iter_prepare_nanos;
                            delta.imm_mem_tombstone_cover_lookup_count = snap_after.imm_mem_tombstone_cover_lookup_count - snap_before.imm_mem_tombstone_cover_lookup_count;
                            delta.imm_mem_tombstone_cover_lookup_nanos = snap_after.imm_mem_tombstone_cover_lookup_nanos - snap_before.imm_mem_tombstone_cover_lookup_nanos;

                            delta.active_mem_iter_construct_count = snap_after.active_mem_iter_construct_count - snap_before.active_mem_iter_construct_count;
                            delta.active_mem_iter_construct_nanos = snap_after.active_mem_iter_construct_nanos - snap_before.active_mem_iter_construct_nanos;
                            delta.imm_mem_iter_construct_count = snap_after.imm_mem_iter_construct_count - snap_before.imm_mem_iter_construct_count;
                            delta.imm_mem_iter_construct_nanos = snap_after.imm_mem_iter_construct_nanos - snap_before.imm_mem_iter_construct_nanos;
                            delta.sst_iter_construct_count = snap_after.sst_iter_construct_count - snap_before.sst_iter_construct_count;
                            delta.sst_iter_construct_nanos = snap_after.sst_iter_construct_nanos - snap_before.sst_iter_construct_nanos;

                            delta.scan_range_del_reseek_count = snap_after.scan_range_del_reseek_count - snap_before.scan_range_del_reseek_count;
                            delta.scan_boundary_advance_count = snap_after.scan_boundary_advance_count - snap_before.scan_boundary_advance_count;
                            delta.scan_range_del_child_next_count = snap_after.scan_range_del_child_next_count - snap_before.scan_range_del_child_next_count;
                            delta.scan_covered_skip_count = snap_after.scan_covered_skip_count - snap_before.scan_covered_skip_count;

                            worker_op_stats[w][p][static_cast<size_t>(op_cls)].AddDelta(delta, lat_ns, is_read);

                            if (is_read && (delta.range_tombstone_view_materialization_count > 0 ||
                                            delta.fragment_build_lock_contended_wait_nanos > 0 ||
                                            delta.fragment_build_lock_contended_count > 0)) {
                                MaterializationEvent evt;
                                evt.run_id = config_.exp_id;
                                evt.rep = config_.rep;
                                evt.phase = p;
                                evt.worker = w;
                                evt.op_id = op.op_id;
                                evt.op_class = AuditOpClassName(op_cls);
                                evt.latency_us = lat_ns / 1000.0;
                                evt.materialized = (delta.range_tombstone_view_materialization_count > 0) ? 1 : 0;
                                evt.lock_contended = (delta.fragment_build_lock_contended_wait_nanos > 0 || delta.fragment_build_lock_contended_count > 0) ? 1 : 0;
                                evt.both_materialized_and_lock_contended = (evt.materialized && evt.lock_contended) ? 1 : 0;
                                evt.affected_union = (evt.materialized || evt.lock_contended) ? 1 : 0;
                                if (evt.materialized) {
                                    evt.active_mem_id = snap_after.last_materialization_memtable_id;
                                    evt.active_mem_tombstones = snap_after.last_materialization_tombstone_count;
                                } else {
                                    evt.active_mem_id = snap_after.last_contended_memtable_id;
                                    evt.active_mem_tombstones = snap_after.last_contended_tombstone_count;
                                }
                                evt.materialization_us = delta.range_tombstone_view_materialization_nanos / 1000.0;
                                evt.lock_wait_us = delta.fragment_build_lock_contended_wait_nanos / 1000.0;
                                evt.active_mem_prep_us = delta.active_mem_tombstone_iter_prepare_nanos / 1000.0;
                                evt.active_mem_lookup_us = delta.active_mem_tombstone_cover_lookup_nanos / 1000.0;
                                evt.sst_iter_construct_us = delta.sst_iter_construct_nanos / 1000.0;
                                worker_mat_events[w].push_back(evt);
                            }
                        }
#endif
                    } // end op in trace

                    // Multi-threaded snapshot requirement:
                    // 1. Worker copies from its TLS into preallocated snapshot array
                    // 2. Worker resets its TLS
                    // 3. Worker enters the end barrier
#ifdef ROCKSDB_READ_PATH_AUDIT
                    if (audit_active) {
                        worker_phase_tls_snapshot[w][p] = rocksdb::g_read_path_audit_stats;
                        rocksdb::g_read_path_audit_stats.Reset();
                    }
#endif

                    // 2. Arrive at phase end barrier and wait for coordinator & all workers
                    phase_end_barriers[p]->arrive_and_wait();
                }
            });
        }

        // Coordinator Thread: Controls Phase Timing with Exact Start/End Barrier Pairs
        std::vector<PhaseStatsAgg> phase_results;
        std::vector<std::string> phase_names = {
            "Phase A (Read Sensitive)",
            "Phase B (Write Burst)",
            "Phase C (Read Recovery)"
        };

        double sum_phase_active_sec = 0.0;
        double oracle_flush_wait_sec = 0.0;

        for (int p = 0; p < 3; ++p) {
            std::cout << "[Coordinator] Phase " << p << " (" << phase_names[p] << ") released at exact T0...\n";
            rtp_controller->SetCurrentPhase(p);
            auto p_t0 = std::chrono::steady_clock::now();

            // Release all workers simultaneously
            phase_start_barriers[p]->arrive_and_wait();

            // Wait for all workers to finish this phase
            phase_end_barriers[p]->arrive_and_wait();

            auto p_t1 = std::chrono::steady_clock::now();
            double p_sec = std::chrono::duration_cast<std::chrono::duration<double>>(p_t1 - p_t0).count();
            sum_phase_active_sec += p_sec;

            // Aggregate Phase Stats
            PhaseStatsAgg agg;
            agg.phase_name = phase_names[p];
            agg.elapsed_sec = p_sec;

            ThreadLocalHistogram p_hist_get_live;
            ThreadLocalHistogram p_hist_get_del;
            ThreadLocalHistogram p_hist_scan;
            ThreadLocalHistogram p_hist_scan_intersect;
            ThreadLocalHistogram p_hist_scan_non_intersect;
            ThreadLocalHistogram p_hist_put;

            for (int w = 0; w < num_workers_; ++w) {
                const auto& ws = worker_phase_stats[w][p];
                agg.completed_ops += ws.completed_ops;
                agg.scan_total_keys_found += ws.scan_keys_found;
                agg.scan_limit_truncated_count += ws.scan_limit_truncated_count;

                p_hist_get_live.MergeFrom(ws.hist_get_live);
                p_hist_get_del.MergeFrom(ws.hist_get_del);
                p_hist_scan.MergeFrom(ws.hist_scan);
                p_hist_scan_intersect.MergeFrom(ws.hist_scan_intersect);
                p_hist_scan_non_intersect.MergeFrom(ws.hist_scan_non_intersect);
                p_hist_put.MergeFrom(ws.hist_put);
            }

            agg.true_phase_iops = (p_sec > 0) ? (agg.completed_ops / p_sec) : 0.0;

            double dummy_mean, dummy_max, dummy_50, dummy_90, dummy_95, dummy_999;
            p_hist_get_live.ComputeQuantiles(agg.get_live_p50, agg.get_live_p90, agg.get_live_p95, agg.get_live_p99, agg.get_live_p999, dummy_mean, dummy_max);
            p_hist_get_del.ComputeQuantiles(agg.get_del_p50, agg.get_del_p90, agg.get_del_p95, agg.get_del_p99, agg.get_del_p999, dummy_mean, dummy_max);
            p_hist_scan.ComputeQuantiles(agg.scan_p50, agg.scan_p90, agg.scan_p95, agg.scan_p99, agg.scan_p999, dummy_mean, dummy_max);
            p_hist_scan_intersect.ComputeQuantiles(dummy_50, dummy_90, dummy_95, agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
            p_hist_scan_non_intersect.ComputeQuantiles(dummy_50, dummy_90, dummy_95, agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
            p_hist_put.ComputeQuantiles(agg.put_p50, agg.put_p90, agg.put_p95, agg.put_p99, agg.put_p999, dummy_mean, dummy_max);

            if (agg.scan_total_keys_found > 0) {
                agg.scan_us_per_key = (p_hist_scan.GetSumNs() / 1000.0) / agg.scan_total_keys_found;
            }

            std::string l0_str, pending_str;
            if (db_->GetProperty("rocksdb.num-files-at-level0", &l0_str)) {
                try { agg.num_l0_files = std::stoull(l0_str); } catch (...) {}
            }
            if (db_->GetProperty("rocksdb.estimate-pending-compaction-bytes", &pending_str)) {
                try { agg.pending_compaction_bytes = std::stoull(pending_str); } catch (...) {}
            }

            phase_results.push_back(agg);
            std::cout << "[Coordinator] Phase " << p << " completed in " << std::fixed << std::setprecision(4)
                      << p_sec << " s, True IOPS = " << std::setprecision(2) << agg.true_phase_iops << "\n";

            // If Phase B, also aggregate Phase B-Inject and Phase B-PostBurst
            if (p == 1) {
                PhaseStatsAgg inj_agg;
                inj_agg.phase_name = "Phase B-Inject (20% Window)";
                inj_agg.elapsed_sec = p_sec * 0.20;

                PhaseStatsAgg post_agg;
                post_agg.phase_name = "Phase B-PostBurst (80% Window)";
                post_agg.elapsed_sec = p_sec * 0.80;

                ThreadLocalHistogram inj_hist_get_live, inj_hist_get_del, inj_hist_scan, inj_hist_scan_int, inj_hist_scan_non, inj_hist_put;
                ThreadLocalHistogram post_hist_get_live, post_hist_get_del, post_hist_scan, post_hist_scan_int, post_hist_scan_non, post_hist_put;

                for (int w = 0; w < num_workers_; ++w) {
                    const auto& ws = worker_phase_stats[w][1];
                    inj_agg.completed_ops += ws.sub_inj.completed_ops;
                    inj_agg.scan_total_keys_found += ws.sub_inj.scan_keys_found;
                    inj_agg.scan_limit_truncated_count += ws.sub_inj.scan_limit_truncated_count;
                    inj_hist_get_live.MergeFrom(ws.sub_inj.hist_get_live);
                    inj_hist_get_del.MergeFrom(ws.sub_inj.hist_get_del);
                    inj_hist_scan.MergeFrom(ws.sub_inj.hist_scan);
                    inj_hist_scan_int.MergeFrom(ws.sub_inj.hist_scan_intersect);
                    inj_hist_scan_non.MergeFrom(ws.sub_inj.hist_scan_non_intersect);
                    inj_hist_put.MergeFrom(ws.sub_inj.hist_put);

                    post_agg.completed_ops += ws.sub_post.completed_ops;
                    post_agg.scan_total_keys_found += ws.sub_post.scan_keys_found;
                    post_agg.scan_limit_truncated_count += ws.sub_post.scan_limit_truncated_count;
                    post_hist_get_live.MergeFrom(ws.sub_post.hist_get_live);
                    post_hist_get_del.MergeFrom(ws.sub_post.hist_get_del);
                    post_hist_scan.MergeFrom(ws.sub_post.hist_scan);
                    post_hist_scan_int.MergeFrom(ws.sub_post.hist_scan_intersect);
                    post_hist_scan_non.MergeFrom(ws.sub_post.hist_scan_non_intersect);
                    post_hist_put.MergeFrom(ws.sub_post.hist_put);
                }

                if (inj_agg.elapsed_sec > 0) inj_agg.true_phase_iops = inj_agg.completed_ops / inj_agg.elapsed_sec;
                inj_hist_get_live.ComputeQuantiles(inj_agg.get_live_p50, inj_agg.get_live_p90, inj_agg.get_live_p95, inj_agg.get_live_p99, inj_agg.get_live_p999, dummy_mean, dummy_max);
                inj_hist_get_del.ComputeQuantiles(inj_agg.get_del_p50, inj_agg.get_del_p90, inj_agg.get_del_p95, inj_agg.get_del_p99, inj_agg.get_del_p999, dummy_mean, dummy_max);
                inj_hist_scan.ComputeQuantiles(inj_agg.scan_p50, inj_agg.scan_p90, inj_agg.scan_p95, inj_agg.scan_p99, inj_agg.scan_p999, dummy_mean, dummy_max);
                inj_hist_scan_int.ComputeQuantiles(dummy_50, dummy_90, dummy_95, inj_agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
                inj_hist_scan_non.ComputeQuantiles(dummy_50, dummy_90, dummy_95, inj_agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
                inj_hist_put.ComputeQuantiles(inj_agg.put_p50, inj_agg.put_p90, inj_agg.put_p95, inj_agg.put_p99, inj_agg.put_p999, dummy_mean, dummy_max);
                if (inj_agg.scan_total_keys_found > 0) inj_agg.scan_us_per_key = (inj_hist_scan.GetSumNs() / 1000.0) / inj_agg.scan_total_keys_found;

                if (post_agg.elapsed_sec > 0) post_agg.true_phase_iops = post_agg.completed_ops / post_agg.elapsed_sec;
                post_hist_get_live.ComputeQuantiles(post_agg.get_live_p50, post_agg.get_live_p90, post_agg.get_live_p95, post_agg.get_live_p99, post_agg.get_live_p999, dummy_mean, dummy_max);
                post_hist_get_del.ComputeQuantiles(post_agg.get_del_p50, post_agg.get_del_p90, post_agg.get_del_p95, post_agg.get_del_p99, post_agg.get_del_p999, dummy_mean, dummy_max);
                post_hist_scan.ComputeQuantiles(post_agg.scan_p50, post_agg.scan_p90, post_agg.scan_p95, post_agg.scan_p99, post_agg.scan_p999, dummy_mean, dummy_max);
                post_hist_scan_int.ComputeQuantiles(dummy_50, dummy_90, dummy_95, post_agg.scan_intersect_p99, dummy_999, dummy_mean, dummy_max);
                post_hist_scan_non.ComputeQuantiles(dummy_50, dummy_90, dummy_95, post_agg.scan_non_intersect_p99, dummy_999, dummy_mean, dummy_max);
                post_hist_put.ComputeQuantiles(post_agg.put_p50, post_agg.put_p90, post_agg.put_p95, post_agg.put_p99, post_agg.put_p999, dummy_mean, dummy_max);
                if (post_agg.scan_total_keys_found > 0) post_agg.scan_us_per_key = (post_hist_scan.GetSumNs() / 1000.0) / post_agg.scan_total_keys_found;

                phase_results.push_back(inj_agg);
                phase_results.push_back(post_agg);

                if (config_.oracle_flush_after_phase_b) {
                    std::cout << "[Coordinator] Executing Oracle Synchronous Flush after Phase B...\n";
                    auto oracle_t0 = std::chrono::steady_clock::now();
                    rocksdb::FlushOptions flush_opts;
                    flush_opts.wait = true;
                    rocksdb::Status fs = db_->Flush(flush_opts);
                    auto oracle_t1 = std::chrono::steady_clock::now();
                    oracle_flush_wait_sec = std::chrono::duration_cast<std::chrono::duration<double>>(oracle_t1 - oracle_t0).count();
                    std::cout << "[Coordinator] Oracle Flush completed in " << std::fixed << std::setprecision(4)
                              << oracle_flush_wait_sec << " s, Status: " << fs.ToString() << "\n";
                    if (!fs.ok()) {
                        std::cerr << "[Coordinator ERROR] Oracle Flush failed: " << fs.ToString() << "\n";
                        experiment_failed_.store(true);
                    }
                }
            }
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        // Disable audit globally once foreground finishes and all workers have joined
#ifdef ROCKSDB_READ_PATH_AUDIT
        if (audit_active) {
            rocksdb::SetReadPathAuditEnabled(false);
        }
#endif

        rtp_controller->Stop();

        auto fg_wallclock_t1 = std::chrono::steady_clock::now();
        double foreground_wallclock_sec = std::chrono::duration_cast<std::chrono::duration<double>>(fg_wallclock_t1 - fg_wallclock_t0).count();

        if (experiment_failed_.load()) {
            std::cerr << "[FormalDriver CRITICAL ERROR] Experiment failed during phase execution!\n";
            return false;
        }

        // Switch EventListener stage to COOLDOWN observation window (Strict 10s Window)
        event_listener_->StartCooldownObservation();
        std::cout << "\n[Cooldown] Strict 10-second post-run background observation window...\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Switch EventListener stage to VERIFICATION (Freezes Cooldown Metrics)
        event_listener_->StartVerificationStage();

        // Deep Key/Value & SHA-256 Full-Scan Verification
        std::cout << "\n[Verification] Running Full Key/Value Version-Aware Verification...\n";
        auto ver_report = KvVerifier::VerifyFullDatabase(db_.get(), worker_models_, config_.total_keys, config_.value_size);

        std::cout << "  DB Live Keys:    " << ver_report.db_live_keys << " (Expected: " << ver_report.model_live_keys << ")\n";
        std::cout << "  DB Payload:      " << (ver_report.db_payload_bytes / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "  DB SHA-256:      " << ver_report.db_sha256_hex << "\n";
        std::cout << "  Model SHA-256:   " << ver_report.model_sha256_hex << "\n";
        std::cout << "  Verification:    " << (ver_report.is_pass ? ">> PASS <<" : ">> FAIL <<") << "\n";

        if (!ver_report.is_pass) {
            std::cerr << "[FormalDriver CRITICAL ERROR] Verification FAILED: " << ver_report.error_detail << std::endl;
            return false;
        }

        // =========================================================================
        // E9 Audit Consistency Check and CSV Generation
        // =========================================================================
        if (audit_active) {
            std::cout << "\n[Audit Processing] Aggregating multi-threaded audit snapshots and validating identities...\n";

            // 1. Calculate Phase Summaries across 8 Workers
            std::vector<std::vector<OpClassAuditStats>> phase_summaries(3);
            std::vector<OpClassAuditStats> phase_read_totals(3);
            std::vector<OpClassAuditStats> phase_all_totals(3);

            for (int p = 0; p < 3; ++p) {
                phase_summaries[p].resize(static_cast<size_t>(AuditOpClass::kCount));
                for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                    for (int w = 0; w < num_workers_; ++w) {
                        phase_summaries[p][c].MergeFrom(worker_op_stats[w][p][c]);
                    }
                    if (c <= static_cast<size_t>(AuditOpClass::kScanNonIntersect)) {
                        phase_read_totals[p].MergeFrom(phase_summaries[p][c]);
                    }
                    phase_all_totals[p].MergeFrom(phase_summaries[p][c]);
                }
            }

            // 2. Calculate Run Summary across 3 Phases
            std::vector<OpClassAuditStats> run_summary(static_cast<size_t>(AuditOpClass::kCount));
            OpClassAuditStats run_read_total;
            OpClassAuditStats run_all_total;

            for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                for (int p = 0; p < 3; ++p) {
                    run_summary[c].MergeFrom(phase_summaries[p][c]);
                }
                if (c <= static_cast<size_t>(AuditOpClass::kScanNonIntersect)) {
                    run_read_total.MergeFrom(run_summary[c]);
                }
                run_all_total.MergeFrom(run_summary[c]);
            }

            // 3. Strict Assertions: sum(worker) == phase, sum(phase) == run, and TLS equality
#ifdef ROCKSDB_READ_PATH_AUDIT
            for (int w = 0; w < num_workers_; ++w) {
                for (int p = 0; p < 3; ++p) {
                    uint64_t sum_mat_ns = 0, sum_lock_wait_ns = 0, sum_inval_cnt = 0;
                    uint64_t sum_prep_ns = 0, sum_cover_ns = 0, sum_sst_iter_ns = 0;
                    uint64_t sum_reseek = 0, sum_boundary = 0, sum_child_next = 0, sum_covered_skip = 0;

                    for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                        const auto& st = worker_op_stats[w][p][c];
                        sum_mat_ns += st.view_materialization_nanos;
                        sum_lock_wait_ns += st.lock_wait_nanos;
                        sum_inval_cnt += st.memtable_cache_invalidation_count;
                        sum_prep_ns += st.active_mem_tombstone_iter_prep_nanos;
                        sum_cover_ns += st.active_mem_tombstone_cover_lookup_nanos;
                        sum_sst_iter_ns += st.sst_iter_construct_nanos;
                        sum_reseek += st.scan_range_del_reseek_count;
                        sum_boundary += st.scan_boundary_advance_count;
                        sum_child_next += st.scan_range_del_child_next_count;
                        sum_covered_skip += st.scan_covered_skip_count;
                    }

                    const auto& tls = worker_phase_tls_snapshot[w][p];
                    if (sum_mat_ns != tls.range_tombstone_view_materialization_nanos ||
                        sum_lock_wait_ns != tls.fragment_build_lock_contended_wait_nanos ||
                        sum_inval_cnt != tls.memtable_cache_invalidation_count ||
                        sum_prep_ns != tls.active_mem_tombstone_iter_prepare_nanos ||
                        sum_cover_ns != tls.active_mem_tombstone_cover_lookup_nanos ||
                        sum_sst_iter_ns != tls.sst_iter_construct_nanos ||
                        sum_reseek != tls.scan_range_del_reseek_count ||
                        sum_boundary != tls.scan_boundary_advance_count ||
                        sum_child_next != tls.scan_range_del_child_next_count ||
                        sum_covered_skip != tls.scan_covered_skip_count)
                    {
                        std::cerr << "[FATAL AUDIT CONSISTENCY ERROR] Worker " << w << " Phase " << p
                                  << " TLS snapshot doesn't match sum of op deltas! sum_mat_ns="
                                  << sum_mat_ns << " vs tls=" << tls.range_tombstone_view_materialization_nanos << "\n";
                        return false;
                    }
                }
            }
#endif

            for (int p = 0; p < 3; ++p) {
                for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                    uint64_t sum_ops = 0, sum_endpoint_ns = 0, sum_mat_ops = 0, sum_lock_ops = 0, sum_both_ops = 0, sum_aff_ops = 0;
                    uint64_t sum_mat_ns = 0, sum_lock_ns = 0, sum_inval_cnt = 0;

                    for (int w = 0; w < num_workers_; ++w) {
                        const auto& ws = worker_op_stats[w][p][c];
                        // Hard check Level 1: Worker level affected_union identity
                        if (ws.materialization_or_lock_affected_reads !=
                            ws.materialized_op_count + ws.lock_contended_op_count - ws.both_materialized_and_lock_op_count)
                        {
                            std::cerr << "[FATAL AUDIT ERROR] Worker " << w << " Phase " << p << " Class "
                                      << AuditOpClassName(static_cast<AuditOpClass>(c))
                                      << " affected_union != mat + lock - both! (" << ws.materialization_or_lock_affected_reads
                                      << " != " << ws.materialized_op_count << " + " << ws.lock_contended_op_count
                                      << " - " << ws.both_materialized_and_lock_op_count << ")\n";
                            return false;
                        }

                        sum_ops += ws.op_count;
                        sum_endpoint_ns += ws.total_endpoint_nanos;
                        sum_mat_ops += ws.materialized_op_count;
                        sum_lock_ops += ws.lock_contended_op_count;
                        sum_both_ops += ws.both_materialized_and_lock_op_count;
                        sum_aff_ops += ws.materialization_or_lock_affected_reads;
                        sum_mat_ns += ws.view_materialization_nanos;
                        sum_lock_ns += ws.lock_wait_nanos;
                        sum_inval_cnt += ws.memtable_cache_invalidation_count;
                    }

                    const auto& ps = phase_summaries[p][c];
                    // Hard check Level 2: Phase summary affected_union identity
                    if (ps.materialization_or_lock_affected_reads !=
                        ps.materialized_op_count + ps.lock_contended_op_count - ps.both_materialized_and_lock_op_count)
                    {
                        std::cerr << "[FATAL AUDIT ERROR] Phase " << p << " Class "
                                  << AuditOpClassName(static_cast<AuditOpClass>(c))
                                  << " affected_union != mat + lock - both! (" << ps.materialization_or_lock_affected_reads
                                  << " != " << ps.materialized_op_count << " + " << ps.lock_contended_op_count
                                  << " - " << ps.both_materialized_and_lock_op_count << ")\n";
                        return false;
                    }

                    if (sum_ops != ps.op_count || sum_endpoint_ns != ps.total_endpoint_nanos ||
                        sum_mat_ops != ps.materialized_op_count || sum_lock_ops != ps.lock_contended_op_count ||
                        sum_both_ops != ps.both_materialized_and_lock_op_count ||
                        sum_aff_ops != ps.materialization_or_lock_affected_reads || sum_mat_ns != ps.view_materialization_nanos ||
                        sum_lock_ns != ps.lock_wait_nanos || sum_inval_cnt != ps.memtable_cache_invalidation_count)
                    {
                        std::cerr << "[FATAL AUDIT CONSISTENCY ERROR] sum(worker snapshots) != phase summary for Phase "
                                  << p << " Class " << AuditOpClassName(static_cast<AuditOpClass>(c)) << "!\n";
                        return false;
                    }
                }

                // Phase total checks
                if (phase_read_totals[p].materialization_or_lock_affected_reads !=
                    phase_read_totals[p].materialized_op_count + phase_read_totals[p].lock_contended_op_count - phase_read_totals[p].both_materialized_and_lock_op_count)
                {
                    std::cerr << "[FATAL AUDIT ERROR] Phase " << p << " TOTAL_READS affected_union identity violated!\n";
                    return false;
                }
            }

            for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                uint64_t sum_ops = 0, sum_endpoint_ns = 0, sum_mat_ops = 0, sum_lock_ops = 0, sum_both_ops = 0, sum_aff_ops = 0;
                uint64_t sum_mat_ns = 0, sum_lock_ns = 0, sum_inval_cnt = 0;

                for (int p = 0; p < 3; ++p) {
                    const auto& ps = phase_summaries[p][c];
                    sum_ops += ps.op_count;
                    sum_endpoint_ns += ps.total_endpoint_nanos;
                    sum_mat_ops += ps.materialized_op_count;
                    sum_lock_ops += ps.lock_contended_op_count;
                    sum_both_ops += ps.both_materialized_and_lock_op_count;
                    sum_aff_ops += ps.materialization_or_lock_affected_reads;
                    sum_mat_ns += ps.view_materialization_nanos;
                    sum_lock_ns += ps.lock_wait_nanos;
                    sum_inval_cnt += ps.memtable_cache_invalidation_count;
                }

                const auto& rs = run_summary[c];
                // Hard check Level 3: Run summary affected_union identity
                if (rs.materialization_or_lock_affected_reads !=
                    rs.materialized_op_count + rs.lock_contended_op_count - rs.both_materialized_and_lock_op_count)
                {
                    std::cerr << "[FATAL AUDIT ERROR] Run summary Class " << AuditOpClassName(static_cast<AuditOpClass>(c))
                              << " affected_union != mat + lock - both!\n";
                    return false;
                }

                if (sum_ops != rs.op_count || sum_endpoint_ns != rs.total_endpoint_nanos ||
                    sum_mat_ops != rs.materialized_op_count || sum_lock_ops != rs.lock_contended_op_count ||
                    sum_both_ops != rs.both_materialized_and_lock_op_count ||
                    sum_aff_ops != rs.materialization_or_lock_affected_reads || sum_mat_ns != rs.view_materialization_nanos ||
                    sum_lock_ns != rs.lock_wait_nanos || sum_inval_cnt != rs.memtable_cache_invalidation_count)
                {
                    std::cerr << "[FATAL AUDIT CONSISTENCY ERROR] sum(phase summaries) != run summary for Class "
                              << AuditOpClassName(static_cast<AuditOpClass>(c)) << "!\n";
                    return false;
                }
            }

            if (run_read_total.materialization_or_lock_affected_reads !=
                run_read_total.materialized_op_count + run_read_total.lock_contended_op_count - run_read_total.both_materialized_and_lock_op_count)
            {
                std::cerr << "[FATAL AUDIT ERROR] Run total TOTAL_READS affected_union identity violated!\n";
                return false;
            }

            std::cout << "[Audit Processing] PASSED All Summation, TLS, and 3-Level affected_union Identities with 100% precision.\n";

            // 4. Dump CSV 1: audit_worker_snapshots.csv
            std::string worker_snap_path = config_.audit_output_dir + "/audit_worker_snapshots.csv";
            std::ofstream fws(worker_snap_path);
            fws << "exp_id,rep,phase,worker_id,op_class,op_count,total_latency_ms,p50_us,p95_us,p99_us,"
                << "materialized_ops,materialization_rate_per_1k,lock_contended_ops,both_count,materialization_or_lock_affected_reads,"
                << "affected_p50_us,affected_p95_us,affected_p99_us,view_materialization_ms,lock_wait_ms,"
                << "materialization_and_lock_ratio,active_mem_prep_ms,active_mem_lookup_ms,imm_mem_prep_ms,"
                << "imm_mem_lookup_ms,active_mem_iter_construct_ms,imm_mem_iter_construct_ms,sst_iter_construct_ms,"
                << "reseek_count,boundary_advance_count,child_next_count,covered_skip_count,cache_invalidation_count\n";

            for (int p = 0; p < 3; ++p) {
                for (int w = 0; w < num_workers_; ++w) {
                    for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                        std::string prefix = config_.exp_id + "," + std::to_string(config_.rep) + "," +
                                             std::to_string(p) + "," + std::to_string(w) + "," +
                                             AuditOpClassName(static_cast<AuditOpClass>(c));
                        FormatStatsCsvLine(fws, prefix, worker_op_stats[w][p][c]);
                    }
                }
            }
            fws.close();
            std::cout << "[Audit Processing] Dumped " << worker_snap_path << "\n";

            // 5. Dump CSV 2: audit_phase_summary.csv
            std::string phase_sum_path = config_.audit_output_dir + "/audit_phase_summary.csv";
            std::ofstream fps(phase_sum_path);
            fps << "exp_id,rep,phase,op_class,op_count,total_latency_ms,p50_us,p95_us,p99_us,"
                << "materialized_ops,materialization_rate_per_1k,lock_contended_ops,both_count,materialization_or_lock_affected_reads,"
                << "affected_p50_us,affected_p95_us,affected_p99_us,view_materialization_ms,lock_wait_ms,"
                << "materialization_and_lock_ratio,active_mem_prep_ms,active_mem_lookup_ms,imm_mem_prep_ms,"
                << "imm_mem_lookup_ms,active_mem_iter_construct_ms,imm_mem_iter_construct_ms,sst_iter_construct_ms,"
                << "reseek_count,boundary_advance_count,child_next_count,covered_skip_count,cache_invalidation_count\n";

            for (int p = 0; p < 3; ++p) {
                for (size_t c = 0; c < static_cast<size_t>(AuditOpClass::kCount); ++c) {
                    std::string prefix = config_.exp_id + "," + std::to_string(config_.rep) + "," +
                                         std::to_string(p) + "," + AuditOpClassName(static_cast<AuditOpClass>(c));
                    FormatStatsCsvLine(fps, prefix, phase_summaries[p][c]);
                }
                std::string read_prefix = config_.exp_id + "," + std::to_string(config_.rep) + "," +
                                          std::to_string(p) + ",TOTAL_READS";
                FormatStatsCsvLine(fps, read_prefix, phase_read_totals[p]);

                std::string all_prefix = config_.exp_id + "," + std::to_string(config_.rep) + "," +
                                         std::to_string(p) + ",TOTAL_ALL";
                FormatStatsCsvLine(fps, all_prefix, phase_all_totals[p]);
            }
            fps.close();
            std::cout << "[Audit Processing] Dumped " << phase_sum_path << "\n";

            // 6. Dump CSV 3: audit_materialization_events.csv
            std::string mat_events_path = config_.audit_output_dir + "/audit_materialization_events.csv";
            std::ofstream fme(mat_events_path);
            fme << "run_id,rep,phase,worker,op_id,op_class,latency_us,materialized,lock_contended,both_materialized_and_lock_contended,affected_union,"
                << "active_mem_id,active_mem_tombstones,materialization_us,lock_wait_us,"
                << "active_mem_prep_us,active_mem_lookup_us,sst_iter_construct_us\n";

            uint64_t total_mat_events = 0;
            for (int w = 0; w < num_workers_; ++w) {
                for (const auto& ev : worker_mat_events[w]) {
                    fme << ev.run_id << "," << ev.rep << "," << ev.phase << "," << ev.worker << ","
                        << ev.op_id << "," << ev.op_class << ","
                        << std::fixed << std::setprecision(2) << ev.latency_us << ","
                        << ev.materialized << "," << ev.lock_contended << "," << ev.both_materialized_and_lock_contended << ","
                        << ev.affected_union << ","
                        << ev.active_mem_id << "," << ev.active_mem_tombstones << ","
                        << ev.materialization_us << "," << ev.lock_wait_us << ","
                        << ev.active_mem_prep_us << "," << ev.active_mem_lookup_us << ","
                        << ev.sst_iter_construct_us << "\n";
                    total_mat_events++;
                }
            }
            fme.close();
            std::cout << "[Audit Processing] Dumped " << mat_events_path << " (" << total_mat_events << " events)\n";

            // 7. Dump CSV 4: audit_run_summary.csv
            std::string run_sum_path = config_.audit_output_dir + "/audit_run_summary.csv";
            std::ofstream frs(run_sum_path);
            frs << "exp_id,rep,total_ops,total_reads,materialized_reads,overall_materialization_rate_per_1k,"
                << "lock_contended_reads,both_reads,materialization_or_lock_affected_reads,overall_read_latency_ms,total_materialization_ms,"
                << "total_lock_wait_ms,overall_materialization_and_lock_ratio,total_cache_invalidations,"
                << "phase_a_materialization_ms,phase_b_materialization_ms,phase_c_materialization_ms,"
                << "phase_a_materialization_or_lock_affected_reads,phase_b_materialization_or_lock_affected_reads,phase_c_materialization_or_lock_affected_reads,"
                << "phase_a_both_reads,phase_b_both_reads,phase_c_both_reads,"
                << "phase_a_invalidations,phase_b_invalidations,phase_c_invalidations,verification_status\n";

            double overall_mat_rate = (run_read_total.op_count > 0) ?
                (1000.0 * run_read_total.materialized_op_count / run_read_total.op_count) : 0.0;
            double overall_mat_lock_ratio = (run_read_total.total_endpoint_nanos > 0) ?
                (static_cast<double>(run_read_total.view_materialization_nanos + run_read_total.lock_wait_nanos) / run_read_total.total_endpoint_nanos) : 0.0;

            frs << config_.exp_id << "," << config_.rep << ","
                << run_all_total.op_count << ","
                << run_read_total.op_count << ","
                << run_read_total.materialized_op_count << ","
                << std::fixed << std::setprecision(4) << overall_mat_rate << ","
                << run_read_total.lock_contended_op_count << ","
                << run_read_total.both_materialized_and_lock_op_count << ","
                << run_read_total.materialization_or_lock_affected_reads << ","
                << (run_read_total.total_endpoint_nanos / 1e6) << ","
                << (run_read_total.view_materialization_nanos / 1e6) << ","
                << (run_read_total.lock_wait_nanos / 1e6) << ","
                << std::setprecision(6) << overall_mat_lock_ratio << ","
                << run_all_total.memtable_cache_invalidation_count << ","
                << std::setprecision(4) << (phase_read_totals[0].view_materialization_nanos / 1e6) << ","
                << (phase_read_totals[1].view_materialization_nanos / 1e6) << ","
                << (phase_read_totals[2].view_materialization_nanos / 1e6) << ","
                << phase_read_totals[0].materialization_or_lock_affected_reads << ","
                << phase_read_totals[1].materialization_or_lock_affected_reads << ","
                << phase_read_totals[2].materialization_or_lock_affected_reads << ","
                << phase_read_totals[0].both_materialized_and_lock_op_count << ","
                << phase_read_totals[1].both_materialized_and_lock_op_count << ","
                << phase_read_totals[2].both_materialized_and_lock_op_count << ","
                << phase_all_totals[0].memtable_cache_invalidation_count << ","
                << phase_all_totals[1].memtable_cache_invalidation_count << ","
                << phase_all_totals[2].memtable_cache_invalidation_count << ",PASS\n";
            frs.close();
            std::cout << "[Audit Processing] Dumped " << run_sum_path << "\n";

            // 8. Dump run_meta.json
            DumpRunMetaJson(config_.audit_output_dir, config_, config_file_path_);
        }

        // =========================================================================
        // Calculate Formal Baseline Metrics (Summary CSV & Phases CSV)
        // =========================================================================
        uint64_t total_trace_events = 0;
        uint64_t total_db_api_calls = 0;
        uint64_t total_logical_put_bytes = 0;
        uint64_t total_scan_limit_truncated = 0;

        ThreadLocalHistogram total_hist_get_live;
        ThreadLocalHistogram total_hist_get_del;
        ThreadLocalHistogram total_hist_scan;
        ThreadLocalHistogram total_hist_put;
        uint64_t total_scan_keys_found = 0;

        for (int w = 0; w < num_workers_; ++w) {
            for (int p = 0; p < 3; ++p) {
                const auto& ws = worker_phase_stats[w][p];
                total_trace_events += ws.completed_ops;
                total_db_api_calls += ws.db_api_calls;
                total_logical_put_bytes += ws.logical_put_bytes;
                total_scan_keys_found += ws.scan_keys_found;
                total_scan_limit_truncated += ws.scan_limit_truncated_count;

                total_hist_get_live.MergeFrom(ws.hist_get_live);
                total_hist_get_del.MergeFrom(ws.hist_get_del);
                total_hist_scan.MergeFrom(ws.hist_scan);
                total_hist_put.MergeFrom(ws.hist_put);
            }
        }

        double fg_trace_iops = (foreground_wallclock_sec > 0) ? (total_trace_events / foreground_wallclock_sec) : 0.0;
        double fg_db_api_iops = (foreground_wallclock_sec > 0) ? (total_db_api_calls / foreground_wallclock_sec) : 0.0;

        uint64_t fg_flush_cnt = event_listener_->GetForegroundFlushCount();
        uint64_t fg_flush_bytes = event_listener_->GetForegroundFlushBytes();
        uint64_t fg_comp_read_bytes = event_listener_->GetForegroundCompactionReadBytes();
        uint64_t fg_comp_write_bytes = event_listener_->GetForegroundCompactionWriteBytes();

        double fg_flush_mb = fg_flush_bytes / (1024.0 * 1024.0);
        double fg_comp_write_mb = fg_comp_write_bytes / (1024.0 * 1024.0);
        double fg_comp_read_mb = fg_comp_read_bytes / (1024.0 * 1024.0);

        double fwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_flush_bytes) / total_logical_put_bytes) : 0.0;
        double cwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_comp_write_bytes) / total_logical_put_bytes) : 0.0;
        double pwa_val_norm_fg = (total_logical_put_bytes > 0) ? (static_cast<double>(fg_flush_bytes + fg_comp_write_bytes) / total_logical_put_bytes) : 0.0;

        uint64_t total_exp_flush_cnt = event_listener_->GetTotalExperimentFlushCount();
        uint64_t total_exp_flush_bytes = event_listener_->GetTotalExperimentFlushBytes();
        uint64_t total_exp_comp_write_bytes = event_listener_->GetTotalExperimentCompactionWriteBytes();
        uint64_t controller_fg_flush_cnt = event_listener_->GetForegroundFlushCountByReason(rocksdb::FlushReason::kRangeTombstoneController);
        uint64_t controller_total_flush_cnt = event_listener_->GetTotalExperimentFlushCountByReason(rocksdb::FlushReason::kRangeTombstoneController);

        double total_exp_flush_mb = total_exp_flush_bytes / (1024.0 * 1024.0);
        double total_exp_comp_write_mb = total_exp_comp_write_bytes / (1024.0 * 1024.0);

        double fwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_flush_bytes) / total_logical_put_bytes) : 0.0;
        double cwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_comp_write_bytes) / total_logical_put_bytes) : 0.0;
        double pwa_val_norm_total = (total_logical_put_bytes > 0) ? (static_cast<double>(total_exp_flush_bytes + total_exp_comp_write_bytes) / total_logical_put_bytes) : 0.0;

        double scan_us_per_key = (total_scan_keys_found > 0) ? ((total_hist_scan.GetSumNs() / 1000.0) / total_scan_keys_found) : 0.0;

        double get_live_p50, get_live_p90, get_live_p95, get_live_p99, get_live_p999, dummy_mean, dummy_max;
        double get_del_p50, get_del_p90, get_del_p95, get_del_p99, get_del_p999;
        double scan_p50, scan_p90, scan_p95, scan_p99, scan_p999;
        double put_p50, put_p90, put_p95, put_p99, put_p999;

        total_hist_get_live.ComputeQuantiles(get_live_p50, get_live_p90, get_live_p95, get_live_p99, get_live_p999, dummy_mean, dummy_max);
        total_hist_get_del.ComputeQuantiles(get_del_p50, get_del_p90, get_del_p95, get_del_p99, get_del_p999, dummy_mean, dummy_max);
        total_hist_scan.ComputeQuantiles(scan_p50, scan_p90, scan_p95, scan_p99, scan_p999, dummy_mean, dummy_max);
        total_hist_put.ComputeQuantiles(put_p50, put_p90, put_p95, put_p99, put_p999, dummy_mean, dummy_max);

        uint64_t sst_size_total = 0;
        for (const auto& entry : std::filesystem::directory_iterator(config_.db_path)) {
            if (entry.path().extension() == ".sst") {
                sst_size_total += entry.file_size();
            }
        }
        double sst_mb = sst_size_total / (1024.0 * 1024.0);

        if (!config_.summary_csv.empty()) {
            bool summary_header = !std::filesystem::exists(config_.summary_csv);
            std::ofstream fsum(config_.summary_csv, std::ios::app);
            if (summary_header) {
                fsum << "exp_id,group_name,desc,threshold,range_tombstone_controller_enabled,range_tombstone_controller_observe_only,range_tombstone_controller_min_range_deletions,range_tombstone_controller_min_memtable_bytes,range_tombstone_controller_cooldown_micros,rtp_mc_mode,rtp_mc_windows,rtp_mc_seals,rtp_mc_conflicts,total_keys,value_size,foreground_wallclock_sec,sum_phase_active_sec,oracle_flush_wait_sec,"
                     << "fg_trace_iops,fg_db_api_iops,scan_us_per_key,scan_p99_us,get_live_p99_us,get_del_p99_us,put_p99_us,"
                     << "scan_limit_truncated_count,"
                     << "fg_flush_count,controller_fg_flush_count,fg_flush_engine_out_mb,fg_comp_read_mb,fg_comp_write_mb,fwa_val_norm_fg,cwa_val_norm_fg,pwa_val_norm_fg,"
                     << "total_exp_flush_count,controller_total_flush_count,total_exp_flush_engine_out_mb,total_exp_comp_write_mb,fwa_val_norm_total,cwa_val_norm_total,pwa_val_norm_total,sst_mb,"
                     << "db_live_keys,model_live_keys,sha256_hex,verification_status\n";
            }
            fsum << config_.exp_id << "," << config_.group_name << ",\"" << config_.desc << "\","
                 << config_.memtable_max_range_deletions << ","
                 << config_.enable_range_tombstone_controller << ","
                 << config_.range_tombstone_controller_observe_only << ","
                 << config_.range_tombstone_controller_min_range_deletions << ","
                 << config_.range_tombstone_controller_min_memtable_bytes << ","
                 << config_.range_tombstone_controller_cooldown_micros << ","
                 << config_.rtp_mc_mode << ","
                 << rtp_controller->GetTotalWindowsLogged() << ","
                 << rtp_controller->GetTotalSealsTriggered() << ","
                 << rtp_controller->GetTotalConflictsLogged() << ","
                 << config_.total_keys << "," << config_.value_size << ","
                 << std::fixed << std::setprecision(4) << foreground_wallclock_sec << ","
                 << sum_phase_active_sec << ","
                 << oracle_flush_wait_sec << ","
                 << fg_trace_iops << "," << fg_db_api_iops << ","
                 << scan_us_per_key << "," << scan_p99 << "," << get_live_p99 << "," << get_del_p99 << "," << put_p99 << ","
                 << total_scan_limit_truncated << ","
                 << fg_flush_cnt << "," << controller_fg_flush_cnt << "," << fg_flush_mb << "," << fg_comp_read_mb << "," << fg_comp_write_mb << ","
                 << fwa_val_norm_fg << "," << cwa_val_norm_fg << "," << pwa_val_norm_fg << ","
                 << total_exp_flush_cnt << "," << controller_total_flush_cnt << "," << total_exp_flush_mb << "," << total_exp_comp_write_mb << ","
                 << fwa_val_norm_total << "," << cwa_val_norm_total << "," << pwa_val_norm_total << "," << sst_mb << ","
                 << ver_report.db_live_keys << "," << ver_report.model_live_keys << ","
                 << ver_report.db_sha256_hex << ",PASS\n";
        }

        if (!config_.phases_csv.empty()) {
            bool phases_header = !std::filesystem::exists(config_.phases_csv);
            std::ofstream fphases(config_.phases_csv, std::ios::app);
            if (phases_header) {
                fphases << "exp_id,group_name,threshold,phase,elapsed_sec,completed_ops,true_phase_iops,"
                        << "scan_us_per_key,scan_p99_us,scan_intersect_p99_us,scan_non_intersect_p99_us,get_live_p99_us,get_del_p99_us,put_p99_us,scan_limit_truncated_count,"
                        << "num_l0_files,pending_compaction_bytes\n";
            }
            for (const auto& pr : phase_results) {
                fphases << config_.exp_id << "," << config_.group_name << "," << config_.memtable_max_range_deletions << ","
                        << "\"" << pr.phase_name << "\"," << std::fixed << std::setprecision(4) << pr.elapsed_sec << ","
                        << pr.completed_ops << "," << pr.true_phase_iops << ","
                        << pr.scan_us_per_key << "," << pr.scan_p99 << ","
                        << pr.scan_intersect_p99 << "," << pr.scan_non_intersect_p99 << ","
                        << pr.get_live_p99 << "," << pr.get_del_p99 << "," << pr.put_p99 << ","
                        << pr.scan_limit_truncated_count << ","
                        << pr.num_l0_files << "," << pr.pending_compaction_bytes << "\n";
            }
        }

        if (!config_.events_csv.empty()) {
            event_listener_->DumpEventsCsv(config_.events_csv, config_.exp_id);
        }

        std::cout << "[FormalDriver] All summaries and event logs successfully dumped.\n";
        return true;
    }

private:
    FormalConfig config_;
    std::string config_file_path_;
    int num_workers_;
    std::atomic<bool> experiment_failed_;
    std::vector<std::pair<uint64_t, uint64_t>> worker_ranges_;

    rocksdb::Options options_;
    std::shared_ptr<rocksdb::Statistics> db_stats_;
    std::shared_ptr<FormalEventListener> event_listener_;
    std::unique_ptr<rocksdb::DB> db_;

    std::vector<std::unique_ptr<WorkerStateModel>> worker_models_;
    std::vector<std::vector<std::vector<FormalTraceRecord>>> worker_traces_;
};

int main(int argc, char* argv[]) {
    std::string config_file = "";
    FormalConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
        else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
        else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
        else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
        else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
        else if (arg == "--events_csv" && i + 1 < argc) config.events_csv = argv[++i];
        else if (arg == "--phases_csv" && i + 1 < argc) config.phases_csv = argv[++i];
        else if (arg == "--windows_csv" && i + 1 < argc) config.windows_csv = argv[++i];
        else if (arg == "--actions_csv" && i + 1 < argc) config.actions_csv = argv[++i];
        else if (arg == "--audit_output_dir" && i + 1 < argc) config.audit_output_dir = argv[++i];
        else if (arg == "--rep" && i + 1 < argc) config.rep = std::stoi(argv[++i]);
        else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
        else if (arg == "--total_keys" && i + 1 < argc) config.total_keys = std::stoull(argv[++i]);
        else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoull(argv[++i]);
        else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--enable_range_tombstone_controller") config.enable_range_tombstone_controller = true;
        else if (arg == "--disable_range_tombstone_controller") config.enable_range_tombstone_controller = false;
        else if (arg == "--range_tombstone_controller_observe_only") config.range_tombstone_controller_observe_only = true;
        else if (arg == "--range_tombstone_controller_active") config.range_tombstone_controller_observe_only = false;
        else if (arg == "--range_tombstone_controller_min_range_deletions" && i + 1 < argc) config.range_tombstone_controller_min_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--range_tombstone_controller_min_memtable_bytes" && i + 1 < argc) config.range_tombstone_controller_min_memtable_bytes = std::stoull(argv[++i]);
        else if (arg == "--range_tombstone_controller_cooldown_micros" && i + 1 < argc) config.range_tombstone_controller_cooldown_micros = std::stoull(argv[++i]);
        else if (arg == "--rtp_mc_mode" && i + 1 < argc) config.rtp_mc_mode = argv[++i];
        else if (arg == "--control_epoch_ms" && i + 1 < argc) config.control_epoch_ms = std::stoull(argv[++i]);
        else if (arg == "--range_del_checkpoint" && i + 1 < argc) config.range_del_checkpoint = std::stoull(argv[++i]);
        else if (arg == "--scan_slo_us" && i + 1 < argc) config.scan_slo_us = std::stod(argv[++i]);
        else if (arg == "--getlive_slo_us" && i + 1 < argc) config.getlive_slo_us = std::stod(argv[++i]);
        else if (arg == "--put_slo_us" && i + 1 < argc) config.put_slo_us = std::stod(argv[++i]);
        else if (arg == "--write_buffer_size" && i + 1 < argc) config.write_buffer_size = std::stoull(argv[++i]);
        else if (arg == "--oracle_flush_after_phase_b") config.oracle_flush_after_phase_b = true;
    }

    if (!config_file.empty()) {
        if (!config.ParseIni(config_file)) {
            std::cerr << "Failed to parse config file: " << config_file << std::endl;
            return 1;
        }
        // Command line overrides
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--exp_id" && i + 1 < argc) config.exp_id = argv[++i];
            else if (arg == "--group_name" && i + 1 < argc) config.group_name = argv[++i];
            else if (arg == "--db_path" && i + 1 < argc) config.db_path = argv[++i];
            else if (arg == "--result_dir" && i + 1 < argc) config.result_dir = argv[++i];
            else if (arg == "--summary_csv" && i + 1 < argc) config.summary_csv = argv[++i];
            else if (arg == "--events_csv" && i + 1 < argc) config.events_csv = argv[++i];
            else if (arg == "--phases_csv" && i + 1 < argc) config.phases_csv = argv[++i];
            else if (arg == "--windows_csv" && i + 1 < argc) config.windows_csv = argv[++i];
            else if (arg == "--actions_csv" && i + 1 < argc) config.actions_csv = argv[++i];
            else if (arg == "--audit_output_dir" && i + 1 < argc) config.audit_output_dir = argv[++i];
            else if (arg == "--rep" && i + 1 < argc) config.rep = std::stoi(argv[++i]);
            else if (arg == "--trace_dir" && i + 1 < argc) config.trace_dir = argv[++i];
            else if (arg == "--total_keys" && i + 1 < argc) config.total_keys = std::stoull(argv[++i]);
            else if (arg == "--value_size" && i + 1 < argc) config.value_size = std::stoull(argv[++i]);
            else if (arg == "--memtable_max_range_deletions" && i + 1 < argc) config.memtable_max_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--enable_range_tombstone_controller") config.enable_range_tombstone_controller = true;
            else if (arg == "--disable_range_tombstone_controller") config.enable_range_tombstone_controller = false;
            else if (arg == "--range_tombstone_controller_observe_only") config.range_tombstone_controller_observe_only = true;
            else if (arg == "--range_tombstone_controller_active") config.range_tombstone_controller_observe_only = false;
            else if (arg == "--range_tombstone_controller_min_range_deletions" && i + 1 < argc) config.range_tombstone_controller_min_range_deletions = static_cast<uint32_t>(std::stoul(argv[++i]));
            else if (arg == "--range_tombstone_controller_min_memtable_bytes" && i + 1 < argc) config.range_tombstone_controller_min_memtable_bytes = std::stoull(argv[++i]);
            else if (arg == "--range_tombstone_controller_cooldown_micros" && i + 1 < argc) config.range_tombstone_controller_cooldown_micros = std::stoull(argv[++i]);
            else if (arg == "--rtp_mc_mode" && i + 1 < argc) config.rtp_mc_mode = argv[++i];
            else if (arg == "--control_epoch_ms" && i + 1 < argc) config.control_epoch_ms = std::stoull(argv[++i]);
            else if (arg == "--range_del_checkpoint" && i + 1 < argc) config.range_del_checkpoint = std::stoull(argv[++i]);
            else if (arg == "--scan_slo_us" && i + 1 < argc) config.scan_slo_us = std::stod(argv[++i]);
            else if (arg == "--getlive_slo_us" && i + 1 < argc) config.getlive_slo_us = std::stod(argv[++i]);
            else if (arg == "--put_slo_us" && i + 1 < argc) config.put_slo_us = std::stod(argv[++i]);
            else if (arg == "--write_buffer_size" && i + 1 < argc) config.write_buffer_size = std::stoull(argv[++i]);
            else if (arg == "--oracle_flush_after_phase_b") config.oracle_flush_after_phase_b = true;
        }
    }

    FormalDriver driver(config, config_file);
    if (!driver.LoadAllWorkerTraces()) return 1;
    if (!driver.InitializeDB()) return 1;
    if (!driver.PreloadDatabase()) return 1;
    if (!driver.ExecuteExperiment()) return 1;

    return 0;
}
