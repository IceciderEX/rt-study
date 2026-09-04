#!/usr/bin/env python3
"""
RTP-MC V2-A vs Native T512 Small Core Matrix (N=5 Interleaved Repetitions)

Protocol:
- Trace: small_dynamic_500k_limit (500k operations, 8 workers)
- Modes:
  1. native_t512: Fixed trigger T=512, memtable_max_range_deletions=512
  2. rtp_mc_v2a: Active RTP-MC V2-A controller, memtable_max_range_deletions=0, dynamic SEAL
- Repetitions: 5 independent runs per group (total 10 runs)
- Execution: Interleaved schedule (ABBA ABBA AB) to eliminate thermal/cache drift
- Safety:
  - Clean DB per run
  - Bit-for-bit SHA-256 verification check per run
  - Full audit logs: summary.csv, phases.csv, events.csv, controller_windows.csv, controller_actions.csv
"""

import os
import sys
import time
import shutil
import subprocess
import json
import pandas as pd
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")

OUT_DIR = os.path.join(BASE_DIR, "results", "formal_v2", "rtp_mc_n5")
SUMMARY_CSV = os.path.join(OUT_DIR, "summary.csv")
PHASES_CSV = os.path.join(OUT_DIR, "phases.csv")
EVENTS_CSV = os.path.join(OUT_DIR, "events.csv")
WINDOWS_CSV = os.path.join(OUT_DIR, "controller_windows.csv")
ACTIONS_CSV = os.path.join(OUT_DIR, "controller_actions.csv")
RAW_DIR = os.path.join(OUT_DIR, "raw")

CONFIG_T512 = os.path.join(BASE_DIR, "configs", "formal_v2", "rtp_mc", "rtp_mc_shadow_t512.ini")
CONFIG_V2A = os.path.join(BASE_DIR, "configs", "formal_v2", "rtp_mc", "rtp_mc_active_v2a.ini")

SCHEDULE = [
    ("native_t512", 1, CONFIG_T512, "RTC-T512"),
    ("rtp_mc_v2a",  1, CONFIG_V2A,  "RTP-MC-V2A"),
    ("rtp_mc_v2a",  2, CONFIG_V2A,  "RTP-MC-V2A"),
    ("native_t512", 2, CONFIG_T512, "RTC-T512"),
    ("native_t512", 3, CONFIG_T512, "RTC-T512"),
    ("rtp_mc_v2a",  3, CONFIG_V2A,  "RTP-MC-V2A"),
    ("rtp_mc_v2a",  4, CONFIG_V2A,  "RTP-MC-V2A"),
    ("native_t512", 4, CONFIG_T512, "RTC-T512"),
    ("native_t512", 5, CONFIG_T512, "RTC-T512"),
    ("rtp_mc_v2a",  5, CONFIG_V2A,  "RTP-MC-V2A"),
]

def run_single(mode_name: str, rep_id: int, config_file: str, group_name: str):
    exp_id = f"{mode_name}_rep{rep_id:02d}"
    db_path = os.path.join(BASE_DIR, "run-db", "formal_v2", f"db_{exp_id}")
    run_raw_dir = os.path.join(RAW_DIR, exp_id)
    os.makedirs(run_raw_dir, exist_ok=True)

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    cmd = [
        DRIVER_BIN,
        "--config", config_file,
        "--exp_id", exp_id,
        "--group_name", group_name,
        "--db_path", db_path,
        "--result_dir", run_raw_dir,
        "--summary_csv", SUMMARY_CSV,
        "--events_csv", EVENTS_CSV,
        "--phases_csv", PHASES_CSV,
        "--windows_csv", WINDOWS_CSV,
        "--actions_csv", ACTIONS_CSV,
    ]

    t0 = time.time()
    print(f"[{time.strftime('%H:%M:%S')}] >>> Starting {exp_id} ({group_name}, rep {rep_id}) ...", flush=True)

    log_path = os.path.join(run_raw_dir, "driver.log")
    with open(log_path, "w") as log_f:
        p = subprocess.run(cmd, stdout=log_f, stderr=subprocess.STDOUT, text=True)

    elapsed = time.time() - t0
    if p.returncode != 0:
        print(f"[FAIL] {exp_id} exited with code {p.returncode}! See {log_path}", file=sys.stderr)
        sys.exit(1)

    # Clean up DB after verified PASS
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"[{time.strftime('%H:%M:%S')}] [PASS] {exp_id} completed in {elapsed:.2f}s", flush=True)

def analyze_results():
    if not os.path.exists(SUMMARY_CSV):
        print(f"Summary file not found: {SUMMARY_CSV}")
        return

    df = pd.read_csv(SUMMARY_CSV)
    print("\n=========================================================================================")
    print("                    RTP-MC V2-A vs Native T512 Evaluation Report (N=5)")
    print("=========================================================================================")
    
    cols = [
        "group_name", "exp_id", "verification_status", "foreground_wallclock_sec", 
        "scan_p99_us", "get_live_p99_us", "put_p99_us", "fg_db_api_iops", 
        "total_exp_flush_count", "controller_total_flush_count", 
        "total_exp_flush_engine_out_mb", "total_exp_comp_write_mb", "cwa_val_norm_total"
    ]
    sub_df = df[cols]
    print(sub_df.to_string(index=False))

    print("\n-----------------------------------------------------------------------------------------")
    print("                              Statistical Aggregation (Mean ± Std)")
    print("-----------------------------------------------------------------------------------------")
    metrics = [
        "foreground_wallclock_sec", "scan_p99_us", "get_live_p99_us", "put_p99_us",
        "fg_db_api_iops", "total_exp_flush_count", "controller_total_flush_count",
        "total_exp_comp_write_mb", "cwa_val_norm_total"
    ]

    agg = df.groupby("group_name")[metrics].agg(["mean", "std"])
    print(agg.to_string())

    # Pareto Comparison
    t512_scan = df[df["group_name"] == "RTC-T512"]["scan_p99_us"].mean()
    v2a_scan = df[df["group_name"] == "RTP-MC-V2A"]["scan_p99_us"].mean()

    t512_get = df[df["group_name"] == "RTC-T512"]["get_live_p99_us"].mean()
    v2a_get = df[df["group_name"] == "RTP-MC-V2A"]["get_live_p99_us"].mean()

    t512_cwa = df[df["group_name"] == "RTC-T512"]["cwa_val_norm_total"].mean()
    v2a_cwa = df[df["group_name"] == "RTP-MC-V2A"]["cwa_val_norm_total"].mean()

    t512_iops = df[df["group_name"] == "RTC-T512"]["fg_db_api_iops"].mean()
    v2a_iops = df[df["group_name"] == "RTP-MC-V2A"]["fg_db_api_iops"].mean()

    print("\n-----------------------------------------------------------------------------------------")
    print("                              Pareto Comparison vs T512")
    print("-----------------------------------------------------------------------------------------")
    scan_diff = ((v2a_scan - t512_scan) / t512_scan) * 100.0
    get_diff = ((v2a_get - t512_get) / t512_get) * 100.0
    cwa_diff = ((v2a_cwa - t512_cwa) / t512_cwa) * 100.0
    iops_diff = ((v2a_iops - t512_iops) / t512_iops) * 100.0

    print(f"Scan P99 Latency:      T512={t512_scan:.2f} us | V2A={v2a_scan:.2f} us | Diff={scan_diff:+.2f}%")
    print(f"GetLive P99 Latency:   T512={t512_get:.2f} us | V2A={v2a_get:.2f} us | Diff={get_diff:+.2f}%")
    print(f"Compaction WA (CWA):   T512={t512_cwa:.4f}    | V2A={v2a_cwa:.4f}    | Diff={cwa_diff:+.2f}%")
    print(f"Foreground IOPS:       T512={t512_iops:.1f}   | V2A={v2a_iops:.1f}   | Diff={iops_diff:+.2f}%")

    is_pareto = (v2a_scan <= t512_scan and v2a_get <= t512_get and v2a_cwa <= t512_cwa and v2a_iops >= t512_iops)
    print(f"\nPareto Dominance: {'YES (V2-A strictly dominates T512)' if is_pareto else 'NO / Mixed Trade-off'}")

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(RAW_DIR, exist_ok=True)

    # Clean existing summary files in target directory
    for f in [SUMMARY_CSV, PHASES_CSV, EVENTS_CSV, WINDOWS_CSV, ACTIONS_CSV]:
        if os.path.exists(f):
            os.remove(f)

    print("=========================================================================================")
    print("     Starting RTP-MC V2-A vs Native T512 Small Core Matrix (N=5 Repetitions)")
    print("=========================================================================================")
    for mode_name, rep_id, config_file, group_name in SCHEDULE:
        run_single(mode_name, rep_id, config_file, group_name)

    analyze_results()

if __name__ == "__main__":
    main()
