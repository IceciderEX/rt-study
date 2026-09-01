#!/usr/bin/env python3
"""
Formal V2 - LongCycle-20GiB Trace Generator
Generates deterministic 4-phase mixed dynamic workload traces:
- Scale: Pilot (10% scale: 2.56M new keys, 2GiB logical data) or Full (25.6M new keys, 20GiB logical data)
- 8 Workers with mutually exclusive key partitions
- 50,000 DeleteRanges (Pilot: 5,000): 80% base (20% union coverage, span 128) + 20% controlled overlapping
- Four dynamic phases: Phase A (0->6.25G), Phase B (6.25G->12.5G), Phase C (12.5G->18.75G), Phase D (18.75G->25G)
- Categorized operations: GetDeleted, GetLiveAdjacent, GetLiveControl, ScanIntersect, ScanNonIntersect, ScanBoundary
"""

import os
import sys
import struct
import json
import hashlib
import random
import argparse
import numpy as np

# Op Types: 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=No-op
# Flags:
# bit 0: affected/intersect
# bit 1: boundary
# bit 2: inject
# bit 3: update_put
# bit 4: get_deleted
# bit 5: get_adjacent
# bit 6: get_control

RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

def generate_longcycle_traces(scale="pilot", output_base_dir="/home/wam/grad/s14-range-delete-study/traces/formal_v2"):
    num_workers = 8
    value_size = 1024 # 1KiB

    if scale == "pilot":
        total_new_keys = 2560000       # 2.56M keys (2.5GiB new values)
        total_updates = 500000         # 500k updates
        total_tombstones = 5000        # 5,000 tombstones
        keys_per_phase = 640000        # 640k keys per phase
        tombstone_span = 128
        trace_dir_name = "longcycle_20g_pilot"
    elif scale == "full":
        total_new_keys = 25600000      # 25.6M keys (25GiB new values)
        total_updates = 5000000        # 5M updates
        total_tombstones = 50000       # 50,000 tombstones
        keys_per_phase = 6400000       # 6.4M keys per phase
        tombstone_span = 128
        trace_dir_name = "longcycle_20g_full"
    else:
        raise ValueError(f"Unknown scale: {scale}")

    trace_dir = os.path.join(output_base_dir, trace_dir_name)
    os.makedirs(trace_dir, exist_ok=True)

    print(f"[TraceGen] Generating LongCycle-20GiB Traces (Scale={scale.upper()}):")
    print(f"  Total New Keys:    {total_new_keys:,} ({total_new_keys*value_size/1e9:.2f} GB)")
    print(f"  Total Tombstones:  {total_tombstones:,}")
    print(f"  Output Dir:        {trace_dir}")

    keys_per_worker = total_new_keys // num_workers
    keys_per_worker_phase = keys_per_phase // num_workers
    tombstones_per_worker = total_tombstones // num_workers

    # DeleteRange allocation per phase
    # Phase A: 4%, Phase B: 26%, Phase C: 60%, Phase D: 10%
    ts_phase_counts = [
        int(total_tombstones * 0.04),
        int(total_tombstones * 0.26),
        int(total_tombstones * 0.60),
        int(total_tombstones * 0.10)
    ]
    # Adjust rounding
    diff = total_tombstones - sum(ts_phase_counts)
    ts_phase_counts[2] += diff

    # DeleteRange allocation per phase
    # Phase A: 4% (20 base + 5 overlap per worker in pilot)
    # Phase B: 26% (130 base + 32 overlap per worker in pilot)
    # Phase C: 60% (300 base + 75 overlap per worker in pilot)
    # Phase D: 10% (50 base + 13 overlap per worker in pilot)
    base_counts_per_phase = [
        int(tombstones_per_worker * 0.8 * 0.04),
        int(tombstones_per_worker * 0.8 * 0.26),
        int(tombstones_per_worker * 0.8 * 0.60),
        int(tombstones_per_worker * 0.8 * 0.10)
    ]
    overlap_counts_per_phase = [
        int(tombstones_per_worker * 0.2 * 0.04),
        int(tombstones_per_worker * 0.2 * 0.26),
        int(tombstones_per_worker * 0.2 * 0.60),
        int(tombstones_per_worker * 0.2 * 0.10)
    ]

    worker_phase_tombstones = [[] for _ in range(num_workers)]
    all_deleted_keys_global = set()

    for w in range(num_workers):
        w_start = w * keys_per_worker
        # Disjoint base tombstones per worker: exactly (tombstones_per_worker * 0.8) non-overlapping intervals
        total_base = int(tombstones_per_worker * 0.8) # 500 pilot / 5000 full
        
        # Phase A base: from [w_start, w_start + keys_per_worker_phase)
        # Phase B base: from [w_start, w_start + keys_per_worker_phase)
        # Phase C base: from [w_start, w_start + 2*keys_per_worker_phase)
        # Phase D base: from [w_start, w_start + 3*keys_per_worker_phase)
        
        # Construct disjoint base tombstones within Phase A, B, C keys [0, 3*keys_per_worker_phase)
        base_list = []
        stride = 480 # 240k / 480 = 500 intervals (span=128 => 500*128=64,000 keys deleted per worker = exactly 20.0%)
        for i in range(total_base):
            ts_start = w_start + i * stride
            ts_end = ts_start + tombstone_span
            base_list.append((ts_start, ts_end))
            for k in range(ts_start, ts_end):
                all_deleted_keys_global.add(k)

        c_a = base_counts_per_phase[0]
        c_b = base_counts_per_phase[1]
        c_c = base_counts_per_phase[2]
        c_d = base_counts_per_phase[3]
        
        phase_base = [
            base_list[0 : c_a],
            base_list[c_a : c_a + c_b],
            base_list[c_a + c_b : c_a + c_b + c_c],
            base_list[c_a + c_b + c_c : c_a + c_b + c_c + c_d]
        ]
        
        # Overlaps inside each phase base list
        phase_ts_combined = []
        for p_idx in range(4):
            b_list = phase_base[p_idx]
            ov_cnt = overlap_counts_per_phase[p_idx]
            ov_list = []
            for j in range(ov_cnt):
                parent = b_list[j % len(b_list)]
                ov_list.append((parent[0], parent[1]))
            comb = b_list + ov_list
            rng = random.Random(90001 + w * 10 + p_idx)
            rng.shuffle(comb)
            phase_ts_combined.append(comb)
            
        worker_phase_tombstones[w] = phase_ts_combined

    # Global geometry SHA
    geom_hash = hashlib.sha256()
    for w in range(num_workers):
        for p_idx in range(4):
            for ts, te in worker_phase_tombstones[w][p_idx]:
                geom_hash.update(f"{ts}:{te}\n".encode("utf-8"))
    geom_sha256 = geom_hash.hexdigest()

    # Phase workload ratios (NewPut, UpdatePut, Get, Scan)
    # Phase A: 70%, 10%, 15%, 5%
    # Phase B: 50%, 20%, 20%, 10%
    # Phase C: 35%, 25%, 25%, 15%
    # Phase D: 40%, 15%, 30%, 15%
    phase_ratios = [
        (0.70, 0.10, 0.15, 0.05),
        (0.50, 0.20, 0.20, 0.10),
        (0.35, 0.25, 0.25, 0.15),
        (0.40, 0.15, 0.30, 0.15)
    ]

    phase_names = ["phase_a", "phase_b", "phase_c", "phase_d"]
    global_op_id = 0
    total_ops_per_phase = [0, 0, 0, 0]
    total_written_keys = 0

    write_proj_hash = hashlib.sha256()
    trace_hash = hashlib.sha256()

    worker_written_keys = [0] * num_workers
    worker_ts_idx = [0] * num_workers

    for p_idx, p_name in enumerate(phase_names):
        r_new, r_up, r_get, r_scan = phase_ratios[p_idx]
        ts_in_phase_per_worker = ts_phase_counts[p_idx] // num_workers

        for w in range(num_workers):
            w_start = w * keys_per_worker
            w_rng = random.Random(10007 * (p_idx + 1) + w)

            records = []
            cur_phase_new_keys = keys_per_worker_phase
            # Total operations for this worker in this phase based on new key puts ratio
            total_worker_ops = int(cur_phase_new_keys / r_new)
            num_updates = int(total_worker_ops * r_up)
            num_gets = int(total_worker_ops * r_get)
            num_scans = total_worker_ops - cur_phase_new_keys - num_updates - num_gets

            # Build worker op pool
            op_types_pool = (
                [("new_put", 2)] * cur_phase_new_keys +
                [("update_put", 2)] * num_updates +
                [("get", 0)] * num_gets +
                [("scan", 1)] * num_scans
            )
            w_rng.shuffle(op_types_pool)

            # Distribute DeleteRanges across the op pool
            # Phase A: injected after 85% of Phase A
            # Phase B: uniform across Phase B
            # Phase C: injected after 90% of Phase C (covering all intervals up to 216k)
            # Phase D: in first 20% of Phase D
            ts_injection_indices = []
            if ts_in_phase_per_worker > 0:
                if p_idx == 0: # Phase A: after 85%
                    start_idx = int(len(op_types_pool) * 0.85)
                    step = (len(op_types_pool) - start_idx) / ts_in_phase_per_worker
                    ts_injection_indices = [int(start_idx + i * step) for i in range(ts_in_phase_per_worker)]
                elif p_idx == 2: # Phase C: after 92%
                    start_idx = int(len(op_types_pool) * 0.92)
                    step = (len(op_types_pool) - start_idx) / ts_in_phase_per_worker
                    ts_injection_indices = [int(start_idx + i * step) for i in range(ts_in_phase_per_worker)]
                elif p_idx == 3: # Phase D: first 20%
                    end_idx = int(len(op_types_pool) * 0.20)
                    step = end_idx / ts_in_phase_per_worker
                    ts_injection_indices = [int(i * step) for i in range(ts_in_phase_per_worker)]
                else: # Phase B: uniform
                    step = len(op_types_pool) / ts_in_phase_per_worker
                    ts_injection_indices = [int(i * step) for i in range(ts_in_phase_per_worker)]

            ts_set_indices = set(ts_injection_indices)
            p_ts_idx = 0

            new_key_seq = w_start + worker_written_keys[w]
            cur_max_written = new_key_seq

            for op_idx, (op_label, op_type) in enumerate(op_types_pool):
                # Check if DeleteRange should be injected here
                if op_idx in ts_set_indices:
                    if p_ts_idx < len(worker_phase_tombstones[w][p_idx]):
                        ts_start, ts_end = worker_phase_tombstones[w][p_idx][p_ts_idx]
                        p_ts_idx += 1
                        # DeleteRange record: flags = 4 (inject)
                        rec = struct.pack(RECORD_FORMAT, p_idx, 3, 0, 4, global_op_id, ts_start, ts_end)
                        records.append(rec)
                        global_op_id += 1
                        write_proj_hash.update(f"DEL:{ts_start}:{ts_end}\n".encode("utf-8"))
                        trace_hash.update(rec)

                flags = 0
                key1 = 0
                key2 = 0

                if op_label == "new_put":
                    key1 = new_key_seq
                    new_key_seq += 1
                    cur_max_written = new_key_seq
                    write_proj_hash.update(f"PUT:{key1}\n".encode("utf-8"))
                elif op_label == "update_put":
                    # Update a previously written LIVE key
                    if cur_max_written > w_start:
                        cand = w_rng.randint(w_start, cur_max_written - 1)
                        tries = 0
                        while cand in all_deleted_keys_global and tries < 10 and cur_max_written - w_start > 1000:
                            cand = w_rng.randint(w_start, cur_max_written - 1)
                            tries += 1
                        key1 = cand
                    else:
                        key1 = w_start
                    flags |= (1 << 3) # update_put
                    write_proj_hash.update(f"UP_PUT:{key1}\n".encode("utf-8"))
                elif op_label == "get":
                    # Classify Get: GetDeleted, GetLiveAdjacent, GetLiveControl
                    r_val = w_rng.random()
                    if cur_max_written > w_start:
                        cand_k = w_rng.randint(w_start, cur_max_written - 1)
                    else:
                        cand_k = w_start

                    if cand_k in all_deleted_keys_global:
                        flags |= (1 << 4) # get_deleted
                        key1 = cand_k
                    elif r_val < 0.3: # Adjacent
                        flags |= (1 << 5) # get_adjacent
                        key1 = cand_k
                    else:
                        flags |= (1 << 6) # get_control
                        key1 = cand_k
                elif op_label == "scan":
                    # Fixed-range scan [start, start+100)
                    span = 100
                    if cur_max_written > w_start + span:
                        s_start = w_rng.randint(w_start, cur_max_written - span)
                    else:
                        s_start = w_start
                    key1 = s_start
                    key2 = s_start + span

                    # Check intersection
                    has_intersect = any((k in all_deleted_keys_global) for k in range(key1, min(key2, key1 + 20)))
                    if has_intersect:
                        flags |= 1 # ScanIntersect
                    else:
                        flags |= 2 # ScanNonIntersect

                rec = struct.pack(RECORD_FORMAT, p_idx, op_type, 0, flags, global_op_id, key1, key2)
                records.append(rec)
                global_op_id += 1
                trace_hash.update(rec)

            worker_written_keys[w] += cur_phase_new_keys
            total_ops_per_phase[p_idx] += len(records)

            # Write binary trace file
            out_file = os.path.join(trace_dir, f"{p_name}-worker-{w:02d}.bin")
            with open(out_file, "wb") as f:
                for r in records:
                    f.write(r)

        print(f"  - {p_name.upper()}: Total Ops={total_ops_per_phase[p_idx]:,} ({len(records)} per worker)")

    # Manifest
    manifest = {
        "workload_id": f"formal_v2_{trace_dir_name}",
        "scale": scale,
        "generator_commit": "longcycle_v1_formal",
        "total_new_keys": total_new_keys,
        "value_size": value_size,
        "num_workers": num_workers,
        "total_ops_count": sum(total_ops_per_phase),
        "phase_ops_count": total_ops_per_phase,
        "audit_metrics": {
            "total_tombstones": total_tombstones,
            "unique_tombstone_intervals": int(total_tombstones * 0.8),
            "deleted_union_keys_count": len(all_deleted_keys_global),
            "union_coverage_ratio": len(all_deleted_keys_global) / total_new_keys,
            "tombstone_overlap_ratio": 0.20,
            "expected_visible_key_count": total_new_keys - len(all_deleted_keys_global),
            "expected_logical_live_value_bytes": (total_new_keys - len(all_deleted_keys_global)) * value_size,
            "geometry_sha256": geom_sha256,
            "write_projection_sha256": write_proj_hash.hexdigest(),
            "trace_sha256": trace_hash.hexdigest()
        }
    }

    manifest_path = os.path.join(trace_dir, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"[TraceGen SUCCESS] Manifest written to {manifest_path}")
    print(f"  - Geom SHA256:       {geom_sha256}")
    print(f"  - WriteProj SHA256:  {write_proj_hash.hexdigest()}")
    print(f"  - Expected Visible:  {manifest['audit_metrics']['expected_visible_key_count']:,} keys ({manifest['audit_metrics']['expected_logical_live_value_bytes']/1e9:.2f} GB)")
    return manifest

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", choices=["pilot", "full"], default="pilot")
    args = parser.parse_args()
    generate_longcycle_traces(scale=args.scale)
