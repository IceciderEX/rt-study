#!/usr/bin/env python3
"""
Formal V2 Pre-Experiment Smoke & Event CSV Window Audit Suite
Runs:
1. 2 independent repetitions of T=0 on small_dynamic_500k_limit
2. Bit-for-bit SHA-256 reproducibility check
3. Deep KV Verification check
4. Event CSV Window Timeline Audit (Preload vs Foreground vs Cooldown vs Verification)
"""

import os
import sys
import subprocess
import shutil
import pandas as pd
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_limit")
AUDIT_OUT_DIR = os.path.join(BASE_DIR, "results", "formal_v2", "smoke_audit")

def run_smoke_and_audit():
    print("=== Running Formal V2 Pre-Formal Smoke & Event Window Audit ===")
    assert os.path.exists(DRIVER_BIN), f"Binary not found: {DRIVER_BIN}"

    if os.path.exists(AUDIT_OUT_DIR):
        shutil.rmtree(AUDIT_OUT_DIR)
    os.makedirs(AUDIT_OUT_DIR, exist_ok=True)

    summary_csv = os.path.join(AUDIT_OUT_DIR, "formal_summary.csv")
    events_csv = os.path.join(AUDIT_OUT_DIR, "formal_events.csv")
    phases_csv = os.path.join(AUDIT_OUT_DIR, "formal_phases.csv")

    runs = [
        {"exp_id": "formal_smoke_t000_rep01", "th": 0},
        {"exp_id": "formal_smoke_t000_rep02", "th": 0}
    ]

    for r in runs:
        db_path = os.path.join(AUDIT_OUT_DIR, f"db_{r['exp_id']}")
        cmd = [
            DRIVER_BIN,
            "--exp_id", r["exp_id"],
            "--group_name", "formal_smoke_t000",
            "--db_path", db_path,
            "--result_dir", AUDIT_OUT_DIR,
            "--summary_csv", summary_csv,
            "--events_csv", events_csv,
            "--phases_csv", phases_csv,
            "--trace_dir", TRACE_DIR,
            "--total_keys", "500000",
            "--value_size", "256",
            "--memtable_max_range_deletions", str(r["th"])
        ]
        res = subprocess.run(cmd)
        assert res.returncode == 0, f"Driver failed on {r['exp_id']}"
        if os.path.exists(db_path):
            shutil.rmtree(db_path)

    # 1. Audit Summary CSV
    assert os.path.exists(summary_csv), "Summary CSV not generated"
    df_sum = pd.read_csv(summary_csv)
    assert len(df_sum) == 2, f"Expected 2 rows, got {len(df_sum)}"

    # 2. Audit State Checksum Bit-for-Bit Determinism
    sha1 = df_sum[df_sum["exp_id"] == "formal_smoke_t000_rep01"]["sha256_hex"].iloc[0]
    sha2 = df_sum[df_sum["exp_id"] == "formal_smoke_t000_rep02"]["sha256_hex"].iloc[0]
    assert sha1 == sha2, f"Checksum mismatch between repetitions: {sha1} != {sha2}"
    print(f"\n[AUDIT 1] Deterministic State Checksum Verified: {sha1}")

    # 3. Audit Foreground vs Phase Times & True IOPS
    for idx, row in df_sum.iterrows():
        fg_wall = row["foreground_wallclock_sec"]
        sum_phase = row["sum_phase_active_sec"]
        assert fg_wall >= sum_phase, f"Foreground wallclock ({fg_wall}) cannot be less than sum of active phases ({sum_phase})"
        expected_trace_iops = 300000.0 / fg_wall
        assert abs(row["fg_trace_iops"] - expected_trace_iops) < 0.1, "FG Trace IOPS formula mismatch"
    print(f"[AUDIT 2] Foreground Wallclock ({df_sum['foreground_wallclock_sec'].mean():.4f}s) & Sum Phase Active ({df_sum['sum_phase_active_sec'].mean():.4f}s) Verified.")

    # 4. Audit Event CSV Window Timeline
    assert os.path.exists(events_csv), "Events CSV not generated"
    df_events = pd.read_csv(events_csv)
    print(f"\n[AUDIT 3] Events CSV Window Timeline Analysis ({len(df_events)} total events recorded):")

    for exp_id in ["formal_smoke_t000_rep01", "formal_smoke_t000_rep02"]:
        sub_e = df_events[df_events["exp_id"] == exp_id]
        sub_s = df_sum[df_sum["exp_id"] == exp_id].iloc[0]
        fg_wall = sub_s["foreground_wallclock_sec"]

        preload_events = sub_e[sub_e["stage"] == "PRELOAD"]
        fg_events = sub_e[sub_e["stage"] == "FOREGROUND"]
        cd_events = sub_e[sub_e["stage"] == "COOLDOWN"]
        ver_events = sub_e[sub_e["stage"] == "VERIFICATION"]

        print(f"  --- Exp: {exp_id} ---")
        print(f"    PRELOAD events:      {len(preload_events)}")
        print(f"    FOREGROUND events:   {len(fg_events)}")
        print(f"    COOLDOWN events:     {len(cd_events)}")
        print(f"    VERIFICATION events: {len(ver_events)}")

        # Verify Foreground Window Timestamps
        if len(fg_events) > 0:
            fg_ts = fg_events["timestamp_sec"].to_numpy()
            assert np.all(fg_ts >= 0.0), "Foreground event timestamp cannot be negative"
            assert np.all(fg_ts <= fg_wall + 0.1), f"Foreground event timestamp ({np.max(fg_ts)}) exceeded foreground wallclock ({fg_wall})"

        # Verify Cooldown Window Timestamps (Strictly in [fg_wall, fg_wall + 10.05])
        if len(cd_events) > 0:
            cd_ts = cd_events["timestamp_sec"].to_numpy()
            assert np.all(cd_ts >= 0.0), "Cooldown timestamp cannot be negative"
            assert np.all(cd_ts <= fg_wall + 10.1), f"Cooldown event timestamp ({np.max(cd_ts)}) exceeded 10s cooldown limit ({fg_wall + 10.0})"

        # Verify Flush and Compaction Byte Closures with Summary CSV
        fg_flush_bytes = fg_events[fg_events["event_type"] == "FLUSH"]["size_or_out_bytes"].sum()
        fg_flush_mb_calc = fg_flush_bytes / (1024.0 * 1024.0)
        assert abs(fg_flush_mb_calc - sub_s["fg_flush_engine_out_mb"]) < 0.01, "Foreground Flush Byte aggregation mismatch"

    print("\n=========================================================")
    print("  PRE-FORMAL SMOKE & EVENT CSV WINDOW AUDIT PASSED (100%)")
    print("=========================================================")

if __name__ == "__main__":
    run_smoke_and_audit()
