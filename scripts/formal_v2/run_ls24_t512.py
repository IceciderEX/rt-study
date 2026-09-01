#!/usr/bin/env python3
"""
Formal V2 LS24 Matrix Addition: T=512 (5 Repetitions)
Executes:
- Group: LS24-DP-T512 (Threshold: 512)
- Reps: 5 independent runs (rep01 ~ rep05)
- Scale: 100,663,296 keys (24GiB scale)
- Workload: 24 worker payload trace (formal_v2/ls24_density_preserved)
- Safety: Serial execution, zero dirty DB, bit-for-bit SHA-256 deep verification.
"""

import os
import sys
import time
import shutil
import subprocess
import json
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")

TOTAL_KEYS = 100663296
VALUE_SIZE = 256
WBS = 67108864 # 64MB
THRESHOLD = 512
GROUP_NAME = "LS24-DP-T512"

TRACE_DIR = os.path.join(BASE_DIR, "traces", "formal_v2", "ls24_density_preserved")
SUMMARY_CSV = os.path.join(BASE_DIR, "results", "summary", "formal-v2-ls24.csv")
PHASES_CSV = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_phases.csv")
EVENTS_CSV = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_events.csv")
RAW_DIR = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "raw")
os.makedirs(RAW_DIR, exist_ok=True)

EXPECTED_SHA = "ed9dd343099ce277ce346bffca051f12293c402ac8d096fc5dd6b30355d64125"

def run_t512_rep(rep_id: int):
    exp_id = f"formal_ls24_dp_t512_rep{rep_id:02d}"
    db_path = os.path.join(BASE_DIR, "run-db", "formal_v2", f"db_{exp_id}")
    run_raw_dir = os.path.join(RAW_DIR, exp_id)
    os.makedirs(run_raw_dir, exist_ok=True)

    t_start = time.time()
    print(f"[{time.strftime('%H:%M:%S')}] >>> Starting {exp_id} (Group: {GROUP_NAME}, Rep: {rep_id}, Th: {THRESHOLD}, Keys: {TOTAL_KEYS:,}) ...", flush=True)

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    cmd = [
        DRIVER_BIN,
        "--exp_id", exp_id,
        "--group_name", GROUP_NAME,
        "--db_path", db_path,
        "--result_dir", run_raw_dir,
        "--summary_csv", SUMMARY_CSV,
        "--events_csv", EVENTS_CSV,
        "--phases_csv", PHASES_CSV,
        "--trace_dir", TRACE_DIR,
        "--total_keys", str(TOTAL_KEYS),
        "--value_size", str(VALUE_SIZE),
        "--memtable_max_range_deletions", str(THRESHOLD),
        "--write_buffer_size", str(WBS)
    ]

    meta = {
        "exp_id": exp_id,
        "group": GROUP_NAME,
        "rep": rep_id,
        "threshold": THRESHOLD,
        "write_buffer_size": WBS,
        "trace_dir": TRACE_DIR,
        "total_keys": TOTAL_KEYS,
        "value_size": VALUE_SIZE,
        "cmd": " ".join(cmd),
        "start_time": time.strftime("%Y-%m-%d %H:%M:%S")
    }
    with open(os.path.join(run_raw_dir, "run_meta.json"), "w") as f:
        json.dump(meta, f, indent=2)

    log_file = os.path.join(run_raw_dir, f"{exp_id}.log")
    with open(log_file, "w") as flog:
        res = subprocess.run(cmd, stdout=flog, stderr=subprocess.STDOUT)

    if res.returncode != 0:
        print(f"\n[FATAL ERROR] Driver execution failed with returncode {res.returncode} on {exp_id}!", flush=True)
        print(f"Check log: {log_file}", flush=True)
        sys.exit(1)

    # Post-run validation check
    df_curr = pd.read_csv(SUMMARY_CSV)
    row_curr = df_curr[df_curr["exp_id"] == exp_id]
    if len(row_curr) == 0:
        print(f"\n[FATAL ERROR] Row not found in summary CSV for {exp_id}!", flush=True)
        sys.exit(1)
    
    st = row_curr["verification_status"].iloc[0]
    sha = row_curr["sha256_hex"].iloc[0]
    if st != "PASS" or sha != EXPECTED_SHA:
        print(f"\n[FATAL ERROR] Deep KV verification failed on {exp_id}! Status={st}, SHA={sha} (expected={EXPECTED_SHA})", flush=True)
        sys.exit(1)

    duration = time.time() - t_start
    fg_sec = row_curr["foreground_wallclock_sec"].iloc[0]
    iops = row_curr["fg_trace_iops"].iloc[0]
    scan_p99 = row_curr["scan_p99_us"].iloc[0]
    get_del = row_curr["get_del_p99_us"].iloc[0]
    put_p99 = row_curr["put_p99_us"].iloc[0]
    pwa = row_curr["pwa_val_norm_fg"].iloc[0]

    print(f"[{time.strftime('%H:%M:%S')}] >>> [PASS] {exp_id} finished in {duration:.1f}s | FG={fg_sec:.2f}s | IOPS={iops:.1f} | ScanP99={scan_p99:.1f}us | Get(Del)P99={get_del:.1f}us | PutP99={put_p99:.1f}us | PWA={pwa:.2f} | SHA={sha[:16]}...", flush=True)

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

def main():
    print("===================================================================")
    print("  Formal V2 LS24 24GiB Matrix: T=512 Extension (5 Independent Reps)")
    print("===================================================================")
    t0 = time.time()
    for rep in range(1, 6):
        run_t512_rep(rep)
    total_time = time.time() - t0
    print(f"\n[ALL 5 REPS PASS] LS24-DP-T512 successfully completed in {total_time:.1f}s! Summary: {SUMMARY_CSV}")

if __name__ == "__main__":
    main()
