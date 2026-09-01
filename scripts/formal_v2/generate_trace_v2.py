#!/usr/bin/env python3
"""
Formal V2 Trace Generator
Generates thesis-grade deterministic, worker-partitioned, barrier-synchronized traces
with comprehensive manifest metadata, SHA-256 verification, and exact operation quotas.
"""

import os
import json
import struct
import hashlib
import random
import subprocess
from typing import List, Tuple, Dict, Any

RECORD_FORMAT = "<BBBBIQQ"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT) # 24 bytes

OP_GET = 0
OP_SCAN = 1
OP_PUT = 2
OP_DELETERANGE = 3
OP_NOOP = 4

SCAN_MODE_RANGE = 0
SCAN_MODE_LIMIT = 1

def compute_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def get_git_commit() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd="/home/wam/grad/rocksdb-v11.8.0").decode().strip()
    except Exception:
        return "abeebd9630f11bd08c28b7bd43c7bdfc62050654"

def generate_workload_v2(
    workload_id: str,
    output_dir: str,
    total_keys: int,
    value_size: int,
    num_workers: int = 8,
    seed: int = 90001,
    phase_configs: Dict[str, Any] = None,
    is_clean_baseline: bool = False,
    scan_mode_default: int = SCAN_MODE_LIMIT,
    description: str = ""
) -> Dict[str, Any]:
    """
    Generates a full V2 trace directory containing manifest.json and 24 binary payload files
    (3 phases x 8 workers).
    """
    os.makedirs(output_dir, exist_ok=True)
    keys_per_worker = total_keys // num_workers
    assert total_keys % num_workers == 0, "total_keys must be divisible by num_workers"

    worker_ranges = []
    for w in range(num_workers):
        worker_ranges.append([w * keys_per_worker, (w + 1) * keys_per_worker])

    manifest = {
        "magic": "0x54524143455632",
        "version": "2.0",
        "endianness": "little",
        "generator_git_commit": get_git_commit(),
        "workload_id": workload_id,
        "description": description if description else workload_id,
        "is_clean_baseline": is_clean_baseline,
        "scan_mode": "SCAN_RANGE" if scan_mode_default == SCAN_MODE_RANGE else "SCAN_LIMIT",
        "total_keys": total_keys,
        "value_size": value_size,
        "num_workers": num_workers,
        "keys_per_worker": keys_per_worker,
        "worker_ranges": worker_ranges,
        "random_seed": seed,
        "record_size_bytes": RECORD_SIZE,
        "phases": ["phase_a", "phase_b", "phase_c"],
        "payload_files": {},
        "actual_metrics": {
            "cross_worker_ops_count": 0,
            "total_ops_count": 0,
            "total_get_count": 0,
            "total_scan_count": 0,
            "total_put_count": 0,
            "total_del_range_count": 0,
            "total_noop_count": 0,
            "union_deleted_keys_count": 0,
            "union_coverage_ratio_pct": 0.0,
            "interval_overlap_count": 0,
            "interval_overlap_ratio_pct": 0.0
        }
    }

    # Pre-calculate total deletions per worker across all phases for disjoint partitioning
    total_dels_per_worker = sum(p_cfg.get("del_count_per_worker", 0) for p_cfg in phase_configs.values())
    del_span_common = max(p_cfg.get("del_span", 0) for p_cfg in phase_configs.values()) if total_dels_per_worker > 0 else 0

    # Pre-partition global disjoint DeleteRange pools per worker
    worker_del_pools = {}
    for w in range(num_workers):
        worker_start_k, worker_end_k = worker_ranges[w]
        worker_del_pools[w] = []
        if total_dels_per_worker > 0:
            bucket_w = keys_per_worker // total_dels_per_worker
            for d_i in range(total_dels_per_worker):
                b = worker_start_k + d_i * bucket_w
                e = min(b + del_span_common, worker_end_k)
                if b < e:
                    worker_del_pools[w].append((b, e))
            rng_pool = random.Random(seed + w * 1000 + 777)
            rng_pool.shuffle(worker_del_pools[w])

    all_deleted_intervals = []
    global_op_id_counter = 0
    worker_del_cursors = {w: 0 for w in range(num_workers)}

    phases = [
        ("phase_a", 0, phase_configs.get("phase_a", {})),
        ("phase_b", 1, phase_configs.get("phase_b", {})),
        ("phase_c", 2, phase_configs.get("phase_c", {}))
    ]

    for p_name, p_idx, p_cfg in phases:
        ops_per_worker = p_cfg.get("ops_per_worker", 12500)
        del_count_per_worker = p_cfg.get("del_count_per_worker", 0)
        put_count_per_worker = p_cfg.get("put_count_per_worker", 0)
        scan_count_per_worker = p_cfg.get("scan_count_per_worker", 0)
        get_count_per_worker = p_cfg.get("get_count_per_worker", 0)
        scan_len = p_cfg.get("scan_len", 100)
        scan_mode = p_cfg.get("scan_mode", scan_mode_default)

        for w in range(num_workers):
            worker_start_k, worker_end_k = worker_ranges[w]
            w_seed = seed + p_idx * 100000 + w * 1000 + 13
            rng = random.Random(w_seed)

            # Construct exact operation pool
            worker_ops = []

            for _ in range(del_count_per_worker):
                b, e = worker_del_pools[w][worker_del_cursors[w]]
                worker_del_cursors[w] += 1
                if is_clean_baseline:
                    worker_ops.append((OP_NOOP, scan_mode, b, e))
                else:
                    worker_ops.append((OP_DELETERANGE, scan_mode, b, e))
                    all_deleted_intervals.append((b, e))

            for _ in range(put_count_per_worker):
                k = rng.randint(worker_start_k, worker_end_k - 1)
                worker_ops.append((OP_PUT, scan_mode, k, 0))

            for _ in range(scan_count_per_worker):
                if scan_mode == SCAN_MODE_RANGE:
                    max_b = max(worker_start_k, worker_end_k - 2)
                    b = rng.randint(worker_start_k, max_b)
                    e = min(b + scan_len, worker_end_k)
                    if b >= e:
                        e = b + 1
                    worker_ops.append((OP_SCAN, scan_mode, b, e))
                else:
                    b = rng.randint(worker_start_k, worker_end_k - 1)
                    worker_ops.append((OP_SCAN, scan_mode, b, scan_len))

            for _ in range(get_count_per_worker):
                k = rng.randint(worker_start_k, worker_end_k - 1)
                worker_ops.append((OP_GET, scan_mode, k, 0))

            # Deterministically shuffle worker operations
            rng.shuffle(worker_ops)

            # Serialize to binary payload
            bin_filename = f"{p_name}-worker-{w:02d}.bin"
            bin_path = os.path.join(output_dir, bin_filename)

            with open(bin_path, "wb") as fout:
                for op_type, s_mode, k1, k2 in worker_ops:
                    global_op_id_counter += 1
                    # Verify boundary constraints
                    if k1 < worker_start_k or k1 >= worker_end_k:
                        manifest["actual_metrics"]["cross_worker_ops_count"] += 1
                    if op_type == OP_DELETERANGE and (k2 > worker_end_k or k2 <= k1):
                        manifest["actual_metrics"]["cross_worker_ops_count"] += 1
                    if op_type == OP_SCAN and s_mode == SCAN_MODE_RANGE and (k2 > worker_end_k or k2 <= k1):
                        manifest["actual_metrics"]["cross_worker_ops_count"] += 1

                    # Count metrics
                    manifest["actual_metrics"]["total_ops_count"] += 1
                    if op_type == OP_GET: manifest["actual_metrics"]["total_get_count"] += 1
                    elif op_type == OP_SCAN: manifest["actual_metrics"]["total_scan_count"] += 1
                    elif op_type == OP_PUT: manifest["actual_metrics"]["total_put_count"] += 1
                    elif op_type == OP_DELETERANGE: manifest["actual_metrics"]["total_del_range_count"] += 1
                    elif op_type == OP_NOOP: manifest["actual_metrics"]["total_noop_count"] += 1

                    # Struct binary packing: 24 bytes (phase_id, op_type, scan_mode, reserved, op_id, key1, key2)
                    buf = struct.pack(
                        RECORD_FORMAT,
                        p_idx,
                        op_type,
                        s_mode,
                        0, # reserved
                        global_op_id_counter,
                        k1,
                        k2
                    )
                    fout.write(buf)

            # Record payload metadata
            payload_sha = compute_sha256(bin_path)
            manifest["payload_files"][bin_filename] = {
                "phase": p_name,
                "phase_id": p_idx,
                "worker_id": w,
                "ops_count": len(worker_ops),
                "file_size_bytes": len(worker_ops) * RECORD_SIZE,
                "sha256": payload_sha
            }

    # Calculate actual union coverage and overlap
    if all_deleted_intervals:
        sorted_intervals = sorted(all_deleted_intervals, key=lambda x: (x[0], x[1]))
        merged = []
        overlaps = 0
        for b, e in sorted_intervals:
            if not merged:
                merged.append((b, e))
            else:
                last_b, last_e = merged[-1]
                if b < last_e:
                    overlaps += 1
                    merged[-1] = (last_b, max(last_e, e))
                else:
                    merged.append((b, e))

        union_keys = sum(e - b for b, e in merged)
        manifest["actual_metrics"]["union_deleted_keys_count"] = union_keys
        manifest["actual_metrics"]["union_coverage_ratio_pct"] = round((union_keys / total_keys) * 100.0, 2)
        manifest["actual_metrics"]["interval_overlap_count"] = overlaps
        manifest["actual_metrics"]["interval_overlap_ratio_pct"] = round((overlaps / len(all_deleted_intervals)) * 100.0, 2)

    # Save manifest.json
    manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"[TraceV2] Generated {workload_id} -> {output_dir}")
    print(f"  Total Ops: {manifest['actual_metrics']['total_ops_count']} across {num_workers} workers")
    print(f"  Union Coverage: {manifest['actual_metrics']['union_coverage_ratio_pct']:.2f}%, Overlap: {manifest['actual_metrics']['interval_overlap_ratio_pct']:.2f}%")
    print(f"  Cross-Worker Ops: {manifest['actual_metrics']['cross_worker_ops_count']} (PASS if 0)")
    return manifest

def main():
    base_dir = "/home/wam/grad/s14-range-delete-study/traces/formal_v2"
    os.makedirs(base_dir, exist_ok=True)

    # 1. small_dynamic_500k_limit (500k Keys, LIMIT-100 Scan, 40.00% Coverage, 0.00% Overlap)
    small_phase_cfg = {
        "phase_a": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 7500,
            "scan_count_per_worker": 2500,
            "put_count_per_worker": 2350,
            "del_count_per_worker": 150,
            "del_span": 20,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        },
        "phase_b": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 2500,
            "scan_count_per_worker": 1250,
            "put_count_per_worker": 8000,
            "del_count_per_worker": 750,
            "del_span": 20,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        },
        "phase_c": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 7500,
            "scan_count_per_worker": 2500,
            "put_count_per_worker": 2150,
            "del_count_per_worker": 350,
            "del_span": 20,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        }
    }

    generate_workload_v2(
        workload_id="small_dynamic_500k_limit",
        output_dir=os.path.join(base_dir, "small_dynamic_500k_limit"),
        total_keys=500000,
        value_size=256,
        phase_configs=small_phase_cfg,
        is_clean_baseline=False,
        scan_mode_default=SCAN_MODE_LIMIT,
        description="Formal V2 Small Dynamic 500k with Scan-Limit-100 and 40% Delete Coverage"
    )

    generate_workload_v2(
        workload_id="small_dynamic_500k_clean",
        output_dir=os.path.join(base_dir, "small_dynamic_500k_clean"),
        total_keys=500000,
        value_size=256,
        phase_configs=small_phase_cfg,
        is_clean_baseline=True,
        scan_mode_default=SCAN_MODE_LIMIT,
        description="Formal V2 Small Dynamic 500k Clean Functional Baseline (No DeleteRange)"
    )

    # 2. small_dynamic_500k_range (500k Keys, SCAN_RANGE [b, b+100), 40% Coverage)
    small_phase_range_cfg = json.loads(json.dumps(small_phase_cfg))
    for p in small_phase_range_cfg.values():
        p["scan_mode"] = SCAN_MODE_RANGE

    generate_workload_v2(
        workload_id="small_dynamic_500k_range",
        output_dir=os.path.join(base_dir, "small_dynamic_500k_range"),
        total_keys=500000,
        value_size=256,
        phase_configs=small_phase_range_cfg,
        is_clean_baseline=False,
        scan_mode_default=SCAN_MODE_RANGE,
        description="Formal V2 Small Dynamic 500k with Fixed Range Scan [b, b+100)"
    )

    generate_workload_v2(
        workload_id="small_dynamic_500k_range_clean",
        output_dir=os.path.join(base_dir, "small_dynamic_500k_range_clean"),
        total_keys=500000,
        value_size=256,
        phase_configs=small_phase_range_cfg,
        is_clean_baseline=True,
        scan_mode_default=SCAN_MODE_RANGE,
        description="Formal V2 Small Dynamic 500k Range Clean Baseline (No DeleteRange)"
    )

    # 3. ls24_density_preserved (96M Keys, 19,856 DeleteRanges, 40.00% Coverage, 0.00% Overlap)
    ls24_phase_cfg = {
        "phase_a": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 7500,
            "scan_count_per_worker": 2500,
            "put_count_per_worker": 2192,
            "del_count_per_worker": 308,
            "del_span": 2028,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        },
        "phase_b": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 2500,
            "scan_count_per_worker": 1250,
            "put_count_per_worker": 7208,
            "del_count_per_worker": 1542,
            "del_span": 2028,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        },
        "phase_c": {
            "ops_per_worker": 12500,
            "get_count_per_worker": 7500,
            "scan_count_per_worker": 2500,
            "put_count_per_worker": 1868,
            "del_count_per_worker": 632,
            "del_span": 2028,
            "scan_len": 100,
            "scan_mode": SCAN_MODE_LIMIT
        }
    }

    generate_workload_v2(
        workload_id="ls24_density_preserved",
        output_dir=os.path.join(base_dir, "ls24_density_preserved"),
        total_keys=100663296, # 96M
        value_size=256,
        phase_configs=ls24_phase_cfg,
        is_clean_baseline=False,
        scan_mode_default=SCAN_MODE_LIMIT,
        description="Formal V2 Large Scale 96M Density Preserved with 40% Delete Coverage"
    )

    generate_workload_v2(
        workload_id="ls24_density_clean",
        output_dir=os.path.join(base_dir, "ls24_density_clean"),
        total_keys=100663296,
        value_size=256,
        phase_configs=ls24_phase_cfg,
        is_clean_baseline=True,
        scan_mode_default=SCAN_MODE_LIMIT,
        description="Formal V2 Large Scale 96M Density Clean Baseline (No DeleteRange)"
    )

    # 4. wb_cleanwrite_256mb (1M keys, 1,310,720 puts to flush 4 full memtables)
    wb_phase_cfg = {
        "phase_a": {"ops_per_worker": 54613, "get_count_per_worker": 0, "scan_count_per_worker": 0, "put_count_per_worker": 54613, "del_count_per_worker": 0, "scan_len": 0},
        "phase_b": {"ops_per_worker": 54613, "get_count_per_worker": 0, "scan_count_per_worker": 0, "put_count_per_worker": 54613, "del_count_per_worker": 0, "scan_len": 0},
        "phase_c": {"ops_per_worker": 54614, "get_count_per_worker": 0, "scan_count_per_worker": 0, "put_count_per_worker": 54614, "del_count_per_worker": 0, "scan_len": 0}
    }

    generate_workload_v2(
        workload_id="wb_cleanwrite_256mb",
        output_dir=os.path.join(base_dir, "wb_cleanwrite_256mb"),
        total_keys=1000000,
        value_size=256,
        phase_configs=wb_phase_cfg,
        is_clean_baseline=True,
        scan_mode_default=SCAN_MODE_LIMIT,
        description="Formal V2 256MB Clean Write MemTable Benchmark"
    )

if __name__ == "__main__":
    main()
