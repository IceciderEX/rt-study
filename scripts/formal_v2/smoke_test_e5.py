#!/usr/bin/env python3
"""
Formal V2-E5 Smoke Test
Runs 1 round of Hot-T512 and 1 round of Cold-T512 to verify admission criteria:
1. No tombstone-triggered Flush occurs (Flush count == 0).
2. No write-capacity natural MemTable Flush occurs.
3. Live keys == 300,000 and SHA-256 verification PASS.
4. Hot and Cold have 100% bit-identical Put and Delete projections.
"""

import os
import subprocess
import shutil
import pandas as pd

DRIVER_BIN = "/home/wam/grad/s14-range-delete-study/bin/formal_driver"
BASE_DIR = "/home/wam/grad/s14-range-delete-study"
SMOKE_DB = "/home/wam/grad/s14-range-delete-study/run-db/smoke_e5"
SMOKE_OUT = "/home/wam/grad/s14-range-delete-study/results/formal_v2/smoke_e5"

os.makedirs(SMOKE_OUT, exist_ok=True)
sum_csv = os.path.join(SMOKE_OUT, "smoke_summary.csv")
events_csv = os.path.join(SMOKE_OUT, "smoke_events.csv")
phases_csv = os.path.join(SMOKE_OUT, "smoke_phases.csv")

if os.path.exists(sum_csv): os.remove(sum_csv)
if os.path.exists(events_csv): os.remove(events_csv)
if os.path.exists(phases_csv): os.remove(phases_csv)

smoke_runs = [
    {
        "exp_id": "smoke_e5_hot_t512",
        "group_name": "E5-Hot-T512",
        "desc": "Smoke Test E5 Hot T=512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_hot"),
        "th": 512,
        "oracle": False
    },
    {
        "exp_id": "smoke_e5_cold_t512",
        "group_name": "E5-Cold-T512",
        "desc": "Smoke Test E5 Cold T=512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_cold"),
        "th": 512,
        "oracle": False
    }
]

print("=== Starting Formal V2-E5 Admission Smoke Tests ===")

for run in smoke_runs:
    exp_id = run["exp_id"]
    db_path = f"{SMOKE_DB}_{exp_id}"
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    cmd = [
        DRIVER_BIN,
        "--exp_id", exp_id,
        "--group_name", run["group_name"],
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
    if run["oracle"]:
        cmd.append("--oracle_flush_after_phase_b")

    print(f"\n[Running Smoke] {exp_id} ({run['group_name']})...")
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"[Smoke CRITICAL ERROR] Execution failed with returncode {res.returncode}!")
        exit(1)

# Verify Smoke Results
df_sum = pd.read_csv(sum_csv)
print("\n=== Smoke Summary Results ===")
print(df_sum[["exp_id", "group_name", "threshold", "fg_flush_count", "db_live_keys", "model_live_keys", "sha256_hex", "verification_status"]])

# Check admission criteria
for _, row in df_sum.iterrows():
    assert row["verification_status"] == "PASS", f"{row['exp_id']} verification failed!"
    assert row["db_live_keys"] == 300000, f"{row['exp_id']} db_live_keys expected 300000, got {row['db_live_keys']}"
    assert row["fg_flush_count"] == 0, f"{row['exp_id']} expected 0 flushes, got {row['fg_flush_count']}"

# Check that Hot and Cold have bit-identical final DB SHA-256
hot_sha = df_sum[df_sum["exp_id"] == "smoke_e5_hot_t512"]["sha256_hex"].values[0]
cold_sha = df_sum[df_sum["exp_id"] == "smoke_e5_cold_t512"]["sha256_hex"].values[0]
assert hot_sha == cold_sha, f"Final DB SHA mismatch! Hot={hot_sha}, Cold={cold_sha}"

print(f"\n[Smoke Admission SUCCESS] Final DB SHA: {hot_sha} (100% bit-identical match across Hot and Cold!)")
print("[Smoke Admission SUCCESS] All criteria met: 0 Flushes, 300,000 Live Keys, PASS!")
