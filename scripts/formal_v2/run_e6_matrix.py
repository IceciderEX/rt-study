#!/usr/bin/env python3
"""
Formal V2-E6 Matrix Runner
Executes 63 runs (60 formal runs + 3 C448-T0 equivalence audit runs) in interleaved randomized order.
Records all results to:
- results/summary/formal-v2-e6-under-threshold-recovery.csv
- results/summary/formal-v2-e6-phase-breakdown.csv
- results/formal_v2/e6_under_threshold_recovery/formal_events.csv
- results/formal_v2/e6_under_threshold_recovery/formal_phases.csv
"""

import os
import sys
import json
import random
import shutil
import hashlib
import subprocess
import time
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin/formal_driver")
RUN_DB_DIR = os.path.join(BASE_DIR, "run-db/e6_matrix")
RESULTS_DIR = os.path.join(BASE_DIR, "results/formal_v2/e6_under_threshold_recovery")
SUMMARY_DIR = os.path.join(BASE_DIR, "results/summary")

os.makedirs(RUN_DB_DIR, exist_ok=True)
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(SUMMARY_DIR, exist_ok=True)

summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e6-under-threshold-recovery.csv")
phases_summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e6-phase-breakdown.csv")
events_csv = os.path.join(RESULTS_DIR, "formal_events.csv")
phases_csv = os.path.join(RESULTS_DIR, "formal_phases.csv")
manifest_json = os.path.join(RESULTS_DIR, "e6_execution_manifest.json")

# Clean existing outputs if starting fresh
for f in [summary_csv, phases_summary_csv, events_csv, phases_csv]:
    if os.path.exists(f):
        os.remove(f)

def compute_file_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

driver_sha = compute_file_sha256(DRIVER_BIN)
print(f"[E6 Matrix] Driver Binary SHA-256: {driver_sha}")

TIERS = ["c256", "c384", "c448"]
STRATEGIES = [
    {"strat": "CLEAN", "th": 0, "oracle": False, "is_clean": True},
    {"strat": "T256", "th": 256, "oracle": False, "is_clean": False},
    {"strat": "T512", "th": 512, "oracle": False, "is_clean": False},
    {"strat": "OracleFlush", "th": 0, "oracle": True, "is_clean": False}
]

CONDITIONS = []
for tier in TIERS:
    tier_upper = tier.upper()
    for s in STRATEGIES:
        trace_suffix = "clean_hot" if s["is_clean"] else "hot"
        trace_dir = os.path.join(BASE_DIR, f"traces/formal_v2/e6_{tier}_{trace_suffix}")
        group_name = f"E6-{tier_upper}-{s['strat']}"
        desc = f"Formal V2 E6 {tier_upper} {s['strat']}"
        CONDITIONS.append({
            "group_name": group_name,
            "tier": tier_upper,
            "strategy": s["strat"],
            "trace_dir": trace_dir,
            "th": s["th"],
            "oracle": s["oracle"],
            "desc": desc,
            "is_audit": False
        })

# Generate 60 formal runs (12 conditions x 5 reps)
NUM_REPETITIONS = 5
RANDOM_SEED = 60001
execution_schedule = []

for rep in range(1, NUM_REPETITIONS + 1):
    rep_conds = list(CONDITIONS)
    rng = random.Random(RANDOM_SEED + rep * 1000)
    rng.shuffle(rep_conds)
    for c in rep_conds:
        exp_id = f"formal_e6_{c['group_name'].lower().replace('-', '_')}_rep{rep:02d}"
        item = dict(c)
        item["exp_id"] = exp_id
        item["rep"] = rep
        execution_schedule.append(item)

# Append 3 C448-T0 equivalence audit runs
for rep in range(1, 4):
    exp_id = f"formal_e6_c448_t0_audit_rep{rep:02d}"
    execution_schedule.append({
        "exp_id": exp_id,
        "group_name": "E6-C448-T0-Audit",
        "tier": "C448",
        "strategy": "T0-Audit",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e6_c448_hot"),
        "th": 0,
        "oracle": False,
        "desc": "Formal V2 E6 C448 T0 Equivalence Audit",
        "rep": rep,
        "is_audit": True
    })

print(f"[E6 Matrix] Generated schedule with {len(execution_schedule)} total runs (60 formal + 3 audit).")

manifest_data = {
    "campaign": "FormalV2-E6 Sub-Threshold Deletion Cessation and Read Recovery",
    "driver_binary_sha256": driver_sha,
    "start_time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "num_runs": len(execution_schedule),
    "schedule": execution_schedule
}

with open(manifest_json, "w", encoding="utf-8") as f:
    json.dump(manifest_data, f, indent=2)

# Execute the 63 runs sequentially
total_runs = len(execution_schedule)
failed_runs = []

for run_idx, task in enumerate(execution_schedule, 1):
    exp_id = task["exp_id"]
    group = task["group_name"]
    db_path = os.path.join(RUN_DB_DIR, f"db_{exp_id}")

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"\n========================================================")
    print(f"  [Run {run_idx:02d}/{total_runs:02d}] {exp_id} ({group})")
    print(f"  Tier: {task['tier']}, Strategy: {task['strategy']}, Threshold: {task['th']}, Oracle: {task['oracle']}")
    print(f"========================================================")

    cmd = [
        DRIVER_BIN,
        "--exp_id", exp_id,
        "--group_name", group,
        "--desc", task["desc"],
        "--db_path", db_path,
        "--summary_csv", summary_csv,
        "--events_csv", events_csv,
        "--phases_csv", phases_csv,
        "--trace_dir", task["trace_dir"],
        "--total_keys", "500000",
        "--value_size", "256",
        "--memtable_max_range_deletions", str(task["th"]),
        "--write_buffer_size", str(64 * 1024 * 1024)
    ]
    if task["oracle"]:
        cmd.append("--oracle_flush_after_phase_b")

    t_start = time.time()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t_elapsed = time.time() - t_start

    print(res.stdout)

    if res.returncode != 0:
        print(f"[E6 Matrix CRITICAL ERROR] Run {exp_id} failed with exit code {res.returncode}!")
        failed_runs.append((exp_id, res.returncode))
        break

    # Clean up run DB to preserve disk space after verified success
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"  [Run {run_idx:02d}/{total_runs:02d} DONE] Completed in {t_elapsed:.2f} s")

if failed_runs:
    print(f"\n[E6 Matrix CRITICAL ERROR] {len(failed_runs)} runs failed! Stopped.")
    sys.exit(1)

# Sync phases CSV to summary directory as well
if os.path.exists(phases_csv):
    shutil.copyfile(phases_csv, phases_summary_csv)

print("\n========================================================")
print("  ALL 63 FORMAL V2-E6 RUNS SUCCESSFULLY COMPLETED!")
print(f"  Summary CSV:        {summary_csv}")
print(f"  Phases Summary CSV: {phases_summary_csv}")
print(f"  Events CSV:         {events_csv}")
print("========================================================")
