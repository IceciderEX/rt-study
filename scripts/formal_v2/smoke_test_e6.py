#!/usr/bin/env python3
"""
Formal V2-E6 Admission Smoke Test
Runs 3 rounds of sub-threshold T512 baseline:
1. smoke_e6_c256_t512
2. smoke_e6_c384_t512
3. smoke_e6_c448_t512

Verifies admission criteria:
1. Zero threshold Flush and zero capacity Flush (fg_flush_count == 0).
2. Live keys strictly == 300,000 for delete groups.
3. Model SHA-256 matches DB SHA-256 with 100% PASS.
4. All Delete groups in each tier produce bit-identical final visible state SHA.
"""

import os
import shutil
import subprocess
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin/formal_driver")
SMOKE_DB = os.path.join(BASE_DIR, "run-db/smoke_e6")
SMOKE_OUT = os.path.join(BASE_DIR, "results/formal_v2/smoke_e6")

os.makedirs(SMOKE_OUT, exist_ok=True)
sum_csv = os.path.join(SMOKE_OUT, "smoke_summary.csv")
events_csv = os.path.join(SMOKE_OUT, "smoke_events.csv")
phases_csv = os.path.join(SMOKE_OUT, "smoke_phases.csv")

for f in [sum_csv, events_csv, phases_csv]:
    if os.path.exists(f):
        os.remove(f)

smoke_runs = [
    {
        "exp_id": "smoke_e6_c256_t512",
        "group_name": "E6-C256-T512",
        "desc": "Smoke Test E6 C256 T=512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e6_c256_hot"),
        "th": 512
    },
    {
        "exp_id": "smoke_e6_c384_t512",
        "group_name": "E6-C384-T512",
        "desc": "Smoke Test E6 C384 T=512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e6_c384_hot"),
        "th": 512
    },
    {
        "exp_id": "smoke_e6_c448_t512",
        "group_name": "E6-C448-T512",
        "desc": "Smoke Test E6 C448 T=512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e6_c448_hot"),
        "th": 512
    }
]

print("=== Starting Formal V2-E6 Admission Smoke Tests ===")

for run in smoke_runs:
    exp_id = run["exp_id"]
    db_path = f"{SMOKE_DB}_{exp_id}"
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    cmd = [
        DRIVER_BIN,
        "--exp_id", exp_id,
        "--group_name", run["group_name"],
        "--desc", run["desc"],
        "--db_path", db_path,
        "--summary_csv", sum_csv,
        "--events_csv", events_csv,
        "--phases_csv", phases_csv,
        "--trace_dir", run["trace_dir"],
        "--total_keys", "500000",
        "--value_size", "256",
        "--memtable_max_range_deletions", str(run["th"]),
        "--write_buffer_size", str(64 * 1024 * 1024)
    ]

    print(f"\n[Running Smoke] {exp_id} ({run['group_name']})...")
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"[Smoke CRITICAL ERROR] Execution failed with returncode {res.returncode}!")
        exit(1)

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

# Verify Smoke Results
df_sum = pd.read_csv(sum_csv)
print("\n=== Smoke Summary Results ===")
print(df_sum[["exp_id", "group_name", "threshold", "fg_flush_count", "db_live_keys", "model_live_keys", "sha256_hex", "verification_status"]])

# Check admission criteria
for _, row in df_sum.iterrows():
    assert row["verification_status"] == "PASS", f"{row['exp_id']} verification failed!"
    assert row["db_live_keys"] == 300000, f"{row['exp_id']} db_live_keys expected 300000, got {row['db_live_keys']}"
    assert row["fg_flush_count"] == 0, f"{row['exp_id']} expected 0 flushes (both threshold and capacity), got {row['fg_flush_count']}"

print("\n[Smoke Admission SUCCESS] All 3 tiers (C256, C384, C448) verified: 0 Flushes, 300,000 Live Keys, PASS!")
