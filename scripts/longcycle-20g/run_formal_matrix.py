#!/usr/bin/env python3
"""
FormalV2-LongCycle-20GiB: 9-Run Formal Randomized Interleaved Matrix
Execution Order (Anti-Drift Latin Order):
  Step 1: LC20-R1-T0-NATIVE       (threshold=0,        clean=false)
  Step 2: LC20-R2-CLEAN           (threshold=0,        clean=true)
  Step 3: LC20-R3-Tactive-STATIC  (threshold=Tactive,  clean=false)
  Step 4: LC20-R4-CLEAN           (threshold=0,        clean=true)
  Step 5: LC20-R5-T0-NATIVE       (threshold=0,        clean=false)
  Step 6: LC20-R6-Tactive-STATIC  (threshold=Tactive,  clean=false)
  Step 7: LC20-R7-Tactive-STATIC  (threshold=Tactive,  clean=false)
  Step 8: LC20-R8-T0-NATIVE       (threshold=0,        clean=false)
  Step 9: LC20-R9-CLEAN           (threshold=0,        clean=true)
"""

import os
import sys
import json
import time
import shutil
import subprocess

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/longcycle_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces/formal_v2/longcycle_20g_full")
MATRIX_OUT_DIR = os.path.join(BASE_DIR, "results/formal_v2/longcycle_20g")
SCRATCH_DIR = os.path.join(BASE_DIR, "scratch/longcycle_formal")
PILOT_DECISION_FILE = os.path.join(BASE_DIR, "results/formal_v2/longcycle_pilot/pilot_decision.json")

DELETE_EXPECTED_KEYS = 20480000
DELETE_EXPECTED_SHA = "60f7b31c97579acd3353b1e3bde6284676198ae0d9068e8dd7ceb8a8a0024674"

CLEAN_EXPECTED_KEYS = 25600000
CLEAN_EXPECTED_SHA = "8fe46e1ffec1394b3004c4b0b07b9989b7c303d9e3ad40ae1f230991d538184f"

def check_disk_safety(step_label):
    stat = shutil.disk_usage(BASE_DIR)
    free_gb = stat.free / (1024**3)
    used_pct = stat.used / stat.total * 100.0
    print("-" * 80)
    print(f"  [DISK SAFETY: {step_label}] Free: {free_gb:.2f} GiB | Used: {used_pct:.1f}%")
    print("-" * 80)

    if free_gb < 200.0:
        raise RuntimeError(f"DISK SAFETY FAILED before {step_label}: Free space {free_gb:.2f} GiB < 200 GiB")
    if used_pct > 70.0:
        raise RuntimeError(f"DISK SAFETY FAILED before {step_label}: Disk usage {used_pct:.1f}% > 70%")
    if free_gb < 50.0:
        raise RuntimeError(f"DISK SAFETY FAILED before {step_label}: Headroom {free_gb:.2f} GiB < 50 GiB")

def run_single_step(step_idx, exp_id, condition_label, threshold, is_clean, tactive_val):
    out_dir = os.path.join(MATRIX_OUT_DIR, exp_id)
    db_dir = os.path.join(SCRATCH_DIR, f"db_{exp_id}")
    os.makedirs(out_dir, exist_ok=True)
    if os.path.exists(db_dir):
        shutil.rmtree(db_dir)
    os.makedirs(db_dir, exist_ok=True)

    check_disk_safety(f"Step {step_idx}/9: {exp_id}")

    exp_keys = CLEAN_EXPECTED_KEYS if is_clean else DELETE_EXPECTED_KEYS
    exp_sha = CLEAN_EXPECTED_SHA if is_clean else DELETE_EXPECTED_SHA

    print("\n" + "=" * 80)
    print(f"  FORMAL MATRIX RUN [{step_idx}/9]: {exp_id}")
    print(f"  Condition:     {condition_label}")
    print(f"  Threshold:     {threshold}")
    print(f"  Is Clean:      {is_clean}")
    print(f"  Expected Keys: {exp_keys:,}")
    print(f"  Expected SHA:  {exp_sha}")
    print(f"  Output Dir:    {out_dir}")
    print("=" * 80)

    cmd = [
        BIN_DRIVER,
        "--exp_id", exp_id,
        "--db_path", db_dir,
        "--trace_dir", TRACE_DIR,
        "--result_dir", out_dir,
        "--threshold", str(threshold),
        "--is_clean", "true" if is_clean else "false",
        "--num_workers", "8",
        "--value_size", "1024",
        "--cooldown_sec", "600",
        "--sample_interval_sec", "5",
        "--expected_keys", str(exp_keys),
        "--expected_sha256", exp_sha
    ]

    t0 = time.time()
    res = subprocess.run(cmd, cwd=BASE_DIR)
    elapsed = time.time() - t0

    if res.returncode != 0:
        raise RuntimeError(f"Formal run failed for {exp_id} with returncode={res.returncode}")

    summary_path = os.path.join(out_dir, "summary.json")
    if not os.path.exists(summary_path):
        raise RuntimeError(f"summary.json not found for {exp_id}")

    with open(summary_path) as f:
        summary = json.load(f)

    if not summary.get("audit_pass", False):
        raise RuntimeError(f"RECONCILIATION AUDIT FAILED for {exp_id}!")

    print(f"\n[STEP {step_idx} COMPLETED] {exp_id} finished in {elapsed:.1f}s")
    print(f"  - DB API IOPS:       {summary.get('db_api_iops', 0):.2f}")
    print(f"  - Total Trace Ops:   {summary.get('total_trace_ops', 0):,}")
    print(f"  - Total DB API Ops:  {summary.get('total_db_api_ops', 0):,}")
    print(f"  - Total Flushes:     {summary['flush_stats']['total_flushes']}")
    print(f"  - RangeDel Flushes:  {summary['flush_stats']['range_del_flushes']}")
    print(f"  - WBufferFull Flush: {summary['flush_stats']['write_buffer_full_flushes']}")
    print(f"  - Audit Status:      PASSED (100% Keys & SHA-256 matched)")

    # Clean up DB directory to free disk space for subsequent runs
    if os.path.exists(db_dir):
        shutil.rmtree(db_dir)

    summary["step_idx"] = step_idx
    summary["condition_label"] = condition_label
    summary["elapsed_wall_sec"] = elapsed
    return summary

def main():
    if not os.path.exists(PILOT_DECISION_FILE):
        raise RuntimeError(f"Pilot decision file {PILOT_DECISION_FILE} not found! Please run pilot pipeline first.")

    with open(PILOT_DECISION_FILE) as f:
        pilot_decision = json.load(f)

    tactive_val = pilot_decision["selected_tactive"]
    tactive_name = pilot_decision["tactive_name"]

    print("=" * 80)
    print("  FormalV2-LongCycle-20GiB Formal 9-Run Matrix Launcher")
    print(f"  Frozen Tactive: {tactive_name} (threshold={tactive_val})")
    print("=" * 80)

    # 9-Run Latin Schedule
    schedule = [
        (1, "LC20-R1-T0-NATIVE", "T0-NATIVE", 0, False),
        (2, "LC20-R2-CLEAN", "CLEAN", 0, True),
        (3, f"LC20-R3-{tactive_name}", tactive_name, tactive_val, False),
        (4, "LC20-R4-CLEAN", "CLEAN", 0, True),
        (5, "LC20-R5-T0-NATIVE", "T0-NATIVE", 0, False),
        (6, f"LC20-R6-{tactive_name}", tactive_name, tactive_val, False),
        (7, f"LC20-R7-{tactive_name}", tactive_name, tactive_val, False),
        (8, "LC20-R8-T0-NATIVE", "T0-NATIVE", 0, False),
        (9, "LC20-R9-CLEAN", "CLEAN", 0, True),
    ]

    os.makedirs(MATRIX_OUT_DIR, exist_ok=True)
    os.makedirs(SCRATCH_DIR, exist_ok=True)

    all_summaries = []
    matrix_start = time.time()

    for step_idx, exp_id, condition_label, threshold, is_clean in schedule:
        summary = run_single_step(step_idx, exp_id, condition_label, threshold, is_clean, tactive_val)
        all_summaries.append(summary)

    total_elapsed = time.time() - matrix_start

    # Save grand matrix summary
    grand_summary = {
        "matrix_name": "FormalV2-LongCycle-20GiB",
        "total_runs": 9,
        "frozen_tactive": tactive_val,
        "frozen_tactive_name": tactive_name,
        "total_elapsed_sec": total_elapsed,
        "runs": all_summaries
    }

    grand_summary_file = os.path.join(MATRIX_OUT_DIR, "matrix_summary.json")
    with open(grand_summary_file, "w") as f:
        json.dump(grand_summary, f, indent=2)

    print("\n" + "=" * 80)
    print("  ALL 9 RUNS OF FORMAL MATRIX COMPLETED SUCCESSFULLY!")
    print(f"  Total Matrix Wall Time: {total_elapsed / 60:.2f} minutes")
    print(f"  Grand Summary saved to: {grand_summary_file}")
    print("=" * 80)

if __name__ == "__main__":
    main()
