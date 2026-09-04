#!/usr/bin/env python3
import os
import sys
import subprocess
import time
from pathlib import Path

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/read_path_audit_driver")
TRACES_BASE = os.path.join(BASE_DIR, "traces/formal_v2")
CSV_OUTPUT = os.path.join(BASE_DIR, "results/summary/read-path-audit-e8.csv")
DB_BASE = os.path.join(BASE_DIR, "run-db/audit_matrix")

CONDITIONS = ["CLEAN", "T0-MEM", "POSTFLUSH-SST"]
WORKLOADS = [
    ("get_live", "e8_get_live_clean", "e8_get_live"),
    ("scan_intersect", "e8_scan_intersect_clean", "e8_scan_intersect")
]
REPS = 3
WARM_OPS = 5000

def main():
    os.makedirs(os.path.dirname(CSV_OUTPUT), exist_ok=True)
    os.makedirs(DB_BASE, exist_ok=True)

    # Remove old CSV if present
    if os.path.exists(CSV_OUTPUT):
        os.remove(CSV_OUTPUT)

    print(f"=== Starting E8 Read-Path Audit Matrix (N={REPS}) ===")
    print(f"Driver: {BIN_DRIVER}")
    print(f"Output: {CSV_OUTPUT}")

    total_runs = len(CONDITIONS) * len(WORKLOADS) * REPS
    current_run = 0

    for rep in range(1, REPS + 1):
        for cond in CONDITIONS:
            for wl_name, clean_trace, dirty_trace in WORKLOADS:
                current_run += 1
                trace_case = clean_trace if cond == "CLEAN" else dirty_trace
                trace_dir = os.path.join(TRACES_BASE, trace_case)
                db_dir = os.path.join(DB_BASE, f"db_{cond}_{wl_name}_rep{rep}")

                print(f"\n[{current_run}/{total_runs}] Running {cond} | {wl_name} | Rep {rep}...")

                cmd = [
                    "taskset", "-c", "2",
                    BIN_DRIVER,
                    "--condition", cond,
                    "--workload", wl_name,
                    "--db_path", db_dir,
                    "--trace_dir", trace_dir,
                    "--csv_path", CSV_OUTPUT,
                    "--rep", str(rep),
                    "--warm_ops", str(WARM_OPS)
                ]

                t0 = time.time()
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                t1 = time.time()

                if res.returncode != 0:
                    print(f"[ERROR] Run failed (code {res.returncode})!")
                    print(f"STDOUT: {res.stdout}")
                    print(f"STDERR: {res.stderr}")
                    sys.exit(1)
                else:
                    # Print audit results
                    for line in res.stdout.splitlines():
                        if "[AUDIT RESULT]" in line:
                            print(f"  {line}")
                    print(f"  -> Finished in {t1 - t0:.2f}s")

                # Clean up DB directory to avoid disk bloat
                subprocess.run(["rm", "-rf", db_dir])

    print("\n=== E8 Read-Path Audit Matrix Completed Successfully! ===")
    print(f"Results saved to: {CSV_OUTPUT}")

if __name__ == "__main__":
    main()
