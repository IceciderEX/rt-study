#!/usr/bin/env python3
"""
AMTV M2d Deterministic Trace Generator and Static Auditor
Workload: m2d_getonly_dynamic_500k
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

LIVE_KEYS_PER_WORKER = 37500  # 60%
DEL_KEYS_PER_WORKER = 25000   # 40%
DEL_INTERVAL_SPAN = 10
DEL_INTERVALS_PER_WORKER = DEL_KEYS_PER_WORKER // DEL_INTERVAL_SPAN  # 2500

OPS_PER_WORKER_PHASE_A = 12500
OPS_PER_WORKER_PHASE_B = 12500
OPS_PER_WORKER_PHASE_C = 12500
TOTAL_OPS_PER_WORKER = 37500
TOTAL_OPS = TOTAL_OPS_PER_WORKER * NUM_WORKERS  # 300000

BASE_SEED = 90001

OP_GET = 0
OP_PUT = 2
OP_DELETE_RANGE = 3

RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = 24

def generate_worker_trace(worker_id, base_seed):
    rng = random.Random(base_seed + worker_id * 1000)
    
    worker_base_key = worker_id * KEYS_PER_WORKER
    live_start = worker_base_key
    live_end = worker_base_key + LIVE_KEYS_PER_WORKER  # exclusive
    del_start = live_end
    del_end = worker_base_key + KEYS_PER_WORKER        # exclusive
    
    records = []
    op_id = 0
    
    # ----------------------------------------------------
    # Phase A: 10,000 GetLive, 2,500 Put, 0 DeleteRange
    # ----------------------------------------------------
    phase_a_ops = [OP_GET] * 10000 + [OP_PUT] * 2500
    rng_a = random.Random(base_seed + worker_id * 1000 + 1)
    rng_a.shuffle(phase_a_ops)
    
    for op_type in phase_a_ops:
        key1 = rng_a.randint(live_start, live_end - 1)
        key2 = 0
        records.append({
            'phase_id': 0,
            'op_type': op_type,
            'scan_mode': 0,
            'flags': 0,
            'op_id': op_id,
            'key1': key1,
            'key2': key2
        })
        op_id += 1
        
    # ----------------------------------------------------
    # Phase B: 2,500 DeleteRange, 6,250 GetLive, 3,750 Put
    # DeleteRange at (local_index + worker_id) % 5 == 0
    # ----------------------------------------------------
    phase_b_non_del = [OP_GET] * 6250 + [OP_PUT] * 3750
    rng_b = random.Random(base_seed + worker_id * 1000 + 2)
    rng_b.shuffle(phase_b_non_del)
    
    del_idx = 0
    non_del_idx = 0
    for local_idx in range(OPS_PER_WORKER_PHASE_B):
        if (local_idx + worker_id) % 5 == 0:
            op_type = OP_DELETE_RANGE
            start_k = del_start + del_idx * DEL_INTERVAL_SPAN
            end_k = start_k + DEL_INTERVAL_SPAN
            del_idx += 1
            key1 = start_k
            key2 = end_k
        else:
            op_type = phase_b_non_del[non_del_idx]
            non_del_idx += 1
            key1 = rng_b.randint(live_start, live_end - 1)
            key2 = 0
            
        records.append({
            'phase_id': 1,
            'op_type': op_type,
            'scan_mode': 0,
            'flags': 0,
            'op_id': op_id,
            'key1': key1,
            'key2': key2
        })
        op_id += 1
        
    assert del_idx == DEL_INTERVALS_PER_WORKER, f"Worker {worker_id} del_idx mismatch: {del_idx}"
    assert non_del_idx == 10000, f"Worker {worker_id} non_del_idx mismatch: {non_del_idx}"
    
    # ----------------------------------------------------
    # Phase C: 11,250 GetLive, 1,250 Put, 0 DeleteRange
    # ----------------------------------------------------
    phase_c_ops = [OP_GET] * 11250 + [OP_PUT] * 1250
    rng_c = random.Random(base_seed + worker_id * 1000 + 3)
    rng_c.shuffle(phase_c_ops)
    
    for op_type in phase_c_ops:
        key1 = rng_c.randint(live_start, live_end - 1)
        key2 = 0
        records.append({
            'phase_id': 2,
            'op_type': op_type,
            'scan_mode': 0,
            'flags': 0,
            'op_id': op_id,
            'key1': key1,
            'key2': key2
        })
        op_id += 1
        
    assert len(records) == TOTAL_OPS_PER_WORKER
    return records

def write_binary_trace(filepath, records):
    with open(filepath, 'wb') as f:
        for r in records:
            buf = struct.pack(
                RECORD_FORMAT,
                r['phase_id'],
                r['op_type'],
                r['scan_mode'],
                r['flags'],
                r['op_id'],
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

def main():
    out_dir = "/home/wam/grad/s14-range-delete-study/traces/m2d_getonly_dynamic_500k"
    os.makedirs(out_dir, exist_ok=True)
    
    print("=" * 70)
    print("Generating AMTV M2d Deterministic Trace (m2d_getonly_dynamic_500k)...")
    print("=" * 70)
    
    all_workers_records = []
    trace_files = []
    
    for w in range(NUM_WORKERS):
        recs = generate_worker_trace(w, BASE_SEED)
        all_workers_records.append(recs)
        trace_path = os.path.join(out_dir, f"worker_{w}.trace")
        write_binary_trace(trace_path, recs)
        trace_files.append(trace_path)
        print(f"  [OK] Generated worker_{w}.trace ({len(recs)} records, {len(recs) * RECORD_SIZE} bytes)")
        
    # ----------------------------------------------------
    # Static Audit & Verifications
    # ----------------------------------------------------
    print("\nExecuting Comprehensive Static Audit...")
    
    # 1. Counts per phase across all workers
    counts_by_phase = {
        0: {OP_GET: 0, OP_PUT: 0, OP_DELETE_RANGE: 0},
        1: {OP_GET: 0, OP_PUT: 0, OP_DELETE_RANGE: 0},
        2: {OP_GET: 0, OP_PUT: 0, OP_DELETE_RANGE: 0}
    }
    
    for w, recs in enumerate(all_workers_records):
        for r in recs:
            counts_by_phase[r['phase_id']][r['op_type']] += 1
            
    # Phase A
    assert counts_by_phase[0][OP_GET] == 80000, f"Phase A GetLive: {counts_by_phase[0][OP_GET]}"
    assert counts_by_phase[0][OP_PUT] == 20000, f"Phase A Put: {counts_by_phase[0][OP_PUT]}"
    assert counts_by_phase[0][OP_DELETE_RANGE] == 0, f"Phase A Del: {counts_by_phase[0][OP_DELETE_RANGE]}"
    print("  [PASS] Phase A counts: 80,000 GetLive, 20,000 Put, 0 DeleteRange (Total: 100,000)")
    
    # Phase B
    assert counts_by_phase[1][OP_GET] == 50000, f"Phase B GetLive: {counts_by_phase[1][OP_GET]}"
    assert counts_by_phase[1][OP_PUT] == 30000, f"Phase B Put: {counts_by_phase[1][OP_PUT]}"
    assert counts_by_phase[1][OP_DELETE_RANGE] == 20000, f"Phase B Del: {counts_by_phase[1][OP_DELETE_RANGE]}"
    print("  [PASS] Phase B counts: 50,000 GetLive, 30,000 Put, 20,000 DeleteRange (Total: 100,000)")
    
    # Phase C
    assert counts_by_phase[2][OP_GET] == 90000, f"Phase C GetLive: {counts_by_phase[2][OP_GET]}"
    assert counts_by_phase[2][OP_PUT] == 10000, f"Phase C Put: {counts_by_phase[2][OP_PUT]}"
    assert counts_by_phase[2][OP_DELETE_RANGE] == 0, f"Phase C Del: {counts_by_phase[2][OP_DELETE_RANGE]}"
    print("  [PASS] Phase C counts: 90,000 GetLive, 10,000 Put, 0 DeleteRange (Total: 100,000)")
    
    # Grand Total
    total_get = sum(counts_by_phase[p][OP_GET] for p in (0, 1, 2))
    total_put = sum(counts_by_phase[p][OP_PUT] for p in (0, 1, 2))
    total_del = sum(counts_by_phase[p][OP_DELETE_RANGE] for p in (0, 1, 2))
    assert total_get == 220000
    assert total_put == 60000
    assert total_del == 20000
    assert total_get + total_put + total_del == 300000
    print(f"  [PASS] Grand Total: {total_get} GetLive, {total_put} Put, {total_del} DeleteRange = 300,000 ops")
    
    # 2. Interval disjointness and coverage check
    all_del_intervals = []
    covered_keys = set()
    for w in range(NUM_WORKERS):
        worker_del_recs = [r for r in all_workers_records[w] if r['op_type'] == OP_DELETE_RANGE]
        assert len(worker_del_recs) == DEL_INTERVALS_PER_WORKER
        for r in worker_del_recs:
            k1, k2 = r['key1'], r['key2']
            assert k2 - k1 == DEL_INTERVAL_SPAN, f"Interval span mismatch: {k1} to {k2}"
            all_del_intervals.append((k1, k2))
            for k in range(k1, k2):
                assert k not in covered_keys, f"Duplicate deleted key: {k}"
                covered_keys.add(k)
                
    assert len(all_del_intervals) == 20000
    assert len(covered_keys) == 200000
    print(f"  [PASS] Range Deletion Intervals: exactly 20,000 disjoint intervals, covering 200,000 keys (40.0%)")
    
    # 3. Disjointness between Live/Put keys and Deleted keys
    for w in range(NUM_WORKERS):
        worker_recs = all_workers_records[w]
        for r in worker_recs:
            if r['op_type'] in (OP_GET, OP_PUT):
                assert r['key1'] not in covered_keys, f"Get/Put key {r['key1']} overlaps deleted keys!"
    print("  [PASS] Live/Put Region Isolation: 0 GetLive or Put operations ever touch the 200,000 deleted keys")
    
    # 4. DeleteRange pulse dispersion check across Phase B
    del_pulses_by_step = [0] * OPS_PER_WORKER_PHASE_B
    for w in range(NUM_WORKERS):
        phase_b_recs = [r for r in all_workers_records[w] if r['phase_id'] == 1]
        for step, r in enumerate(phase_b_recs):
            if r['op_type'] == OP_DELETE_RANGE:
                del_pulses_by_step[step] += 1
                
    max_concurrent_dels = max(del_pulses_by_step)
    min_concurrent_dels = min(del_pulses_by_step)
    assert max_concurrent_dels <= 2, f"Too many concurrent deletes: {max_concurrent_dels}"
    assert min_concurrent_dels >= 1, f"Gap in delete schedule: {min_concurrent_dels}"
    print(f"  [PASS] DeleteRange Pulse Dispersion: at any step, exactly {min_concurrent_dels} to {max_concurrent_dels} workers execute DeleteRange (no 8-worker burst)")
    
    # ----------------------------------------------------
    # Hash / Projections computation
    # ----------------------------------------------------
    file_hashes = {}
    for w, path in enumerate(trace_files):
        file_hashes[f"worker_{w}.trace"] = sha256_file(path)
        
    # Read projection SHA-256
    read_h = hashlib.sha256()
    for w in range(NUM_WORKERS):
        for r in all_workers_records[w]:
            if r['op_type'] == OP_GET:
                read_h.update(struct.pack("<H B I Q", w, r['phase_id'], r['op_id'], r['key1']))
    read_proj_sha = read_h.hexdigest()
    
    # Write projection SHA-256
    write_h = hashlib.sha256()
    for w in range(NUM_WORKERS):
        for r in all_workers_records[w]:
            if r['op_type'] in (OP_PUT, OP_DELETE_RANGE):
                write_h.update(struct.pack("<H B I B Q Q", w, r['phase_id'], r['op_id'], r['op_type'], r['key1'], r['key2']))
    write_proj_sha = write_h.hexdigest()
    
    # Manifest creation
    manifest = {
        'workload': 'm2d_getonly_dynamic_500k',
        'total_keys_preloaded': TOTAL_KEYS,
        'value_size_bytes': VALUE_SIZE,
        'num_workers': NUM_WORKERS,
        'total_operations': TOTAL_OPS,
        'random_seed': BASE_SEED,
        'phases': {
            'phase_a': {'get_live': 80000, 'put': 20000, 'delete_range': 0, 'total': 100000},
            'phase_b': {'get_live': 50000, 'put': 30000, 'delete_range': 20000, 'total': 100000},
            'phase_c': {'get_live': 90000, 'put': 10000, 'delete_range': 0, 'total': 100000}
        },
        'key_coverage': {
            'total_keys': TOTAL_KEYS,
            'permanently_live_keys': TOTAL_KEYS - len(covered_keys),
            'deleted_keys': len(covered_keys),
            'deleted_intervals': len(all_del_intervals),
            'interval_span': DEL_INTERVAL_SPAN
        },
        'worker_file_sha256': file_hashes,
        'read_projection_sha256': read_proj_sha,
        'write_projection_sha256': write_proj_sha
    }
    
    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)
        
    manifest_sha = sha256_file(manifest_path)
    print("\nManifest & Projection Signatures:")
    print(f"  manifest_sha256:         {manifest_sha}")
    print(f"  read_projection_sha256:  {read_proj_sha}")
    print(f"  write_projection_sha256: {write_proj_sha}")
    for fname, fhash in file_hashes.items():
        print(f"  {fname}: {fhash}")
        
    print("=" * 70)
    print("Trace generation and static audit 100% SUCCESSFUL!")
    print("=" * 70)

if __name__ == '__main__':
    main()
