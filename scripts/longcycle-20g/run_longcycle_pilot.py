#!/usr/bin/env python3
"""
Formal V2 - LongCycle-20GiB Pilot Runner (2GiB Scale)
Runs:
1. LC20-PILOT-T0 (threshold=0)
2. LC20-PILOT-T512 (threshold=512)
Validates 4-phase synchronization, 5s timeseries output, L0/L1/L2 level formation,
full-DB SHA-256 state reconciliation, and estimates runtime/storage for 20GiB formal matrix.
"""

import os
import sys
import time
import json
import subprocess
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/longcycle_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces/formal_v2/longcycle_20g_pilot")
RUN_BASE = os.path.join(BASE_DIR, "run-db/longcycle_pilot")
RESULTS_BASE = os.path.join(BASE_DIR, "results/formal_v2/longcycle_20g")

PILOT_DELETE_SHA = "b194850c0e644b23a90d8bfc295a91b68eabc9f226c975be30b709887ccdf08a"
EXPECTED_VISIBLE_KEYS = 2048000

def run_pilot_case(case_name, threshold, is_clean=False):
    db_dir = os.path.join(RUN_BASE, case_name, "db")
    result_dir = os.path.join(RESULTS_BASE, case_name)
    os.makedirs(result_dir, exist_ok=True)
    os.makedirs(db_dir, exist_ok=True)

    cmd = [
        BIN_DRIVER,
        "--db_path", db_dir,
        "--trace_dir", TRACE_DIR,
        "--result_dir", result_dir,
        "--exp_id", case_name,
        "--threshold", str(threshold),
        "--is_clean", "true" if is_clean else "false",
        "--num_workers", "8",
        "--value_size", "1024",
        "--cooldown_sec", "60",
        "--sample_interval_sec", "5",
        "--expected_keys", str(EXPECTED_VISIBLE_KEYS),
        "--expected_sha256", PILOT_DELETE_SHA
    ]

    print(f"\n================================================================")
    print(f"  Starting LongCycle Pilot: {case_name} (threshold={threshold})")
    print(f"================================================================")

    t0 = time.time()
    proc = subprocess.Popen(cmd)
    ret = proc.wait()
    t1 = time.time()

    if ret != 0:
        print(f"[FATAL] {case_name} failed with return code {ret}!")
        return None

    elapsed = t1 - t0
    print(f"[SUCCESS] {case_name} finished in {elapsed:.2f}s ({elapsed/60:.2f}m)")

    # Read summary
    sum_file = os.path.join(result_dir, "summary.json")
    if os.path.exists(sum_file):
        with open(sum_file) as f:
            summary = json.load(f)
        summary["wall_time_sec"] = elapsed
        return summary
    return None

def main():
    print(">>> Launching LongCycle 2GiB Pilot Suite...")
    results = {}

    # 1. Pilot T0
    res_t0 = run_pilot_case("LC20-PILOT-T0", threshold=0)
    if not res_t0:
        sys.exit(1)
    results["T0"] = res_t0

    # 2. Pilot T512
    res_t512 = run_pilot_case("LC20-PILOT-T512", threshold=512)
    if not res_t512:
        sys.exit(1)
    results["T512"] = res_t512

    # Save summary
    out_json = os.path.join(RESULTS_BASE, "pilot_summary.json")
    with open(out_json, "w") as f:
        json.dump(results, f, indent=2)

    print("\n================================================================")
    print("  Pilot Summary and Verification")
    print("================================================================")
    print(f"  T0   Wall Time: {results['T0']['wall_time_sec']:.1f}s ({results['T0']['wall_time_sec']/60:.2f}m) | Disk: {results['T0']['physical_db_bytes']/1e9:.2f} GB | L0={results['T0']['l0_files']}, L1={results['T0']['l1_files']}, L2={results['T0']['l2_files']}, L3={results['T0']['l3_files']}")
    print(f"  T512 Wall Time: {results['T512']['wall_time_sec']:.1f}s ({results['T512']['wall_time_sec']/60:.2f}m) | Disk: {results['T512']['physical_db_bytes']/1e9:.2f} GB | L0={results['T512']['l0_files']}, L1={results['T512']['l1_files']}, L2={results['T512']['l2_files']}, L3={results['T512']['l3_files']}")
    print(f"  State Audit:   T0 Audit Pass={results['T0']['audit_pass']}, T512 Audit Pass={results['T512']['audit_pass']}")

if __name__ == "__main__":
    main()
