#!/usr/bin/env python3
"""
Formal V2 Benchmark Suite Orchestrator
Executes strict, interleaved, verified runs for:
- Small Matrix (15 runs): 5 groups x 3 reps (CLEAN, T0, T64, T256, T2048)
- LS24 24GiB Matrix (12 runs): 4 groups x 3 reps (CLEAN, T0, T256, T2048)

Safety Protocol:
- Interleaved execution order to eliminate time-bias
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
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")

def run_suite(suite_name: str):
    assert os.path.exists(DRIVER_BIN), f"Driver binary not found: {DRIVER_BIN}"

    if suite_name == "small":
        print("\n=========================================================")
        print("  Starting Small Core Matrix (15 Runs Interleaved)")
        print("=========================================================")
        total_keys = 500000
        value_size = 256
        trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_limit")
        trace_clean = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_clean")

        out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-small.csv")
        out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "small_matrix", "formal_phases.csv")
        out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "small_matrix", "formal_events.csv")
        raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "small_matrix", "raw")
        os.makedirs(raw_dir, exist_ok=True)
        os.makedirs(os.path.dirname(out_summary_csv), exist_ok=True)

        for f in [out_summary_csv, out_phases_csv, out_events_csv]:
            if os.path.exists(f): os.remove(f)

        groups_def = {
            "CLEAN": {"th": 0, "trace": trace_clean, "desc": "Formal V2 Small Clean Baseline (No-op)"},
            "T0":    {"th": 0, "trace": trace_del, "desc": "Formal V2 Small T=0 (Default Disabled)"},
            "T64":   {"th": 64, "trace": trace_del, "desc": "Formal V2 Small T=64 (Aggressive)"},
            "T256":  {"th": 256, "trace": trace_del, "desc": "Formal V2 Small T=256 (Balanced)"},
            "T2048": {"th": 2048, "trace": trace_del, "desc": "Formal V2 Small T=2048 (Conservative)"}
        }

        schedule = [
            ("CLEAN", 1), ("T0", 1), ("T64", 1), ("T256", 1), ("T2048", 1),
            ("T2048", 2), ("T256", 2), ("T64", 2), ("T0", 2), ("CLEAN", 2),
            ("T64", 3), ("CLEAN", 3), ("T2048", 3), ("T0", 3), ("T256", 3)
        ]

    elif suite_name == "ls24":
        print("\n=========================================================")
        print("  Starting 24GiB Large-Scale Matrix (12 Runs Interleaved)")
        print("=========================================================")
        total_keys = 100663296 # ~24GiB
        value_size = 256
        trace_del = os.path.join(BASE_DIR, "traces", "formal_v2", "ls24_density_preserved")
        trace_clean = os.path.join(BASE_DIR, "traces", "formal_v2", "ls24_density_clean")

        out_summary_csv = os.path.join(BASE_DIR, "results", "summary", "formal-v2-ls24.csv")
        out_phases_csv = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_phases.csv")
        out_events_csv = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_events.csv")
        raw_dir = os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "raw")
        os.makedirs(raw_dir, exist_ok=True)
        os.makedirs(os.path.dirname(out_summary_csv), exist_ok=True)

        for f in [out_summary_csv, out_phases_csv, out_events_csv]:
            if os.path.exists(f): os.remove(f)

        groups_def = {
            "LS24-DP-CLEAN": {"th": 0, "trace": trace_clean, "desc": "Formal V2 LS24 Density Clean Baseline"},
            "LS24-DP-T0":    {"th": 0, "trace": trace_del, "desc": "Formal V2 LS24 Density Preserved T=0"},
            "LS24-DP-T256":  {"th": 256, "trace": trace_del, "desc": "Formal V2 LS24 Density Preserved T=256"},
            "LS24-DP-T2048": {"th": 2048, "trace": trace_del, "desc": "Formal V2 LS24 Density Preserved T=2048"}
        }

        schedule = [
            ("LS24-DP-CLEAN", 1), ("LS24-DP-T0", 1), ("LS24-DP-T256", 1), ("LS24-DP-T2048", 1),
            ("LS24-DP-T2048", 2), ("LS24-DP-T256", 2), ("LS24-DP-T0", 2), ("LS24-DP-CLEAN", 2),
            ("LS24-DP-T256", 3), ("LS24-DP-CLEAN", 3), ("LS24-DP-T2048", 3), ("LS24-DP-T0", 3)
        ]
    else:
        raise ValueError(f"Unknown suite name: {suite_name}")

    total_runs = len(schedule)
    print(f"Total Runs Scheduled: {total_runs}\n")

    for run_idx, (g_name, rep_id) in enumerate(schedule, 1):
        g_info = groups_def[g_name]
        exp_id = f"formal_{g_name.lower().replace('-', '_')}_rep{rep_id:02d}"
        db_path = os.path.join(BASE_DIR, "run-db", "formal_v2", f"db_{exp_id}")
        run_raw_dir = os.path.join(raw_dir, exp_id)
        os.makedirs(run_raw_dir, exist_ok=True)

        t_start = time.time()
        print(f"[{time.strftime('%H:%M:%S')}] >>> [{run_idx}/{total_runs}] Starting {exp_id} (Group: {g_name}, Rep: {rep_id}, Th: {g_info['th']}) ...", flush=True)
        
        # Ensure fresh DB path
        if os.path.exists(db_path):
            shutil.rmtree(db_path)

        cmd = [
            DRIVER_BIN,
            "--exp_id", exp_id,
            "--group_name", g_name,
            "--db_path", db_path,
            "--result_dir", run_raw_dir,
            "--summary_csv", out_summary_csv,
            "--events_csv", out_events_csv,
            "--phases_csv", out_phases_csv,
            "--trace_dir", g_info["trace"],
            "--total_keys", str(total_keys),
            "--value_size", str(value_size),
            "--memtable_max_range_deletions", str(g_info["th"])
        ]

        meta = {
            "exp_id": exp_id,
            "group": g_name,
            "rep": rep_id,
            "threshold": g_info["th"],
            "trace_dir": g_info["trace"],
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
        df_curr = pd.read_csv(out_summary_csv)
        row_curr = df_curr[df_curr["exp_id"] == exp_id]
        if len(row_curr) == 0 or row_curr["verification_status"].iloc[0] != "PASS":
            print(f"\n[FATAL ERROR] Deep KV verification did not PASS on {exp_id}!", flush=True)
            sys.exit(1)

        duration = time.time() - t_start
        print(f"[{time.strftime('%H:%M:%S')}] >>> [PASS] {exp_id} completed in {duration:.1f}s, SHA={row_curr['sha256_hex'].iloc[0]}", flush=True)

        # Safe DB cleanup to conserve storage
        if os.path.exists(db_path):
            shutil.rmtree(db_path)

    print(f"\n=========================================================")
    print(f"  SUITE {suite_name.upper()} COMPLETED SUCCESSFULLY ({total_runs}/{total_runs} RUNS PASS)")
    print(f"  Summary CSV: {out_summary_csv}")
    print(f"=========================================================\n", flush=True)

def main():
    parser = argparse.ArgumentParser(description="Formal V2 Benchmark Suite Orchestrator")
    parser.add_argument("--suite", choices=["small", "ls24", "all"], required=True, help="Suite to execute")
    args = parser.parse_args()

    if args.suite == "small":
        run_suite("small")
    elif args.suite == "ls24":
        run_suite("ls24")
    elif args.suite == "all":
        run_suite("small")
        run_suite("ls24")

if __name__ == "__main__":
    main()
