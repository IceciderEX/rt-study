#!/usr/bin/env python3
"""
Formal V2-E5 Matrix Runner
Executes 50 formal runs (10 conditions x 5 repetitions) in interleaved randomized order.
Records all results to results/summary/formal-v2-e5-hot-cold.csv,
results/formal_v2/e5_hot_cold/formal_events.csv,
and results/formal_v2/e5_hot_cold/formal_phases.csv.
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
RUN_DB_DIR = os.path.join(BASE_DIR, "run-db/e5_matrix")
RESULTS_DIR = os.path.join(BASE_DIR, "results/formal_v2/e5_hot_cold")
SUMMARY_DIR = os.path.join(BASE_DIR, "results/summary")

os.makedirs(RUN_DB_DIR, exist_ok=True)
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(SUMMARY_DIR, exist_ok=True)

summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e5-hot-cold.csv")
events_csv = os.path.join(RESULTS_DIR, "formal_events.csv")
phases_csv = os.path.join(RESULTS_DIR, "formal_phases.csv")
manifest_json = os.path.join(RESULTS_DIR, "e5_execution_manifest.json")

# Clean existing outputs if starting fresh
for f in [summary_csv, events_csv, phases_csv]:
    if os.path.exists(f):
        os.remove(f)

def compute_file_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

driver_sha = compute_file_sha256(DRIVER_BIN)
print(f"[E5 Matrix] Driver Binary SHA-256: {driver_sha}")

CONDITIONS = [
    {
        "group_name": "E5-Hot-CLEAN",
        "locality": "Hot",
        "strategy": "CLEAN",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_clean_hot"),
        "th": 0,
        "oracle": False,
        "desc": "Formal V2 E5 Hot CLEAN Functional Baseline"
    },
    {
        "group_name": "E5-Cold-CLEAN",
        "locality": "Cold",
        "strategy": "CLEAN",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_clean_cold"),
        "th": 0,
        "oracle": False,
        "desc": "Formal V2 E5 Cold CLEAN Functional Baseline"
    },
    {
        "group_name": "E5-Hot-T0",
        "locality": "Hot",
        "strategy": "T0",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_hot"),
        "th": 0,
        "oracle": False,
        "desc": "Formal V2 E5 Hot Default T0 Baseline"
    },
    {
        "group_name": "E5-Cold-T0",
        "locality": "Cold",
        "strategy": "T0",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_cold"),
        "th": 0,
        "oracle": False,
        "desc": "Formal V2 E5 Cold Default T0 Baseline"
    },
    {
        "group_name": "E5-Hot-T256",
        "locality": "Hot",
        "strategy": "T256",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_hot"),
        "th": 256,
        "oracle": False,
        "desc": "Formal V2 E5 Hot Aggressive T256 Strategy"
    },
    {
        "group_name": "E5-Cold-T256",
        "locality": "Cold",
        "strategy": "T256",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_cold"),
        "th": 256,
        "oracle": False,
        "desc": "Formal V2 E5 Cold Aggressive T256 Strategy"
    },
    {
        "group_name": "E5-Hot-T512",
        "locality": "Hot",
        "strategy": "T512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_hot"),
        "th": 512,
        "oracle": False,
        "desc": "Formal V2 E5 Hot Sub-Threshold T512 Strategy"
    },
    {
        "group_name": "E5-Cold-T512",
        "locality": "Cold",
        "strategy": "T512",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_cold"),
        "th": 512,
        "oracle": False,
        "desc": "Formal V2 E5 Cold Sub-Threshold T512 Strategy"
    },
    {
        "group_name": "E5-Hot-OracleFlush",
        "locality": "Hot",
        "strategy": "OracleFlush",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_hot"),
        "th": 0,
        "oracle": True,
        "desc": "Formal V2 E5 Hot Oracle Synchronous Flush"
    },
    {
        "group_name": "E5-Cold-OracleFlush",
        "locality": "Cold",
        "strategy": "OracleFlush",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e5_cold"),
        "th": 0,
        "oracle": True,
        "desc": "Formal V2 E5 Cold Oracle Synchronous Flush"
    }
]

# Generate interleaved execution schedule
NUM_REPETITIONS = 5
RANDOM_SEED = 50001
execution_schedule = []

for rep in range(1, NUM_REPETITIONS + 1):
    rep_conds = list(CONDITIONS)
    rng = random.Random(RANDOM_SEED + rep * 1000)
    rng.shuffle(rep_conds)
    for c in rep_conds:
        exp_id = f"formal_e5_{c['group_name'].lower().replace('-', '_')}_rep{rep:02d}"
        item = dict(c)
        item["exp_id"] = exp_id
        item["rep"] = rep
        execution_schedule.append(item)

print(f"[E5 Matrix] Generated schedule with {len(execution_schedule)} runs across {NUM_REPETITIONS} repetitions.")

manifest_data = {
    "campaign": "FormalV2-E5 Hot/Cold Read Pressure Discrimination",
    "driver_binary_sha256": driver_sha,
    "start_time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "num_runs": len(execution_schedule),
    "schedule": execution_schedule
}

with open(manifest_json, "w", encoding="utf-8") as f:
    json.dump(manifest_data, f, indent=2)

# Execute the 50 runs sequentially
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
    print(f"  Locality: {task['locality']}, Strategy: {task['strategy']}, Threshold: {task['th']}, Oracle: {task['oracle']}")
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
        print(f"[E5 Matrix CRITICAL ERROR] Run {exp_id} failed with exit code {res.returncode}!")
        failed_runs.append((exp_id, res.returncode))
        break

    # Clean up run DB to preserve disk space after verified success
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"  [Run {run_idx:02d}/{total_runs:02d} DONE] Completed in {t_elapsed:.2f} s")

if failed_runs:
    print(f"\n[E5 Matrix CRITICAL ERROR] {len(failed_runs)} runs failed! Stopped.")
    sys.exit(1)

print("\n========================================================")
print("  ALL 50 FORMAL V2-E5 RUNS SUCCESSFULLY COMPLETED!")
print(f"  Summary CSV: {summary_csv}")
print(f"  Phases CSV:  {phases_csv}")
print(f"  Events CSV:  {events_csv}")
print("========================================================")
