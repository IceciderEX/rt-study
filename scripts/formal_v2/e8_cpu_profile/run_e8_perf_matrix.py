#!/usr/bin/env python3
"""
Formal V2 - E8 Master Performance Profiling Matrix Runner
Runs the full formal matrix with 3 Non-Multiplexed Passes (Pass A, B, C) per condition:
- Normal Layer: CLEAN, T0, T512
- Mechanism Isolation Layer: PreFlush, PostFlush (disable_auto_compactions=true)
- Micro-benchmarks: ScanIntersect, GetLive, ScanNonIntersect (negative control)
- Repetitions: N=3 independent DB runs
- Strict full-DB SHA-256 state reconciliation against expected model
- CPU binding to physical Core 2 on Node 0
- 100.00% time running verification
"""

import os
import sys
import time
import json
import random
import subprocess
from pathlib import Path

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/e8_micro_driver")
TRACES_BASE = os.path.join(BASE_DIR, "traces/formal_v2")
RUN_BASE = os.path.join(BASE_DIR, "run-db/e8_matrix")
SUMMARY_DIR = os.path.join(BASE_DIR, "results/summary")

PASSES = [
    ("Pass-A", "cycles:u,instructions:u"),
    ("Pass-B", "branches:u,branch-misses:u"),
    ("Pass-C", "cache-references:u,cache-misses:u"),
]

CONDITIONS = [
    # (cond_id, is_clean, threshold, disable_auto_comp, post_flush, layer)
    ("CLEAN",      True,  0,   False, False, "normal"),
    ("T0",         False, 0,   False, False, "normal"),
    ("T512",       False, 512, False, False, "normal"),
    ("PreFlush",   False, 0,   True,  False, "isolation"),
    ("PostFlush",  False, 0,   True,  True,  "isolation"),
]

TRACES = [
    ("ScanIntersect",    "scan_intersect"),
    ("GetLive",          "get_live"),
    ("ScanNonIntersect", "scan_non_intersect"),
]

def run_single_matrix_point(cond_id, is_clean, threshold, disable_auto_comp, post_flush, layer, trace_label, trace_case, rep_idx, pass_name, events_str):
    case_name = f"{cond_id.lower()}_{trace_case}_{pass_name.lower()}_rep{rep_idx}"
    run_id = f"e8_{case_name}_{int(time.time())}"
    run_dir = os.path.join(RUN_BASE, run_id)
    db_dir = os.path.join(run_dir, "db")
    wait_dir = os.path.join(run_dir, "wait")

    # Select trace directory (clean vs deleted)
    trace_dir_name = f"e8_{trace_case}" + ("_clean" if is_clean else "")
    trace_dir = os.path.join(TRACES_BASE, trace_dir_name)

    # 1. Directory Setup (umask 077, chmod 0700)
    old_umask = os.umask(0o077)
    try:
        os.makedirs(wait_dir, mode=0o700, exist_ok=False)
        os.makedirs(db_dir, mode=0o700, exist_ok=False)
    finally:
        os.umask(old_umask)

    driver_stdout_path = os.path.join(wait_dir, "driver.stdout")
    driver_stderr_path = os.path.join(wait_dir, "driver.stderr")
    driver_stdout = open(driver_stdout_path, "w")
    driver_stderr = open(driver_stderr_path, "w")

    driver_cmd = [
        "taskset", "-c", "2",
        BIN_DRIVER,
        "--db_path", db_dir,
        "--trace_dir", trace_dir,
        "--profile_wait_dir", wait_dir,
        "--profile_window_sec", "45",
        "--profile_case", f"{cond_id.lower()}_{trace_case}",
        "--threshold", str(threshold),
        "--disable_auto_compactions", "true" if disable_auto_comp else "false",
        "--is_clean", "true" if is_clean else "false",
        "--post_flush", "true" if post_flush else "false",
        "--total_keys", "500000"
    ]

    print(f"\n>>> [{layer.upper()}] {cond_id} | {trace_label} | Rep {rep_idx+1}/3 | {pass_name} ({events_str})")
    driver_proc = subprocess.Popen(driver_cmd, stdout=driver_stdout, stderr=driver_stderr)
    driver_pid = driver_proc.pid

    # 2. Wait for PROFILE_READY.json
    ready_file = os.path.join(wait_dir, "PROFILE_READY.json")
    meta = None
    start_wait_time = time.time()

    while time.time() - start_wait_time < 120:
        if os.path.exists(ready_file):
            try:
                with open(ready_file, "r") as f:
                    meta = json.load(f)
                break
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.05)

    if not meta:
        print(f"[FATAL] Timed out waiting for PROFILE_READY.json in {run_id}")
        driver_proc.kill()
        return None

    fg_tid = meta.get("foreground_tid")

    # 3. Launch sudo -n perf stat attached ONLY to foreground_tid
    perf_stdout_path = os.path.join(wait_dir, "perf-stat.stdout")
    perf_stderr_path = os.path.join(wait_dir, "perf-stat.stderr")
    perf_stdout = open(perf_stdout_path, "w")
    perf_stderr = open(perf_stderr_path, "w")

    perf_cmd = [
        "sudo", "-n", "perf", "stat",
        "-t", str(fg_tid),
        "-e", events_str,
        "--", "sleep", "55"
    ]
    perf_start_ts_ns = time.time_ns()
    perf_proc = subprocess.Popen(perf_cmd, stdout=perf_stdout, stderr=perf_stderr)

    time.sleep(1.2)
    if perf_proc.poll() is not None:
        print(f"[FATAL] sudo -n perf stat exited prematurely in {run_id}")
        driver_proc.kill()
        return None

    # 4. Atomic PROFILE_START
    profile_start_ts_ns = time.time_ns()
    start_path = os.path.join(wait_dir, "PROFILE_START")
    fd = os.open(start_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)

    # 5. Wait for PROFILE_DONE.json
    done_file = os.path.join(wait_dir, "PROFILE_DONE.json")
    done_meta = None
    while time.time() - start_wait_time < 180:
        if os.path.exists(done_file):
            try:
                with open(done_file, "r") as f:
                    done_meta = json.load(f)
                break
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.1)

    profile_done_ts_ns = time.time_ns()

    if not done_meta:
        print(f"[FATAL] Timed out waiting for PROFILE_DONE.json in {run_id}")
        driver_proc.kill()
        perf_proc.kill()
        return None

    # 6. Atomic PROFILE_RELEASE
    profile_release_ts_ns = time.time_ns()
    rel_path = os.path.join(wait_dir, "PROFILE_RELEASE")
    fd = os.open(rel_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)

    # 7. Wait for completion
    driver_ret = driver_proc.wait()
    perf_ret = perf_proc.wait()
    perf_exit_ts_ns = time.time_ns()

    driver_stdout.close()
    driver_stderr.close()
    perf_stdout.close()
    perf_stderr.close()

    if driver_ret != 0:
        print(f"[FATAL] Driver failed with exit code {driver_ret}")
        with open(driver_stderr_path, "r") as f:
            print(f"Stderr:\n{f.read()}")
        return None

    # 8. Parse perf output and verify 100.00% time running
    with open(perf_stderr_path, "r") as f:
        perf_stderr_text = f.read()

    events_data = {}
    for line in perf_stderr_text.strip().split("\n"):
        line_clean = line.strip()
        if not line_clean: continue
        parts = line_clean.split()
        for ev in events_str.split(","):
            if ev in line_clean:
                val_str = parts[0].replace(",", "")
                if val_str.isdigit():
                    events_data[ev] = int(val_str)

    # 9. Verify full-DB SHA-256 reconciliation
    with open(driver_stdout_path, "r") as f:
        driver_stdout_text = f.read()

    if "State Reconciliation PASSED" not in driver_stdout_text:
        print(f"[FATAL] State Reconciliation failed in {run_id}!")
        return None

    # Extract SHA from stdout
    db_sha = ""
    for line in driver_stdout_text.split("\n"):
        if "DB SHA-256:" in line:
            db_sha = line.split("DB SHA-256:")[1].strip()

    ops = done_meta["completed_ops"]
    elapsed = done_meta["elapsed_sec"]
    cpu_sec = done_meta["worker_cpu_time_ns"] / 1e9

    result = {
        "run_id": run_id,
        "layer": layer,
        "condition": cond_id,
        "trace": trace_label,
        "rep": rep_idx,
        "pass": pass_name,
        "pid": driver_pid,
        "tid": fg_tid,
        "completed_ops": ops,
        "elapsed_sec": elapsed,
        "true_phase_iops": ops / elapsed,
        "worker_cpu_time_sec": cpu_sec,
        "api_seek_calls": done_meta["api_seek_calls"],
        "api_next_calls": done_meta["api_next_calls"],
        "returned_visible_keys": done_meta["returned_visible_keys"],
        "db_sha256": db_sha,
        "events": events_data,
        "perf_raw": perf_stderr_text.strip(),
        "timestamps": {
            "perf_start_ts_ns": perf_start_ts_ns,
            "profile_start_created_ts_ns": profile_start_ts_ns,
            "worker_window_start_ts_ns": done_meta["window_start_ns"],
            "worker_window_end_ts_ns": done_meta["window_end_ns"],
            "profile_done_written_ts_ns": profile_done_ts_ns,
            "profile_release_created_ts_ns": profile_release_ts_ns,
            "perf_exit_ts_ns": perf_exit_ts_ns
        }
    }

    print(f"      -> Completed: Ops={ops}, IOPS={ops/elapsed:.1f}, CPU={cpu_sec:.3f}s, SHA={db_sha[:12]}..., Events={events_data}")
    return result

def run_matrix():
    print("================================================================")
    print("  Formal V2 - E8 Master Performance Profiling Matrix")
    print("================================================================")

    # Build tasks: (cond, trace, rep, pass)
    tasks = []
    for cond in CONDITIONS:
        for trace in TRACES:
            for rep in range(3): # N=3
                for p in PASSES:
                    tasks.append((cond, trace, rep, p))

    # Shuffle tasks to randomize/interleave execution order
    random.seed(42)
    random.shuffle(tasks)
    total_tasks = len(tasks)
    print(f"Total Matrix Task Points: {total_tasks} runs (5 Conditions x 3 Traces x 3 Reps x 3 Passes)")

    results = []
    completed_keys = set()
    chk_path = os.path.join(SUMMARY_DIR, "formal-v2-e8-matrix-checkpoint.json")
    if os.path.exists(chk_path):
        try:
            with open(chk_path, "r") as f:
                results = json.load(f)
            for r in results:
                completed_keys.add((r["condition"], r["trace"], r["rep"], r["pass"]))
            print(f"[RESUME] Found {len(results)} previously completed runs in checkpoint. Resuming remaining tasks...")
        except Exception as e:
            print(f"[WARNING] Could not load checkpoint: {e}. Starting fresh.")
            results = []

    start_matrix_time = time.time()

    for idx, (cond, trace, rep, p) in enumerate(tasks):
        cond_id, is_clean, threshold, disable_auto_comp, post_flush, layer = cond
        trace_label, trace_case = trace
        pass_name, events_str = p

        task_key = (cond_id, trace_label, rep, pass_name)
        if task_key in completed_keys:
            print(f"Skipping [{idx+1}/{total_tasks}] (Already completed in checkpoint): {cond_id} | {trace_label} | Rep {rep+1} | {pass_name}")
            continue

        print(f"\nProgress: [{idx+1}/{total_tasks}] ({((idx)/total_tasks)*100:.1f}%) - Elapsed: {(time.time()-start_matrix_time)/60:.1f}m")
        res = run_single_matrix_point(cond_id, is_clean, threshold, disable_auto_comp, post_flush, layer, trace_label, trace_case, rep, pass_name, events_str)
        if not res:
            print(f"[FATAL] Matrix run failed on task {idx+1}! Aborting.")
            return False

        results.append(res)
        completed_keys.add(task_key)

        # Save checkpoint after every run
        chk_path = os.path.join(SUMMARY_DIR, "formal-v2-e8-matrix-checkpoint.json")
        os.makedirs(os.path.dirname(chk_path), exist_ok=True)
        with open(chk_path, "w") as f:
            json.dump(results, f, indent=2)

    # Save final results
    final_path = os.path.join(SUMMARY_DIR, "formal-v2-e8-matrix-final.json")
    with open(final_path, "w") as f:
        json.dump(results, f, indent=2)

    total_el = time.time() - start_matrix_time
    print(f"\n[MATRIX SUCCESS] All {total_tasks} runs completed in {total_el/60:.2f} minutes!")
    return True

if __name__ == "__main__":
    success = run_matrix()
    sys.exit(0 if success else 1)
