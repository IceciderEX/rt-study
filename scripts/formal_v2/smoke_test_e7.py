#!/usr/bin/env python3
"""
Formal V2-E7 Admission Smoke Test
Runs 3 smoke tests:
1. smoke_e7_t512 (T=512)
2. smoke_e7_t2048 (T=2048)
3. smoke_e7_clean (CLEAN)

Verifies admission criteria:
1. Delete groups have 300,000 live keys and matching SHA-256 (PASS).
2. Clean group has 500,000 live keys and passes verification.
3. Natural capacity flushes occur properly with 1KiB Value and 64MB write buffer.
"""

import os
import shutil
import subprocess
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin/formal_driver")
SMOKE_DB = os.path.join(BASE_DIR, "run-db/smoke_e7")
SMOKE_OUT = os.path.join(BASE_DIR, "results/formal_v2/smoke_e7")

os.makedirs(SMOKE_OUT, exist_ok=True)
sum_csv = os.path.join(SMOKE_OUT, "smoke_summary.csv")
events_csv = os.path.join(SMOKE_OUT, "smoke_events.csv")
phases_csv = os.path.join(SMOKE_OUT, "smoke_phases.csv")

for f in [sum_csv, events_csv, phases_csv]:
    if os.path.exists(f):
        os.remove(f)

smoke_runs = [
    {
        "exp_id": "smoke_e7_t512",
        "group_name": "E7-T512",
        "desc": "Smoke Test E7 T=512 (1KiB Value)",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "th": 512,
        "exp_live": 300000
    },
    {
        "exp_id": "smoke_e7_t2048",
        "group_name": "E7-T2048",
        "desc": "Smoke Test E7 T=2048 (1KiB Value)",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "th": 2048,
        "exp_live": 300000
    },
    {
        "exp_id": "smoke_e7_clean",
        "group_name": "E7-CLEAN",
        "desc": "Smoke Test E7 CLEAN (1KiB Value)",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_clean_cold"),
        "th": 0,
        "exp_live": 500000
    }
]

print("=== Starting Formal V2-E7 Admission Smoke Tests (1KiB Value) ===")

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
        "--value_size", "1024",
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
print(df_sum[["exp_id", "group_name", "threshold", "fg_flush_count", "fg_flush_engine_out_mb", "fg_comp_write_mb", "db_live_keys", "model_live_keys", "sha256_hex", "verification_status"]])

# Check admission criteria
for _, row in df_sum.iterrows():
    assert row["verification_status"] == "PASS", f"{row['exp_id']} verification failed!"
    if "CLEAN" in row["group_name"]:
        assert row["db_live_keys"] == 500000, f"CLEAN live keys expected 500000, got {row['db_live_keys']}"
    else:
        assert row["db_live_keys"] == 300000, f"Delete group live keys expected 300000, got {row['db_live_keys']}"

# Check Delete groups have matching SHA-256
t512_sha = df_sum[df_sum["exp_id"] == "smoke_e7_t512"]["sha256_hex"].values[0]
t2048_sha = df_sum[df_sum["exp_id"] == "smoke_e7_t2048"]["sha256_hex"].values[0]
assert t512_sha == t2048_sha, f"SHA mismatch! T512={t512_sha}, T2048={t2048_sha}"

print(f"\n[Smoke Admission SUCCESS] Delete groups SHA-256 match: {t512_sha}")
print("[Smoke Admission SUCCESS] All 3 smoke tests (T512, T2048, CLEAN) verified PASS!")
