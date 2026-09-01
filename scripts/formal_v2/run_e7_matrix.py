#!/usr/bin/env python3
"""
Formal V2-E7 Matrix Runner
Executes 23 runs (20 formal runs + 3 T0 equivalence audit runs) in interleaved randomized order.
Records all results to:
- results/summary/formal-v2-e7-cold-write-pressure.csv
- results/summary/formal-v2-e7-phase-breakdown.csv
- results/summary/formal-v2-e7-flush-events.csv
- results/formal_v2/e7_cold_write_pressure/formal_events.csv
- results/formal_v2/e7_cold_write_pressure/formal_phases.csv
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
RUN_DB_DIR = os.path.join(BASE_DIR, "run-db/e7_matrix")
RESULTS_DIR = os.path.join(BASE_DIR, "results/formal_v2/e7_cold_write_pressure")
SUMMARY_DIR = os.path.join(BASE_DIR, "results/summary")

os.makedirs(RUN_DB_DIR, exist_ok=True)
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(SUMMARY_DIR, exist_ok=True)

summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e7-cold-write-pressure.csv")
phases_summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e7-phase-breakdown.csv")
flush_events_summary_csv = os.path.join(SUMMARY_DIR, "formal-v2-e7-flush-events.csv")
events_csv = os.path.join(RESULTS_DIR, "formal_events.csv")
phases_csv = os.path.join(RESULTS_DIR, "formal_phases.csv")
manifest_json = os.path.join(RESULTS_DIR, "e7_execution_manifest.json")

# Clean existing outputs if starting fresh
for f in [summary_csv, phases_summary_csv, flush_events_summary_csv, events_csv, phases_csv]:
    if os.path.exists(f):
        os.remove(f)

def compute_file_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

driver_sha = compute_file_sha256(DRIVER_BIN)
print(f"[E7 Matrix] Driver Binary SHA-256: {driver_sha}")

CONDITIONS = [
    {
        "group_name": "E7-CLEAN",
        "strategy": "CLEAN",
        "th": 0,
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_clean_cold"),
        "desc": "Formal V2 E7 CLEAN High Write Pressure Reference",
        "is_audit": False
    },
    {
        "group_name": "E7-T256",
        "strategy": "T256",
        "th": 256,
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "desc": "Formal V2 E7 T256 Aggressive Static Threshold",
        "is_audit": False
    },
    {
        "group_name": "E7-T512",
        "strategy": "T512",
        "th": 512,
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "desc": "Formal V2 E7 T512 Default Static Threshold Baseline",
        "is_audit": False
    },
    {
        "group_name": "E7-T2048",
        "strategy": "T2048",
        "th": 2048,
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "desc": "Formal V2 E7 T2048 Conservative Static Threshold",
        "is_audit": False
    }
]

NUM_REPETITIONS = 5
RANDOM_SEED = 70001
execution_schedule = []

for rep in range(1, NUM_REPETITIONS + 1):
    rep_conds = list(CONDITIONS)
    rng = random.Random(RANDOM_SEED + rep * 1000)
    rng.shuffle(rep_conds)
    for c in rep_conds:
        exp_id = f"formal_e7_{c['group_name'].lower().replace('-', '_')}_rep{rep:02d}"
        item = dict(c)
        item["exp_id"] = exp_id
        item["rep"] = rep
        execution_schedule.append(item)

# Append 3 T0 equivalence audit runs
for rep in range(1, 4):
    exp_id = f"formal_e7_t0_audit_rep{rep:02d}"
    execution_schedule.append({
        "exp_id": exp_id,
        "group_name": "E7-T0-Audit",
        "strategy": "T0-Audit",
        "trace_dir": os.path.join(BASE_DIR, "traces/formal_v2/e7_c768_cold"),
        "th": 0,
        "desc": "Formal V2 E7 T0 Equivalence Audit",
        "rep": rep,
        "is_audit": True
    })

print(f"[E7 Matrix] Generated schedule with {len(execution_schedule)} total runs (20 formal + 3 audit).")

manifest_data = {
    "campaign": "FormalV2-E7 Cold Read Access and Heavy Write Pressure Static Flush Excessive Maintenance",
    "driver_binary_sha256": driver_sha,
    "start_time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "num_runs": len(execution_schedule),
    "schedule": execution_schedule
}

with open(manifest_json, "w", encoding="utf-8") as f:
    json.dump(manifest_data, f, indent=2)

# Execute the 23 runs sequentially
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
    print(f"  Strategy: {task['strategy']}, Threshold: {task['th']}")
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
        "--value_size", "1024",
        "--memtable_max_range_deletions", str(task["th"]),
        "--write_buffer_size", str(64 * 1024 * 1024)
    ]

    t_start = time.time()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t_elapsed = time.time() - t_start

    print(res.stdout)

    if res.returncode != 0:
        print(f"[E7 Matrix CRITICAL ERROR] Run {exp_id} failed with exit code {res.returncode}!")
        failed_runs.append((exp_id, res.returncode))
        break

    # Clean up run DB to preserve disk space after verified success
    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"  [Run {run_idx:02d}/{total_runs:02d} DONE] Completed in {t_elapsed:.2f} s")

if failed_runs:
    print(f"\n[E7 Matrix CRITICAL ERROR] {len(failed_runs)} runs failed! Stopped.")
    sys.exit(1)

# Sync phases CSV and extract flush events to summary directory
if os.path.exists(phases_csv):
    shutil.copyfile(phases_csv, phases_summary_csv)

if os.path.exists(events_csv):
    shutil.copyfile(events_csv, flush_events_summary_csv)

print("\n========================================================")
print("  ALL 23 FORMAL V2-E7 RUNS SUCCESSFULLY COMPLETED!")
print(f"  Summary CSV:        {summary_csv}")
print(f"  Phases Summary CSV: {phases_summary_csv}")
print(f"  Flush Events CSV:   {flush_events_summary_csv}")
print("========================================================")
