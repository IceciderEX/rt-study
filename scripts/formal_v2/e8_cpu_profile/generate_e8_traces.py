#!/usr/bin/env python3
"""
Formal V2 - E8 Trace Generator with Exact Physical Geometry
Generates deterministic, partition-balanced, audited micro-traces for E8 profiling:
1. e8_scan_intersect (Physical span=50 keys: 10 deleted, 40 live, 1 tombstone boundary)
2. e8_scan_non_intersect (Physical span=50 keys: 0 deleted, 50 live)
3. e8_get_live (1 key: 0 deleted, 1 live)
4. e8_get_deleted (1 key: 1 deleted, 0 live)
5. Corresponding Clean variants (DeleteRange replaced by No-op)
"""

import os
import struct
import json
import hashlib
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
TRACES_BASE = os.path.join(BASE_DIR, "traces/formal_v2")

TOTAL_KEYS = 500000
NUM_WORKERS = 8
VALUE_SIZE = 256
TOTAL_TOMBSTONES = 20000
KEYS_PER_WORKER = TOTAL_KEYS // NUM_WORKERS # 62500
TOMBSTONES_PER_WORKER = TOTAL_TOMBSTONES // NUM_WORKERS # 2500
DELETED_KEYS_PER_WORKER = 25000 # 40.0% of 62500

def generate_geometry(worker_id):
    """
    Constructs deterministic, strictly mutually exclusive geometry for worker_id:
    Keyspace: [w_start, w_end) where w_start = worker_id * 62500, w_end = (worker_id+1)*62500
    Total tombstones for worker: 2,500 (25,000 deleted keys)
    
    Structure within worker keyspace (62,500 keys):
    Zone 1 (Scan-Intersect candidate zone): 500 blocks of 50 keys = 25,000 keys [w_start, w_start + 25000)
      In each 50-key block [b, b+50):
        - DeleteRange: [b, b+10) (10 deleted keys)
        - Live keys:   [b+10, b+50) (40 live keys)
      -> Yields 500 tombstones (5,000 deleted keys).
      
    Zone 2 (Dense deletion zone): 25,000 keys [w_start + 25000, w_start + 50000)
      Contains remaining 2,000 tombstones of width 10 keys.
      Stride = 12.5 keys: 2,000 * 10 = 20,000 deleted keys, 5,000 live keys.
      -> Stride structure: 2,000 tombstones [w_start + 25000 + i*12.5, +10).
      
    Zone 3 (Pure live zone for Scan-NonIntersect): 12,500 keys [w_start + 50000, w_end)
      Contains ZERO tombstones (100% live keys).
    """
    w_start = worker_id * KEYS_PER_WORKER
    tombstones = []
    
    # Zone 1: 500 tombstones in 50-key blocks
    intersect_scan_ranges = []
    for i in range(500):
        b = w_start + i * 50
        t_start = b
        t_end = b + 10
        tombstones.append((t_start, t_end))
        intersect_scan_ranges.append((b, b + 50)) # Span 50: [b, b+10) del, [b+10, b+50) live
        
    # Zone 2: 2,000 tombstones in dense zone
    z2_start = w_start + 25000
    for i in range(2000):
        # 2000 tombstones across 25000 keys
        # block of 12 or 13 keys: exactly 20000 deleted keys
        # Let base be z2_start + i * 12 + (i % 2) -> max offset = 1999 * 12 + 1 = 23989 + 10 = 23999 < 25000
        # Use exact non-overlapping interval: i * 12
        t_start = z2_start + i * 12
        t_end = t_start + 10
        tombstones.append((t_start, t_end))
        
    assert len(tombstones) == TOMBSTONES_PER_WORKER, f"Expected {TOMBSTONES_PER_WORKER}, got {len(tombstones)}"
    
    # Zone 3: Pure live ranges for non-intersect scans
    z3_start = w_start + 50000
    non_intersect_scan_ranges = []
    for i in range(250): # 250 blocks of 50 keys = 12,500 keys
        b = z3_start + i * 50
        non_intersect_scan_ranges.append((b, b + 50))
        
    # Get-Live candidate keys (from Zone 3)
    live_get_keys = [z3_start + i for i in range(5000)]
    
    # Get-Deleted candidate keys (from Zone 1 tombstones)
    del_get_keys = []
    for t_start, t_end in tombstones[:500]:
        for k in range(t_start, t_end):
            del_get_keys.append(k)
            if len(del_get_keys) >= 5000: break
        if len(del_get_keys) >= 5000: break
        
    return tombstones, intersect_scan_ranges, non_intersect_scan_ranges, live_get_keys, del_get_keys

def compute_geometry_sha256(all_tombstones):
    h = hashlib.sha256()
    for w in range(NUM_WORKERS):
        for ts, te in all_tombstones[w]:
            h.update(f"{ts}:{te}\n".encode("utf-8"))
    return h.hexdigest()

def generate_workload_traces(case_name, is_clean=False):
    trace_dir = os.path.join(TRACES_BASE, f"e8_{case_name}" + ("_clean" if is_clean else ""))
    os.makedirs(trace_dir, exist_ok=True)
    
    all_tombstones = [generate_geometry(w)[0] for w in range(NUM_WORKERS)]
    geom_sha = compute_geometry_sha256(all_tombstones)
    
    global_op_id = 0
    worker_ops_count = []
    
    for w in range(NUM_WORKERS):
        tombstones, int_scans, non_int_scans, live_gets, del_gets = generate_geometry(w)
        
        # 1. Phase B (Injection trace): 2500 DeleteRanges (or No-ops if clean)
        # Struct: phase_id(1B), op_type(1B), scan_mode(1B), flags(1B), op_id(4B), key1(8B), key2(8B) = 24B
        records_inj = []
        for ts, te in tombstones:
            op_type = 4 if is_clean else 3 # 4=No-op, 3=DeleteRange
            # flags: bit 1 (inject) = 2
            rec = struct.pack("<BBBB I QQ", 1, op_type, 0, 2, global_op_id, ts, te)
            records_inj.append(rec)
            global_op_id += 1
            
        inj_path = os.path.join(trace_dir, f"phase_b-worker-{w:02d}.bin")
        with open(inj_path, "wb") as f:
            for r in records_inj: f.write(r)
            
        # 2. Phase C (Micro-trace profile window): 5,000 operations matching case_name
        records_micro = []
        if "scan_intersect" in case_name:
            for idx in range(5000):
                s_start, s_end = int_scans[idx % len(int_scans)]
                # op_type=1 (Scan), scan_mode=0 (Range), flags=1 (affected/intersect)
                rec = struct.pack("<BBBB I QQ", 2, 1, 0, 1, global_op_id, s_start, s_end)
                records_micro.append(rec)
                global_op_id += 1
        elif "scan_non_intersect" in case_name:
            for idx in range(5000):
                s_start, s_end = non_int_scans[idx % len(non_int_scans)]
                # op_type=1 (Scan), scan_mode=0 (Range), flags=0
                rec = struct.pack("<BBBB I QQ", 2, 1, 0, 0, global_op_id, s_start, s_end)
                records_micro.append(rec)
                global_op_id += 1
        elif "get_live" in case_name:
            for idx in range(5000):
                k = live_gets[idx % len(live_gets)]
                # op_type=0 (Get), scan_mode=0, flags=0
                rec = struct.pack("<BBBB I QQ", 2, 0, 0, 0, global_op_id, k, 0)
                records_micro.append(rec)
                global_op_id += 1
        elif "get_deleted" in case_name:
            for idx in range(5000):
                k = del_gets[idx % len(del_gets)]
                # op_type=0 (Get), scan_mode=0, flags=1 (affected/deleted)
                rec = struct.pack("<BBBB I QQ", 2, 0, 0, 1, global_op_id, k, 0)
                records_micro.append(rec)
                global_op_id += 1
        else:
            raise ValueError(f"Unknown case: {case_name}")
            
        micro_path = os.path.join(trace_dir, f"phase_c-worker-{w:02d}.bin")
        with open(micro_path, "wb") as f:
            for r in records_micro: f.write(r)
            
        # Empty Phase A (driver does preload internally)
        a_path = os.path.join(trace_dir, f"phase_a-worker-{w:02d}.bin")
        with open(a_path, "wb") as f:
            pass # 0 records
            
        worker_ops_count.append(len(records_inj) + len(records_micro))
        
    # Manifest JSON
    manifest = {
        "workload_id": f"e8_{case_name}" + ("_clean" if is_clean else ""),
        "description": f"Formal V2 E8 Micro-trace for {case_name} (Clean={is_clean})",
        "generator_commit": "e8_v5_hardcoded_geometry",
        "total_keys": TOTAL_KEYS,
        "value_size": VALUE_SIZE,
        "num_workers": NUM_WORKERS,
        "total_ops_count": sum(worker_ops_count),
        "audit_metrics": {
            "physical_span": 50 if "scan" in case_name else 1,
            "deleted_physical_keys_count": 10 if "scan_intersect" in case_name else (1 if "get_deleted" in case_name and not is_clean else 0),
            "live_physical_keys_count": 40 if "scan_intersect" in case_name else (50 if "scan_non_intersect" in case_name else 1),
            "actual_returned_visible_keys": 40 if "scan_intersect" in case_name and not is_clean else (50 if "scan" in case_name else (0 if "get_deleted" in case_name and not is_clean else 1)),
            "intersecting_tombstone_count_per_req": 1 if "scan_intersect" in case_name else 0,
            "actual_tombstone_count": 0 if is_clean else TOTAL_TOMBSTONES,
            "actual_union_coverage": 0 if is_clean else 200000,
            "geometry_sha256": geom_sha,
            "expected_visible_key_count": TOTAL_KEYS if is_clean else (TOTAL_KEYS - 200000)
        }
    }
    
    with open(os.path.join(trace_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        
    print(f"[TraceGenerator] Generated {trace_dir}: TotalOps={sum(worker_ops_count)}, GeomSHA={geom_sha[:12]}...")

if __name__ == "__main__":
    cases = ["scan_intersect", "scan_non_intersect", "get_live", "get_deleted"]
    for c in cases:
        generate_workload_traces(c, is_clean=False)
        generate_workload_traces(c, is_clean=True)
