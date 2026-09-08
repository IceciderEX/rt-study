#!/usr/bin/env python3
"""
AMTV M3a Deterministic Trace Generator and Static Auditor
Workload: m3a_mixed_read_dynamic_500k
Key space: 500,000 keys, 8 workers (62,500 keys/worker)
Operations: 300,000 (3 phases x 100,000)
"""

import os
import sys
import struct
import hashlib
import json
import random

TOTAL_KEYS = 500000
VALUE_SIZE = 256
NUM_WORKERS = 8
KEYS_PER_WORKER = TOTAL_KEYS // NUM_WORKERS  # 62500

INTERSECT_ZONE_KEYS = 50000
NON_INTERSECT_ZONE_KEYS = 12500
assert INTERSECT_ZONE_KEYS + NON_INTERSECT_ZONE_KEYS == KEYS_PER_WORKER

CYCLE_LEN = 20
DEL_SPAN = 10
LIVE_SPAN_IN_CYCLE = 10
CYCLES_PER_WORKER = INTERSECT_ZONE_KEYS // CYCLE_LEN  # 2500

OPS_PER_WORKER_PHASE = 12500
TOTAL_OPS_PER_WORKER = OPS_PER_WORKER_PHASE * 3  # 37500
TOTAL_OPS = TOTAL_OPS_PER_WORKER * NUM_WORKERS   # 300000

# Operation types
OP_GET = 0
OP_SCAN = 1
OP_PUT = 2
OP_DELETE_RANGE = 3

# Scan types
SCAN_NONE = 0
SCAN_PLANNED_INTERSECT = 1
SCAN_INTERSECT = 2
SCAN_NON_INTERSECT = 3

# Binary record format (24 bytes)
RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
assert RECORD_SIZE == 24, f"RECORD_SIZE must be 24, got {RECORD_SIZE}"


def build_worker_live_keys(worker_id):
    """Build list of all 37,500 permanently live keys for a worker."""
    base_k = worker_id * KEYS_PER_WORKER
    live_keys = []
    # 1. 2,500 cycles x 10 live keys = 25,000 live keys in Intersectable Zone
    for c in range(CYCLES_PER_WORKER):
        cycle_start = base_k + c * CYCLE_LEN
        live_start = cycle_start + DEL_SPAN
        for k in range(live_start, live_start + LIVE_SPAN_IN_CYCLE):
            live_keys.append(k)
    # 2. 12,500 live keys in Permanently Live Zone
    perm_start = base_k + INTERSECT_ZONE_KEYS
    for k in range(perm_start, perm_start + NON_INTERSECT_ZONE_KEYS):
        live_keys.append(k)
    assert len(live_keys) == 37500
    return live_keys


def generate_worker_trace(worker_id, base_seed):
    base_k = worker_id * KEYS_PER_WORKER
    live_keys = build_worker_live_keys(worker_id)
    
    records = []
    local_op_idx = 0

    # ----------------------------------------------------
    # Phase A: 8,750 GetLive, 625 Planned-Intersect Scan, 625 NonIntersect Scan, 2,500 Put
    # Total: 12,500 ops. All scans return 100 keys because no tombstones exist yet.
    # ----------------------------------------------------
    rng_a = random.Random(base_seed + worker_id * 1000 + 1)
    
    phase_a_ops = (
        [(OP_GET, SCAN_NONE)] * 8750 +
        [(OP_SCAN, SCAN_PLANNED_INTERSECT)] * 625 +
        [(OP_SCAN, SCAN_NON_INTERSECT)] * 625 +
        [(OP_PUT, SCAN_NONE)] * 2500
    )
    rng_a.shuffle(phase_a_ops)
    assert len(phase_a_ops) == OPS_PER_WORKER_PHASE

    for op_type, scan_type in phase_a_ops:
        if op_type == OP_GET or op_type == OP_PUT:
            k1 = rng_a.choice(live_keys)
            k2 = 0
            exp_keys = 0
        elif op_type == OP_SCAN:
            if scan_type == SCAN_PLANNED_INTERSECT:
                # 5 consecutive cycles in Intersectable Zone
                c = rng_a.randint(0, CYCLES_PER_WORKER - 5)
                k1 = base_k + c * CYCLE_LEN
                k2 = k1 + 100
                exp_keys = 100  # Phase A: no tombstones yet!
            elif scan_type == SCAN_NON_INTERSECT:
                # In Permanently Live Zone
                offset = rng_a.randint(0, NON_INTERSECT_ZONE_KEYS - 100)
                k1 = base_k + INTERSECT_ZONE_KEYS + offset
                k2 = k1 + 100
                exp_keys = 100
            else:
                assert False, f"Invalid Phase A scan type: {scan_type}"
        else:
            assert False, f"Invalid Phase A op_type: {op_type}"

        records.append({
            'phase_id': 0,
            'op_type': op_type,
            'scan_type': scan_type,
            'expected_visible_keys': exp_keys,
            'worker_local_op_index': local_op_idx,
            'key1': k1,
            'key2': k2
        })
        local_op_idx += 1

    # ----------------------------------------------------
    # Phase B: 2,500 DeleteRange, 5,000 GetLive, 3,750 Put, 625 Intersect Scan, 625 NonIntersect Scan
    # Total: 12,500 ops.
    # DeleteRange triggered at local_idx % 5 == (worker_id % 5).
    # Intersect Scan has causality: must only pick from completed DeleteRanges!
    # ----------------------------------------------------
    rng_b = random.Random(base_seed + worker_id * 1000 + 2)

    # 10,000 non-del ops
    phase_b_non_del = (
        [(OP_GET, SCAN_NONE)] * 5000 +
        [(OP_PUT, SCAN_NONE)] * 3750 +
        [(OP_SCAN, SCAN_INTERSECT)] * 625 +
        [(OP_SCAN, SCAN_NON_INTERSECT)] * 625
    )
    # To satisfy causality, reserve the first 50 non-del ops to be Non-Intersect Scan / Put / GetLive
    # so that at least 10 DeleteRanges are already injected before any Intersect Scan occurs.
    rng_b.shuffle(phase_b_non_del)
    
    # Ensure no SCAN_INTERSECT is in the first 50 non-del slots
    first_non_del = []
    rest_non_del = []
    for item in phase_b_non_del:
        if item[1] == SCAN_INTERSECT:
            rest_non_del.append(item)
        else:
            if len(first_non_del) < 50:
                first_non_del.append(item)
            else:
                rest_non_del.append(item)
    rng_b.shuffle(rest_non_del)
    phase_b_ordered_non_del = first_non_del + rest_non_del
    assert len(phase_b_ordered_non_del) == 10000

    del_idx = 0
    non_del_idx = 0
    completed_dels = set()

    for step in range(OPS_PER_WORKER_PHASE):
        is_del = (step % 5 == (worker_id % 5))
        if is_del:
            assert del_idx < CYCLES_PER_WORKER
            op_type = OP_DELETE_RANGE
            scan_type = SCAN_NONE
            start_k = base_k + del_idx * CYCLE_LEN
            end_k = start_k + DEL_SPAN
            k1 = start_k
            k2 = end_k
            exp_keys = 0
            completed_dels.add(del_idx)
            del_idx += 1
        else:
            op_type, scan_type = phase_b_ordered_non_del[non_del_idx]
            non_del_idx += 1
            if op_type == OP_GET or op_type == OP_PUT:
                k1 = rng_b.choice(live_keys)
                k2 = 0
                exp_keys = 0
            elif op_type == OP_SCAN:
                if scan_type == SCAN_INTERSECT:
                    # Hard causality check: must pick 5 consecutive cycles from completed_dels
                    max_c = del_idx - 5
                    assert max_c >= 0, f"Worker {worker_id} step {step}: not enough dels ({del_idx}) for Intersect Scan!"
                    c = rng_b.randint(0, max_c)
                    # Verify causality assertion
                    for d in range(c, c + 5):
                        assert d in completed_dels, f"Worker {worker_id} op {local_op_idx}: DeleteRange {d} not completed yet!"
                    k1 = base_k + c * CYCLE_LEN
                    k2 = k1 + 100
                    exp_keys = 50  # 50 deleted, 50 live
                elif scan_type == SCAN_NON_INTERSECT:
                    offset = rng_b.randint(0, NON_INTERSECT_ZONE_KEYS - 100)
                    k1 = base_k + INTERSECT_ZONE_KEYS + offset
                    k2 = k1 + 100
                    exp_keys = 100
                else:
                    assert False, f"Invalid Phase B scan type: {scan_type}"
            else:
                assert False, f"Invalid Phase B op_type: {op_type}"

        records.append({
            'phase_id': 1,
            'op_type': op_type,
            'scan_type': scan_type,
            'expected_visible_keys': exp_keys,
            'worker_local_op_index': local_op_idx,
            'key1': k1,
            'key2': k2
        })
        local_op_idx += 1

    assert del_idx == CYCLES_PER_WORKER, f"Worker {worker_id} del_idx mismatch: {del_idx}"
    assert non_del_idx == 10000, f"Worker {worker_id} non_del_idx mismatch: {non_del_idx}"

    # ----------------------------------------------------
    # Phase C: 10,000 GetLive, 625 Intersect Scan, 625 NonIntersect Scan, 1,250 Put
    # Total: 12,500 ops. All 2,500 DeleteRanges are already completed.
    # ----------------------------------------------------
    rng_c = random.Random(base_seed + worker_id * 1000 + 3)

    phase_c_ops = (
        [(OP_GET, SCAN_NONE)] * 10000 +
        [(OP_SCAN, SCAN_INTERSECT)] * 625 +
        [(OP_SCAN, SCAN_NON_INTERSECT)] * 625 +
        [(OP_PUT, SCAN_NONE)] * 1250
    )
    rng_c.shuffle(phase_c_ops)
    assert len(phase_c_ops) == OPS_PER_WORKER_PHASE

    for op_type, scan_type in phase_c_ops:
        if op_type == OP_GET or op_type == OP_PUT:
            k1 = rng_c.choice(live_keys)
            k2 = 0
            exp_keys = 0
        elif op_type == OP_SCAN:
            if scan_type == SCAN_INTERSECT:
                c = rng_c.randint(0, CYCLES_PER_WORKER - 5)
                k1 = base_k + c * CYCLE_LEN
                k2 = k1 + 100
                exp_keys = 50  # 50 deleted, 50 live
            elif scan_type == SCAN_NON_INTERSECT:
                offset = rng_c.randint(0, NON_INTERSECT_ZONE_KEYS - 100)
                k1 = base_k + INTERSECT_ZONE_KEYS + offset
                k2 = k1 + 100
                exp_keys = 100
            else:
                assert False, f"Invalid Phase C scan type: {scan_type}"
        else:
            assert False, f"Invalid Phase C op_type: {op_type}"

        records.append({
            'phase_id': 2,
            'op_type': op_type,
            'scan_type': scan_type,
            'expected_visible_keys': exp_keys,
            'worker_local_op_index': local_op_idx,
            'key1': k1,
            'key2': k2
        })
        local_op_idx += 1

    assert len(records) == TOTAL_OPS_PER_WORKER
    return records


def write_binary_trace(filepath, records):
    with open(filepath, 'wb') as f:
        for r in records:
            buf = struct.pack(
                RECORD_FORMAT,
                r['phase_id'],
                r['op_type'],
                r['scan_type'],
                r['expected_visible_keys'],
                r['worker_local_op_index'],
                r['key1'],
                r['key2']
            )
            assert len(buf) == RECORD_SIZE
            f.write(buf)


def sha256_file(filepath):
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def compute_projections(all_records):
    """
    Computes:
    1. Read projection SHA-256 (GetLive and Scan records: phase, op, scan_type, key1, key2, exp_keys)
    2. Write projection SHA-256 (Put and DeleteRange records: phase, op, key1, key2)
    3. DeleteRange geometry SHA-256 (DeleteRange records only: key1, key2)
    """
    read_ctx = hashlib.sha256()
    write_ctx = hashlib.sha256()
    del_ctx = hashlib.sha256()

    for w_records in all_records:
        for r in w_records:
            op = r['op_type']
            if op == OP_GET or op == OP_SCAN:
                s = f"{r['phase_id']}:{op}:{r['scan_type']}:{r['key1']}:{r['key2']}:{r['expected_visible_keys']}\n"
                read_ctx.update(s.encode('utf-8'))
            elif op == OP_PUT or op == OP_DELETE_RANGE:
                s = f"{r['phase_id']}:{op}:{r['key1']}:{r['key2']}\n"
                write_ctx.update(s.encode('utf-8'))
                if op == OP_DELETE_RANGE:
                    s_del = f"{r['key1']}:{r['key2']}\n"
                    del_ctx.update(s_del.encode('utf-8'))

    return read_ctx.hexdigest(), write_ctx.hexdigest(), del_ctx.hexdigest()


def generate_trace_package(seed, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    
    all_worker_records = []
    worker_files = []
    
    for w in range(NUM_WORKERS):
        records = generate_worker_trace(w, seed)
        all_worker_records.append(records)
        fn = f"worker_{w}.trace"
        fp = os.path.join(out_dir, fn)
        write_binary_trace(fp, records)
        worker_files.append(fp)

    # Compute overall trace file hash by concatenating hashes
    combined_hash = hashlib.sha256()
    for fp in worker_files:
        combined_hash.update(sha256_file(fp).encode('utf-8'))
    trace_sha256 = combined_hash.hexdigest()

    read_sha, write_sha, del_sha = compute_projections(all_worker_records)

    # Audit operation counts per phase
    counts = {
        'phase_a': {'get': 0, 'scan_planned_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        'phase_b': {'get': 0, 'scan_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        'phase_c': {'get': 0, 'scan_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        'total': {'get': 0, 'scan_planned_intersect': 0, 'scan_intersect': 0, 'scan_non_intersect': 0, 'put': 0, 'del': 0, 'total': 0},
        'expected_visible_keys': {
            'phase_a_scan_planned_intersect': 0,
            'phase_a_scan_non_intersect': 0,
            'phase_b_scan_intersect': 0,
            'phase_b_scan_non_intersect': 0,
            'phase_c_scan_intersect': 0,
            'phase_c_scan_non_intersect': 0,
            'total_scan_visible_keys': 0
        }
    }

    for w_records in all_worker_records:
        for r in w_records:
            p = r['phase_id']
            op = r['op_type']
            st = r['scan_type']
            exp_k = r['expected_visible_keys']

            p_key = 'phase_a' if p == 0 else ('phase_b' if p == 1 else 'phase_c')
            counts[p_key]['total'] += 1
            counts['total']['total'] += 1

            if op == OP_GET:
                counts[p_key]['get'] += 1
                counts['total']['get'] += 1
            elif op == OP_PUT:
                counts[p_key]['put'] += 1
                counts['total']['put'] += 1
            elif op == OP_DELETE_RANGE:
                counts[p_key]['del'] += 1
                counts['total']['del'] += 1
            elif op == OP_SCAN:
                counts['expected_visible_keys']['total_scan_visible_keys'] += exp_k
                if st == SCAN_PLANNED_INTERSECT:
                    counts[p_key]['scan_planned_intersect'] += 1
                    counts['total']['scan_planned_intersect'] += 1
                    counts['expected_visible_keys']['phase_a_scan_planned_intersect'] += exp_k
                elif st == SCAN_INTERSECT:
                    counts[p_key]['scan_intersect'] += 1
                    counts['total']['scan_intersect'] += 1
                    if p == 1:
                        counts['expected_visible_keys']['phase_b_scan_intersect'] += exp_k
                    else:
                        counts['expected_visible_keys']['phase_c_scan_intersect'] += exp_k
                elif st == SCAN_NON_INTERSECT:
                    counts[p_key]['scan_non_intersect'] += 1
                    counts['total']['scan_non_intersect'] += 1
                    if p == 0:
                        counts['expected_visible_keys']['phase_a_scan_non_intersect'] += exp_k
                    elif p == 1:
                        counts['expected_visible_keys']['phase_b_scan_non_intersect'] += exp_k
                    else:
                        counts['expected_visible_keys']['phase_c_scan_non_intersect'] += exp_k

    manifest = {
        'workload': 'm3a_mixed_read_dynamic_500k',
        'random_seed': seed,
        'total_keys': TOTAL_KEYS,
        'num_workers': NUM_WORKERS,
        'keys_per_worker': KEYS_PER_WORKER,
        'records_per_worker': TOTAL_OPS_PER_WORKER,
        'total_operations': TOTAL_OPS,
        'record_format': RECORD_FORMAT,
        'record_size_bytes': RECORD_SIZE,
        'trace_sha256': trace_sha256,
        'read_projection_sha256': read_sha,
        'write_projection_sha256': write_sha,
        'delete_geometry_sha256': del_sha,
        'operation_counts': counts,
        'audit_gates': {
            'phase_a_planned_intersect_scans': counts['phase_a']['scan_planned_intersect'],
            'phase_b_intersect_scans': counts['phase_b']['scan_intersect'],
            'phase_c_intersect_scans': counts['phase_c']['scan_intersect'],
            'total_non_intersect_scans': counts['total']['scan_non_intersect'],
            'total_delete_ranges': counts['total']['del'],
            'total_puts': counts['total']['put'],
            'total_get_lives': counts['total']['get'],
            'total_scan_visible_keys': counts['expected_visible_keys']['total_scan_visible_keys']
        }
    }

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"[Seed {seed}] Trace generation complete -> {out_dir}")
    print(f"  Trace SHA:            {trace_sha256}")
    print(f"  Read Projection SHA:  {read_sha}")
    print(f"  Write Projection SHA: {write_sha}")
    print(f"  Delete Geometry SHA:  {del_sha}")
    print(f"  Planned-Intersect:    {counts['phase_a']['scan_planned_intersect']} (exp 100 keys)")
    print(f"  Phase B Intersect:    {counts['phase_b']['scan_intersect']} (exp 50 keys)")
    print(f"  Phase C Intersect:    {counts['phase_c']['scan_intersect']} (exp 50 keys)")
    print(f"  Total NonIntersect:   {counts['total']['scan_non_intersect']} (exp 100 keys)")
    print(f"  Total Scan Vis Keys:  {counts['expected_visible_keys']['total_scan_visible_keys']}")

    return manifest


def main():
    SEEDS = {
        'smoke': (90001, "traces/m3a_smoke_seed90001"),
        'rep1':  (510001, "traces/m3a_rep1_seed510001"),
        'rep2':  (520001, "traces/m3a_rep2_seed520001"),
        'rep3':  (530001, "traces/m3a_rep3_seed530001")
    }

    print("======================================================================")
    print("AMTV M3a Trace Generator: Pre-registered Scan Semantics & Hard Causality")
    print("======================================================================")

    manifests = {}
    for name, (seed, out_dir) in SEEDS.items():
        manifests[name] = generate_trace_package(seed, out_dir)

    print("\n[Audit Gate Check]")
    del_shas = set(m['delete_geometry_sha256'] for m in manifests.values())
    assert len(del_shas) == 1, f"Delete geometry SHA must be identical across all seeds, got: {del_shas}"
    print(f"  [PASS] Delete Geometry SHA is invariant across all seeds: {list(del_shas)[0]}")

    for name, m in manifests.items():
        gates = m['audit_gates']
        assert gates['phase_a_planned_intersect_scans'] == 5000
        assert gates['phase_b_intersect_scans'] == 5000
        assert gates['phase_c_intersect_scans'] == 5000
        assert gates['total_non_intersect_scans'] == 15000
        assert gates['total_delete_ranges'] == 20000
        assert gates['total_puts'] == 60000
        assert gates['total_get_lives'] == 190000
        # Phase A: 5000*100 + 5000*100 = 1,000,000
        # Phase B: 5000*50 + 5000*100 = 750,000
        # Phase C: 5000*50 + 5000*100 = 750,000
        # Total = 2,500,000
        assert gates['total_scan_visible_keys'] == 2500000
        print(f"  [PASS] All gate assertions passed for {name}")

    print("\nAll 4 trace sets generated and statically verified successfully!")
    print("======================================================================")


if __name__ == '__main__':
    main()
