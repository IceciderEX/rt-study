#!/usr/bin/env python3
"""
AMTV M3a Static Trace Auditor
Reads binary trace files for Smoke and Rep 1-3, verifies:
1. Exact binary length (24 bytes * 37,500 = 900,000 bytes per worker)
2. Operation counts:
   - Phase A: 70k GetLive, 5k Scan-PlannedIntersect (exp 100), 5k Scan-NonIntersect (exp 100), 20k Put
   - Phase B: 40k GetLive, 5k Scan-Intersect (exp 50), 5k Scan-NonIntersect (exp 100), 30k Put, 20k DeleteRange
   - Phase C: 80k GetLive, 5k Scan-Intersect (exp 50), 5k Scan-NonIntersect (exp 100), 10k Put
3. Hard causality check on every Phase B Scan-Intersect:
   Assert that all 5 DeleteRanges covered by [start_key, end_key) have already been injected by that worker prior to the scan!
4. State model consistency: simulate 500,000 keys and verify 300,000 Live, 200,000 Deleted!
5. Invariance of DeleteRange geometry SHA across all seeds.
"""

import os
import sys
import struct
import hashlib
import json

TOTAL_KEYS = 500000
NUM_WORKERS = 8
KEYS_PER_WORKER = 62500
TOTAL_OPS_PER_WORKER = 37500
RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = 24

OP_GET = 0
OP_SCAN = 1
OP_PUT = 2
OP_DELETE_RANGE = 3

SCAN_NONE = 0
SCAN_PLANNED_INTERSECT = 1
SCAN_INTERSECT = 2
SCAN_NON_INTERSECT = 3


def audit_trace_package(trace_dir):
    manifest_path = os.path.join(trace_dir, "manifest.json")
    assert os.path.exists(manifest_path), f"Missing manifest: {manifest_path}"
    with open(manifest_path, "r") as f:
        manifest = json.load(f)

    # Track overall model
    # Key status: True = live, False = deleted
    key_status = [True] * TOTAL_KEYS

    total_ops = 0
    phase_counts = {
        0: {'get': 0, 'scan_planned_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        1: {'get': 0, 'scan_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        2: {'get': 0, 'scan_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0}
    }
    scan_vis_keys_total = 0

    del_geometry_ctx = hashlib.sha256()

    for w in range(NUM_WORKERS):
        trace_file = os.path.join(trace_dir, f"worker_{w}.trace")
        file_size = os.path.getsize(trace_file)
        assert file_size == TOTAL_OPS_PER_WORKER * RECORD_SIZE, \
            f"Worker {w} file size {file_size} != {TOTAL_OPS_PER_WORKER * RECORD_SIZE}"

        # Track completed DeleteRanges for this worker (cycle indices c in [0, 2499])
        worker_base_k = w * KEYS_PER_WORKER
        completed_dels = set()

        with open(trace_file, "rb") as f:
            for local_idx in range(TOTAL_OPS_PER_WORKER):
                buf = f.read(RECORD_SIZE)
                assert len(buf) == RECORD_SIZE
                phase_id, op_type, scan_type, exp_keys, rec_op_id, k1, k2 = struct.unpack(RECORD_FORMAT, buf)

                assert rec_op_id == local_idx, f"Op ID mismatch: expected {local_idx}, got {rec_op_id}"
                expected_phase = local_idx // 12500
                assert phase_id == expected_phase, f"Phase mismatch: expected {expected_phase}, got {phase_id}"

                total_ops += 1
                p_dict = phase_counts[phase_id]
                p_dict['total'] += 1

                # Verify key bounds are strictly within this worker's partition
                assert worker_base_k <= k1 < worker_base_k + KEYS_PER_WORKER, \
                    f"Worker {w} key1 {k1} out of bounds [{worker_base_k}, {worker_base_k + KEYS_PER_WORKER})"

                if op_type == OP_GET:
                    p_dict['get'] += 1
                    assert scan_type == SCAN_NONE
                    assert exp_keys == 0
                    assert k2 == 0
                    # Must be a permanently live key
                    local_k = k1 - worker_base_k
                    is_in_live_zone = (local_k >= 50000) or ((local_k % 20) >= 10)
                    assert is_in_live_zone, f"GetLive hit deleted region at key {k1}"

                elif op_type == OP_PUT:
                    p_dict['put'] += 1
                    assert scan_type == SCAN_NONE
                    assert exp_keys == 0
                    assert k2 == 0
                    # Must be a permanently live key
                    local_k = k1 - worker_base_k
                    is_in_live_zone = (local_k >= 50000) or ((local_k % 20) >= 10)
                    assert is_in_live_zone, f"Put hit deleted region at key {k1}"

                elif op_type == OP_DELETE_RANGE:
                    p_dict['del'] += 1
                    assert phase_id == 1, "DeleteRange must only occur in Phase B"
                    assert scan_type == SCAN_NONE
                    assert exp_keys == 0
                    assert k2 == k1 + 10, f"DeleteRange span must be 10, got [{k1}, {k2})"
                    local_k = k1 - worker_base_k
                    assert local_k < 50000 and (local_k % 20) == 0, f"DeleteRange misaligned at {k1}"
                    del_cycle = local_k // 20
                    assert del_cycle not in completed_dels, f"Duplicate DeleteRange {del_cycle}"
                    completed_dels.add(del_cycle)
                    # Mark in model
                    for k in range(k1, k2):
                        key_status[k] = False
                    del_geometry_ctx.update(f"{k1}:{k2}\n".encode('utf-8'))

                elif op_type == OP_SCAN:
                    assert k2 == k1 + 100, f"Scan span must be 100, got [{k1}, {k2})"
                    scan_vis_keys_total += exp_keys

                    if scan_type == SCAN_PLANNED_INTERSECT:
                        assert phase_id == 0, "Scan-PlannedIntersect only in Phase A"
                        assert exp_keys == 100, "Scan-PlannedIntersect must return 100 keys"
                        p_dict['scan_planned_intersect'] += 1
                        local_k = k1 - worker_base_k
                        assert local_k <= 50000 - 100 and (local_k % 20) == 0, f"Planned-Intersect misaligned: {k1}"

                    elif scan_type == SCAN_INTERSECT:
                        assert phase_id in (1, 2), "Scan-Intersect only in Phase B and C"
                        assert exp_keys == 50, "Scan-Intersect must return 50 keys"
                        p_dict['scan_intersect'] += 1
                        local_k = k1 - worker_base_k
                        assert local_k <= 50000 - 100 and (local_k % 20) == 0, f"Intersect misaligned: {k1}"
                        start_c = local_k // 20
                        # HARD CAUSALITY ASSERTION: All 5 DeleteRanges must have already been injected!
                        for c in range(start_c, start_c + 5):
                            assert c in completed_dels, \
                                f"CAUSALITY VIOLATION: Worker {w} op {local_idx}: DeleteRange {c} not completed yet!"

                    elif scan_type == SCAN_NON_INTERSECT:
                        assert exp_keys == 100, "Scan-NonIntersect must return 100 keys"
                        p_dict['scan_non_intersect'] += 1
                        local_k = k1 - worker_base_k
                        assert local_k >= 50000 and (local_k + 100) <= 62500, \
                            f"Non-Intersect out of permanent live zone: [{k1}, {k2})"
                    else:
                        assert False, f"Unknown scan_type: {scan_type}"
                else:
                    assert False, f"Unknown op_type: {op_type}"

        assert len(completed_dels) == 2500, f"Worker {w} only completed {len(completed_dels)} DeleteRanges"

    # Final Model State Check
    live_count = sum(1 for s in key_status if s)
    del_count = sum(1 for s in key_status if not s)
    assert live_count == 300000, f"Final live keys {live_count} != 300,000"
    assert del_count == 200000, f"Final deleted keys {del_count} != 200,000"

    # Verify counts
    assert total_ops == 300000
    assert phase_counts[0]['get'] == 70000
    assert phase_counts[0]['scan_planned_intersect'] == 5000
    assert phase_counts[0]['scan_non_intersect'] == 5000
    assert phase_counts[0]['put'] == 20000

    assert phase_counts[1]['get'] == 40000
    assert phase_counts[1]['scan_intersect'] == 5000
    assert phase_counts[1]['scan_non_intersect'] == 5000
    assert phase_counts[1]['put'] == 30000
    assert phase_counts[1]['del'] == 20000

    assert phase_counts[2]['get'] == 80000
    assert phase_counts[2]['scan_intersect'] == 5000
    assert phase_counts[2]['scan_non_intersect'] == 5000
    assert phase_counts[2]['put'] == 10000

    assert scan_vis_keys_total == 2500000

    calc_del_sha = del_geometry_ctx.hexdigest()
    assert calc_del_sha == manifest['delete_geometry_sha256'], \
        f"Del geometry SHA mismatch: {calc_del_sha} != {manifest['delete_geometry_sha256']}"

    return {
        'trace_dir': trace_dir,
        'seed': manifest['random_seed'],
        'trace_sha256': manifest['trace_sha256'],
        'read_projection_sha256': manifest['read_projection_sha256'],
        'write_projection_sha256': manifest['write_projection_sha256'],
        'delete_geometry_sha256': calc_del_sha,
        'total_ops': total_ops,
        'live_keys': live_count,
        'deleted_keys': del_count,
        'scan_visible_keys_total': scan_vis_keys_total,
        'pass_all_assertions': True
    }


def main():
    dirs = [
        "traces/m3a_smoke_seed90001",
        "traces/m3a_rep1_seed510001",
        "traces/m3a_rep2_seed520001",
        "traces/m3a_rep3_seed530001"
    ]
    results = []
    print("======================================================================")
    print("M3a Static Trace Auditor: Binary Layout, Causality & Invariants Audit")
    print("======================================================================")

    for d in dirs:
        res = audit_trace_package(d)
        results.append(res)
        print(f"[{res['seed']}] Audit PASS: {d}")
        print(f"  Trace SHA:   {res['trace_sha256']}")
        print(f"  Del Geo SHA: {res['delete_geometry_sha256']}")
        print(f"  Scan Keys:   {res['scan_visible_keys_total']} (Exp: 2,500,000)")
        print(f"  Final State: {res['live_keys']} Live, {res['deleted_keys']} Deleted")

    # Invariant Check across seeds
    del_shas = set(r['delete_geometry_sha256'] for r in results)
    assert len(del_shas) == 1, "Delete geometry SHA must be identical across all seeds!"
    print(f"\n[PASS] All 4 seeds have 100% identical delete geometry SHA: {list(del_shas)[0]}")

    out_json = "results/m3a_audit/m3a_trace_static_audit_summary.json"
    os.makedirs(os.path.dirname(out_json), exist_ok=True)
    with open(out_json, "w") as f:
        json.dump(results, f, indent=2)
    print(f"Summary written to {out_json}")
    print("======================================================================")


if __name__ == '__main__':
    main()
