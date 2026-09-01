#!/usr/bin/env python3
"""
Formal V2 Baseline Completion Suite Orchestrator
Executes:
- F1: 500K Static Threshold Matrix (7 groups x 5 reps = 35 runs)
- F2: LS24 24GiB Matrix Completion (4 groups x 2 additional reps = 8 runs -> 20 runs total)
- F3: Scan Robustness Fixed-Range Matrix (4 groups x 3 reps = 12 runs)
- F4: Alternative Buffer Sizing Matrix (6 groups x 3 reps = 18 runs)

Safety Protocol:
- Serial execution only (no concurrent DB conflicts)
- Zero dirty database policy (fresh DB per run)
- Bit-for-bit SHA-256 deep KV verification check per run
- Immediate abort on any failure (no silent retries or drops)
- DB cleanup only after PASS and log archiving
- Direct file redirection (no pipe buffer limits)
"""

import os
import sys
import time
import shutil
import subprocess
import argparse
import json
import random
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")

def run_single_experiment(
    exp_id: str,
    group_name: str,
    rep_id: int,
    threshold: int,
    trace_dir: str,
    total_keys: int,
    value_size: int,
    write_buffer_size: int,
    summary_csv: str,
    events_csv: str,
    phases_csv: str,
    raw_dir: str,
    run_idx: int,
    total_runs: int
):
    db_path = os.path.join(BASE_DIR, "run-db", "formal_v2", f"db_{exp_id}")
    run_raw_dir = os.path.join(raw_dir, exp_id)
    os.makedirs(run_raw_dir, exist_ok=True)

    t_start = time.time()
    print(f"[{time.strftime('%H:%M:%S')}] >>> [{run_idx}/{total_runs}] Starting {exp_id} (Group: {group_name}, Rep: {rep_id}, Th: {threshold}, WBS: {write_buffer_size // (1024*1024)}MB) ...", flush=True)

    # Ensure clean DB path
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    cmd = [
        DRIVER_BIN,
        "--exp_id", exp_id,
        "--group_name", group_name,
        "--db_path", db_path,
        "--result_dir", run_raw_dir,
        "--summary_csv", summary_csv,
        "--events_csv", events_csv,
        "--phases_csv", phases_csv,
        "--trace_dir", trace_dir,
        "--total_keys", str(total_keys),
        "--value_size", str(value_size),
        "--memtable_max_range_deletions", str(threshold),
        "--write_buffer_size", str(write_buffer_size)
    ]

    meta = {
        "exp_id": exp_id,
        "group": group_name,
        "rep": rep_id,
        "threshold": threshold,
        "write_buffer_size": write_buffer_size,
        "trace_dir": trace_dir,
        "total_keys": total_keys,
        "value_size": value_size,
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
        print(f"Aborting entire suite to prevent invalid data pollution. Check log: {log_file}", flush=True)
        sys.exit(1)

    # Post-run validation check from summary CSV
    df_curr = pd.read_csv(summary_csv)
    row_curr = df_curr[df_curr["exp_id"] == exp_id]
    if len(row_curr) == 0 or row_curr["verification_status"].iloc[0] != "PASS":
        print(f"\n[FATAL ERROR] Deep KV verification did not PASS on {exp_id}!", flush=True)
        sys.exit(1)

    duration = time.time() - t_start
    sha = row_curr['sha256_hex'].iloc[0]
    print(f"[{time.strftime('%H:%M:%S')}] >>> [PASS] {exp_id} completed in {duration:.1f}s, SHA={sha}", flush=True)

    # Safe DB cleanup to conserve storage
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

def run_f1():
    print("\n=========================================================")
    print("  Starting F1: 500K Static Threshold Matrix (35 Runs)")
    print("=========================================================")
    total_keys = 500000
    value_size = 256
    wbs = 67108864 # 64MB
    trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_limit")
    trace_clean = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_clean")

    out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-f1-small.csv")
    out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f1_small", "formal_phases.csv")
    out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f1_small", "formal_events.csv")
    raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "f1_small", "raw")
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(os.path.dirname(out_summary_csv), exist_ok=True)

    for f in [out_summary_csv, out_phases_csv, out_events_csv]:
        if os.path.exists(f): os.remove(f)

    groups_def = {
        "CLEAN": {"th": 0, "trace": trace_clean},
        "T0":    {"th": 0, "trace": trace_del},
        "T64":   {"th": 64, "trace": trace_del},
        "T256":  {"th": 256, "trace": trace_del},
        "T512":  {"th": 512, "trace": trace_del},
        "T1024": {"th": 1024, "trace": trace_del},
        "T2048": {"th": 2048, "trace": trace_del}
    }

    group_keys = list(groups_def.keys())
    # Deterministic interleaved schedule with 5 seeded rounds
    schedule = []
    seeds = [10001, 10002, 10003, 10004, 10005]
    for rep in range(1, 6):
        rng = random.Random(seeds[rep - 1])
        round_groups = group_keys.copy()
        rng.shuffle(round_groups)
        for g in round_groups:
            schedule.append((g, rep))

    total_runs = len(schedule)
    for idx, (g, rep) in enumerate(schedule, 1):
        g_info = groups_def[g]
        exp_id = f"formal_f1_{g.lower()}_rep{rep:02d}"
        run_single_experiment(
            exp_id, g, rep, g_info["th"], g_info["trace"],
            total_keys, value_size, wbs,
            out_summary_csv, out_events_csv, out_phases_csv, raw_dir,
            idx, total_runs
        )
    print(f"\n[PASS] F1 Completed: {out_summary_csv}\n")

def run_f2():
    print("\n=========================================================")
    print("  Starting F2: 24GiB Matrix Completion (Rep 04 & Rep 05, 8 Runs)")
    print("=========================================================")
    total_keys = 100663296
    value_size = 256
    wbs = 67108864
    trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "ls24_density_preserved")
    trace_clean = os.path.join(BASE_DIR, "traces", "formal_v2", "ls24_density_clean")

    out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-ls24.csv")
    out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_phases.csv")
    out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_events.csv")
    raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "raw")
    os.makedirs(raw_dir, exist_ok=True)

    groups_def = {
        "LS24-DP-CLEAN": {"th": 0, "trace": trace_clean},
        "LS24-DP-T0":    {"th": 0, "trace": trace_del},
        "LS24-DP-T256":  {"th": 256, "trace": trace_del},
        "LS24-DP-T2048": {"th": 2048, "trace": trace_del}
    }

    # Interleaved schedule for rep 4 and rep 5
    schedule = [
        # Rep 4: CLEAN -> T0 -> T256 -> T2048
        ("LS24-DP-CLEAN", 4), ("LS24-DP-T0", 4), ("LS24-DP-T256", 4), ("LS24-DP-T2048", 4),
        # Rep 5: T2048 -> T256 -> T0 -> CLEAN
        ("LS24-DP-T2048", 5), ("LS24-DP-T256", 5), ("LS24-DP-T0", 5), ("LS24-DP-CLEAN", 5)
    ]

    total_runs = len(schedule)
    for idx, (g, rep) in enumerate(schedule, 1):
        g_info = groups_def[g]
        exp_id = f"formal_{g.lower().replace('-', '_')}_rep{rep:02d}"
        run_single_experiment(
            exp_id, g, rep, g_info["th"], g_info["trace"],
            total_keys, value_size, wbs,
            out_summary_csv, out_events_csv, out_phases_csv, raw_dir,
            idx, total_runs
        )
    print(f"\n[PASS] F2 Completed (All 20 reps archived): {out_summary_csv}\n")

def run_f3():
    print("\n=========================================================")
    print("  Starting F3: Scan Robustness (Fixed-Range Scan, 12 Runs)")
    print("=========================================================")
    total_keys = 500000
    value_size = 256
    wbs = 67108864
    trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_range")
    trace_clean = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_range_clean")

    out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-f3-rangescan.csv")
    out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f3_rangescan", "formal_phases.csv")
    out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f3_rangescan", "formal_events.csv")
    raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "f3_rangescan", "raw")
    os.makedirs(raw_dir, exist_ok=True)

    for f in [out_summary_csv, out_phases_csv, out_events_csv]:
        if os.path.exists(f): os.remove(f)

    groups_def = {
        "F3-CLEAN": {"th": 0, "trace": trace_clean},
        "F3-T0":    {"th": 0, "trace": trace_del},
        "F3-T256":  {"th": 256, "trace": trace_del},
        "F3-T2048": {"th": 2048, "trace": trace_del}
    }

    schedule = [
        ("F3-CLEAN", 1), ("F3-T0", 1), ("F3-T256", 1), ("F3-T2048", 1),
        ("F3-T2048", 2), ("F3-T256", 2), ("F3-T0", 2), ("F3-CLEAN", 2),
        ("F3-T256", 3), ("F3-CLEAN", 3), ("F3-T2048", 3), ("F3-T0", 3)
    ]

    total_runs = len(schedule)
    for idx, (g, rep) in enumerate(schedule, 1):
        g_info = groups_def[g]
        exp_id = f"formal_{g.lower().replace('-', '_')}_rep{rep:02d}"
        run_single_experiment(
            exp_id, g, rep, g_info["th"], g_info["trace"],
            total_keys, value_size, wbs,
            out_summary_csv, out_events_csv, out_phases_csv, raw_dir,
            idx, total_runs
        )
    print(f"\n[PASS] F3 Completed: {out_summary_csv}\n")

def run_f4():
    print("\n=========================================================")
    print("  Starting F4: Alternative Buffer Sizing Baseline (18 Runs)")
    print("=========================================================")
    value_size = 256
    trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_limit")
    trace_clean_write = os.path.join(BASE_DIR, "traces", "formal_v2", "wb_cleanwrite_256mb")

    out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-f4-buffersize.csv")
    out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f4_buffersize", "formal_phases.csv")
    out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "f4_buffersize", "formal_events.csv")
    raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "f4_buffersize", "raw")
    os.makedirs(raw_dir, exist_ok=True)

    for f in [out_summary_csv, out_phases_csv, out_events_csv]:
        if os.path.exists(f): os.remove(f)

    # 6 groups across F4-A (Delete) and F4-B (Clean Write)
    groups_def = {
        # F4-A: Range Delete Workload (500k keys) with 16MB, 64MB, 128MB
        "F4A-WBS-16MB":  {"th": 0, "trace": trace_del, "keys": 500000,  "wbs": 16 * 1024 * 1024},
        "F4A-WBS-64MB":  {"th": 0, "trace": trace_del, "keys": 500000,  "wbs": 64 * 1024 * 1024},
        "F4A-WBS-128MB": {"th": 0, "trace": trace_del, "keys": 500000,  "wbs": 128 * 1024 * 1024},
        # F4-B: Clean Write Workload (1M keys) with 16MB, 64MB, 128MB
        "F4B-WBS-16MB":  {"th": 0, "trace": trace_clean_write, "keys": 1000000, "wbs": 16 * 1024 * 1024},
        "F4B-WBS-64MB":  {"th": 0, "trace": trace_clean_write, "keys": 1000000, "wbs": 64 * 1024 * 1024},
        "F4B-WBS-128MB": {"th": 0, "trace": trace_clean_write, "keys": 1000000, "wbs": 128 * 1024 * 1024}
    }

    schedule = [
        # Rep 1: F4A-16 -> F4A-64 -> F4A-128 -> F4B-16 -> F4B-64 -> F4B-128
        ("F4A-WBS-16MB", 1), ("F4A-WBS-64MB", 1), ("F4A-WBS-128MB", 1),
        ("F4B-WBS-16MB", 1), ("F4B-WBS-64MB", 1), ("F4B-WBS-128MB", 1),
        # Rep 2: F4B-128 -> F4B-64 -> F4B-16 -> F4A-128 -> F4A-64 -> F4A-16
        ("F4B-WBS-128MB", 2), ("F4B-WBS-64MB", 2), ("F4B-WBS-16MB", 2),
        ("F4A-WBS-128MB", 2), ("F4A-WBS-64MB", 2), ("F4A-WBS-16MB", 2),
        # Rep 3: Interleaved mix
        ("F4A-WBS-64MB", 3), ("F4B-WBS-16MB", 3), ("F4A-WBS-128MB", 3),
        ("F4B-WBS-64MB", 3), ("F4A-WBS-16MB", 3), ("F4B-WBS-128MB", 3)
    ]

    total_runs = len(schedule)
    for idx, (g, rep) in enumerate(schedule, 1):
        g_info = groups_def[g]
        exp_id = f"formal_{g.lower().replace('-', '_')}_rep{rep:02d}"
        run_single_experiment(
            exp_id, g, rep, g_info["th"], g_info["trace"],
            g_info["keys"], value_size, g_info["wbs"],
            out_summary_csv, out_events_csv, out_phases_csv, raw_dir,
            idx, total_runs
        )
    print(f"\n[PASS] F4 Completed: {out_summary_csv}\n")

def main():
    parser = argparse.ArgumentParser(description="Formal V2 Baseline Completion Orchestrator")
    parser.add_argument("--suite", choices=["f1", "f2", "f3", "f4", "all"], required=True, help="Suite to run")
    args = parser.parse_args()

    if args.suite == "f1":
        run_f1()
    elif args.suite == "f2":
        run_f2()
    elif args.suite == "f3":
        run_f3()
    elif args.suite == "f4":
        run_f4()
    elif args.suite == "all":
        run_f1()
        run_f3()
        run_f4()
        run_f2()

if __name__ == "__main__":
    main()
