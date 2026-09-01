#!/usr/bin/env python3
import os
import sys
import json
import time
import subprocess
import shutil

BASE_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DRIVER_BIN = os.path.join(BASE_DIR, "bin/longcycle_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces/formal_v2/longcycle_20g_full")
RUN_BASE = os.path.join(BASE_DIR, "run-db/longcycle_20g")
RESULTS_BASE = os.path.join(BASE_DIR, "results/formal_v2/longcycle_20g")

FULL_MATRIX_ORDER = [
    ("LC20-R1-T0", 0, False),
    ("LC20-R2-CLEAN", 0, True),
    ("LC20-R3-T512", 512, False),
    ("LC20-R4-CLEAN", 0, True),
    ("LC20-R5-T0", 0, False),
    ("LC20-R6-T512", 512, False),
    ("LC20-R7-T512", 512, False),
    ("LC20-R8-T0", 0, False),
    ("LC20-R9-CLEAN", 0, True),
]

def check_disk_safety():
    stat = shutil.disk_usage(RUN_BASE if os.path.exists(RUN_BASE) else BASE_DIR)
    free_gb = stat.free / (1024**3)
    used_pct = stat.used / stat.total
    print(f"[PREFLIGHT DISK CHECK] Free: {free_gb:.2f} GB, Used: {used_pct*100:.1f}%")
    if free_gb < 200.0 or used_pct > 0.70:
        print(f"[FATAL ERROR] Disk safety violation! Free={free_gb:.2f}GB (<200GB) or Used={used_pct*100:.1f}% (>70%)")
        sys.exit(1)

def run_matrix_case(exp_id, threshold, is_clean):
    db_path = os.path.join(RUN_BASE, exp_id, "db")
    out_dir = os.path.join(RESULTS_BASE, exp_id)
    os.makedirs(out_dir, exist_ok=True)

    if os.path.exists(db_path):
        shutil.rmtree(db_path)
    os.makedirs(db_path, exist_ok=True)

    cmd = [
        DRIVER_BIN,
        "--trace_dir", TRACE_DIR,
        "--db_dir", db_path,
        "--out_dir", out_dir,
        "--exp_id", exp_id,
        "--threshold", str(threshold),
        "--value_size", "1024",
        "--is_clean", "true" if is_clean else "false",
        "--sample_interval_sec", "5",
        "--cooldown_sec", "600"
    ]

    print("\n" + "=" * 64)
    print(f"  Starting LongCycle 20GiB: {exp_id} (threshold={threshold}, clean={is_clean})")
    print("=" * 64)
    t0 = time.time()
    res = subprocess.run(cmd)
    t_wall = time.time() - t0

    if res.returncode != 0:
        print(f"[FATAL] {exp_id} failed with return code {res.returncode}!")
        sys.exit(1)

    print(f"[SUCCESS] {exp_id} finished in {t_wall:.2f}s ({t_wall/60:.2f}m)")
    return t_wall

def main():
    print(">>> Starting FormalV2-LongCycle-20GiB 9-Run Matrix Execution...")
    os.makedirs(RUN_BASE, exist_ok=True)
    os.makedirs(RESULTS_BASE, exist_ok=True)

    manifest_path = os.path.join(TRACE_DIR, "manifest.json")
    if not os.path.exists(manifest_path):
        print(f"[FATAL] Trace manifest not found at {manifest_path}. Please generate full trace first.")
        sys.exit(1)

    check_disk_safety()

    matrix_results = {}
    total_start = time.time()

    for idx, (exp_id, threshold, is_clean) in enumerate(FULL_MATRIX_ORDER):
        print(f"\n[MATRIX PROGRESS] Step {idx+1}/9: {exp_id}")
        check_disk_safety()
        wall_time = run_matrix_case(exp_id, threshold, is_clean)
        
        sum_file = os.path.join(RESULTS_BASE, exp_id, "summary.json")
        if os.path.exists(sum_file):
            with open(sum_file) as f:
                data = json.load(f)
                data["wall_time_sec"] = wall_time
                matrix_results[exp_id] = data

        with open(os.path.join(RESULTS_BASE, "matrix_summary.json"), "w") as f:
            json.dump(matrix_results, f, indent=2)

    total_wall = time.time() - total_start
    print("\n" + "=" * 64)
    print("  FormalV2-LongCycle-20GiB 9-Run Matrix Fully Completed!")
    print(f"  Total Wall Time: {total_wall:.2f}s ({total_wall/60:.2f}m / {total_wall/3600:.2f}h)")
    print("=" * 64)

if __name__ == "__main__":
    main()
