// Copyright (c) 2026-present. All rights reserved.
// Unit test for M3a multi-threaded audit aggregation (TakeAndReset) across 8 workers and 3 phases.

#include <iostream>
#include <vector>
#include <thread>
#include <barrier>
#include <cassert>
#include <numeric>

#include "db/read_path_audit.h"

using namespace rocksdb;

struct WorkerPhaseAuditSnapshot {
    ReadPathAuditStats stats[static_cast<size_t>(AuditOpType::kMax)];
};

int main() {
    std::cout << "======================================================================\n";
    std::cout << "Starting M3a Audit Aggregator Unit Test (8 workers, 7 op tags, 3 phases)\n";
    std::cout << "======================================================================\n";

    SetReadPathAuditEnabled(true);
    assert(IsReadPathAuditEnabled());

    const int kNumWorkers = 8;
    const int kNumPhases = 3;

    WorkerPhaseAuditSnapshot snapshots[kNumWorkers][kNumPhases];
    std::barrier sync_barrier(kNumWorkers + 1);

    std::vector<std::thread> workers;
    workers.reserve(kNumWorkers);

    for (int w = 0; w < kNumWorkers; ++w) {
        workers.emplace_back([&, w]() {
            for (int phase = 0; phase < kNumPhases; ++phase) {
                // Ensure starting from clean slate
                ResetAllAuditStats();

                // 1. Simulate GetLive ops
                {
                    AuditOpScope scope(AuditOpType::kGetLive);
                    uint64_t count = (w + 1) * 100 + (phase + 1) * 10;
                    AUDIT_COUNT_ADD(fragment_build_lock_attempt_count, count);
                    AUDIT_COUNT_ADD(fragment_build_lock_contended_count, count / 2);
                }
                assert(GetCurrentAuditOpType() == AuditOpType::kNone);

                // 2. Simulate ScanPlannedIntersect ops
                {
                    AuditOpScope scope(AuditOpType::kScanPlannedIntersect);
                    uint64_t count = (w + 1) * 50 + (phase + 1) * 5;
                    AUDIT_COUNT_ADD(scan_boundary_advance_count, count);
                }
                assert(GetCurrentAuditOpType() == AuditOpType::kNone);

                // 3. Simulate ScanIntersect ops
                {
                    AuditOpScope scope(AuditOpType::kScanIntersect);
                    uint64_t count = (w + 1) * 40 + (phase + 1) * 4;
                    AUDIT_COUNT_ADD(scan_range_del_reseek_count, count);
                    AUDIT_COUNT_ADD(range_tombstone_view_materialization_count, count / 4);
                }
                assert(GetCurrentAuditOpType() == AuditOpType::kNone);

                // 4. Simulate ScanNonIntersect ops
                {
                    AuditOpScope scope(AuditOpType::kScanNonIntersect);
                    uint64_t count = (w + 1) * 30 + (phase + 1) * 3;
                    AUDIT_COUNT_ADD(scan_covered_skip_count, count);
                }
                assert(GetCurrentAuditOpType() == AuditOpType::kNone);

                // 5. TakeAndReset: snapshot into worker exclusive slot
                for (size_t op = 0; op < static_cast<size_t>(AuditOpType::kMax); ++op) {
                    snapshots[w][phase].stats[op] = *GetReadPathAuditStats(static_cast<AuditOpType>(op));
                }
                ResetAllAuditStats();

                // Verify that thread-local stats are completely 0 after TakeAndReset
                for (size_t op = 0; op < static_cast<size_t>(AuditOpType::kMax); ++op) {
                    const auto& s = *GetReadPathAuditStats(static_cast<AuditOpType>(op));
                    assert(s.fragment_build_lock_attempt_count == 0);
                    assert(s.scan_range_del_reseek_count == 0);
                    assert(s.scan_boundary_advance_count == 0);
                    assert(s.scan_covered_skip_count == 0);
                }

                sync_barrier.arrive_and_wait(); // Worker wait for main thread to aggregate
                sync_barrier.arrive_and_wait(); // Wait for main thread to finish verification
            }
        });
    }

    // Main thread aggregation loop
    for (int phase = 0; phase < kNumPhases; ++phase) {
        sync_barrier.arrive_and_wait(); // Wait for all workers to finish phase

        ReadPathAuditStats phase_agg[static_cast<size_t>(AuditOpType::kMax)];
        for (int w = 0; w < kNumWorkers; ++w) {
            for (size_t op = 0; op < static_cast<size_t>(AuditOpType::kMax); ++op) {
                phase_agg[op].MergeFrom(snapshots[w][phase].stats[op]);
            }
        }

        // Verify mathematical exactness
        // Worker weights: sum(w+1) for w=0..7 = 1+2+3+4+5+6+7+8 = 36.
        uint64_t sum_w = 36;
        uint64_t p_factor = (phase + 1);

        // GetLive:
        uint64_t expected_get_attempts = sum_w * 100 + kNumWorkers * p_factor * 10;
        uint64_t expected_get_contended = expected_get_attempts / 2;
        assert(phase_agg[static_cast<size_t>(AuditOpType::kGetLive)].fragment_build_lock_attempt_count == expected_get_attempts);
        assert(phase_agg[static_cast<size_t>(AuditOpType::kGetLive)].fragment_build_lock_contended_count == expected_get_contended);

        // ScanPlannedIntersect:
        uint64_t expected_planned_advance = sum_w * 50 + kNumWorkers * p_factor * 5;
        assert(phase_agg[static_cast<size_t>(AuditOpType::kScanPlannedIntersect)].scan_boundary_advance_count == expected_planned_advance);

        // ScanIntersect:
        uint64_t expected_intersect_reseek = sum_w * 40 + kNumWorkers * p_factor * 4;
        uint64_t expected_intersect_mat = expected_intersect_reseek / 4;
        assert(phase_agg[static_cast<size_t>(AuditOpType::kScanIntersect)].scan_range_del_reseek_count == expected_intersect_reseek);
        assert(phase_agg[static_cast<size_t>(AuditOpType::kScanIntersect)].range_tombstone_view_materialization_count == expected_intersect_mat);

        // ScanNonIntersect:
        uint64_t expected_nonintersect_skip = sum_w * 30 + kNumWorkers * p_factor * 3;
        assert(phase_agg[static_cast<size_t>(AuditOpType::kScanNonIntersect)].scan_covered_skip_count == expected_nonintersect_skip);

        // Ensure other buckets have zero leak
        assert(phase_agg[static_cast<size_t>(AuditOpType::kPut)].fragment_build_lock_attempt_count == 0);
        assert(phase_agg[static_cast<size_t>(AuditOpType::kDeleteRange)].fragment_build_lock_attempt_count == 0);
        assert(phase_agg[static_cast<size_t>(AuditOpType::kNone)].fragment_build_lock_attempt_count == 0);

        std::cout << "  [PASS] Phase " << phase << " Aggregation: GetAttempts=" << expected_get_attempts
                  << ", IntersectReseeks=" << expected_intersect_reseek
                  << ", NonIntersectSkips=" << expected_nonintersect_skip << "\n";

        sync_barrier.arrive_and_wait(); // Release workers for next phase
    }

    for (auto& t : workers) {
        t.join();
    }

    std::cout << "[PASS] All 8 workers, 7 op tags, 3 phases aggregation verified exactly with zero duplicate or missing counts.\n";
    std::cout << "======================================================================\n";
    return 0;
}
