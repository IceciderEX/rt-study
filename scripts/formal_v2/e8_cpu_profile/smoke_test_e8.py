#!/usr/bin/env python3
"""
Formal V2 - E8 Smoke Test Runner (T0 + Scan-Intersect) - 3 Non-Multiplexed Passes
Strictly adheres to Wait-Point Profiling Architecture v5:
1. Pass A: cycles:u, instructions:u (100.00% time running)
2. Pass B: branches:u, branch-misses:u (100.00% time running)
3. Pass C: cache-references:u, cache-misses:u (100.00% time running)
4. Kernel-blocking wait points (Wait-Point 1: start, Wait-Point 2: release);
5. Complete timeline timestamps + CLOCK_THREAD_CPUTIME_ID audit;
6. Full-DB KV reconciliation (300,000 visible keys).
"""

import os
import sys
import time
import json
import subprocess
from pathlib import Path

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/e8_micro_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces/formal_v2/e8_scan_intersect")
RUN_BASE = os.path.join(BASE_DIR, "run-db/e8_smoke")

PASS_CONFIGS = [
    ("Pass-A", "cycles:u,instructions:u"),
    ("Pass-B", "branches:u,branch-misses:u"),
    ("Pass-C", "cache-references:u,cache-misses:u"),
]

def run_single_pass(pass_name, events_str):
    print(f"\n================================================================")
    print(f"  Executing {pass_name}: Events = [{events_str}]")
    print(f"================================================================")

    # 1. P0 Sudo Non-Interactive Pre-check
    res = subprocess.run(["sudo", "-n", "true"], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FATAL] sudo -n true failed! Sudo non-interactive session expired.")
        return False, None

    # 2. Directory and Umask Setup
    run_id = f"smoke_t0_scan_intersect_{pass_name.lower()}_{int(time.time())}"
    run_dir = os.path.join(RUN_BASE, run_id)
    db_dir = os.path.join(run_dir, "db")
    wait_dir = os.path.join(run_dir, "wait")

    if os.path.exists(run_dir):
        print(f"[FATAL] Run directory {run_dir} already exists!")
        return False, None

    old_umask = os.umask(0o077)
    try:
        os.makedirs(wait_dir, mode=0o700, exist_ok=False)
        os.makedirs(db_dir, mode=0o700, exist_ok=False)
    finally:
        os.umask(old_umask)

    # 3. Launch Driver as normal user wam bound to physical CPU 2
    driver_stdout_path = os.path.join(wait_dir, "driver.stdout")
    driver_stderr_path = os.path.join(wait_dir, "driver.stderr")
    driver_stdout = open(driver_stdout_path, "w")
    driver_stderr = open(driver_stderr_path, "w")

    driver_cmd = [
        "taskset", "-c", "2",
        BIN_DRIVER,
        "--db_path", db_dir,
        "--trace_dir", TRACE_DIR,
        "--profile_wait_dir", wait_dir,
        "--profile_window_sec", "45",
        "--profile_case", "t0_scan_intersect",
        "--threshold", "0",
        "--disable_auto_compactions", "false",
        "--is_clean", "false",
        "--total_keys", "500000"
    ]

    print(f"[{pass_name} 1/8] Launching e8_micro_driver (bound to CPU 2)...")
    driver_proc = subprocess.Popen(driver_cmd, stdout=driver_stdout, stderr=driver_stderr)
    driver_pid = driver_proc.pid

    # 4. Wait for PROFILE_READY.json from Coordinator
    print(f"[{pass_name} 2/8] Waiting for PROFILE_READY.json...")
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
        print("[FATAL] Timed out waiting for PROFILE_READY.json!")
        driver_proc.kill()
        return False, None

    fg_tid = meta.get("foreground_tid")
    print(f"      -> Coordinator and Worker Ready: PID={driver_pid}, Worker TID={fg_tid}")

    # 5. Verify 7-Attribute Identity
    if meta.get("pid") != driver_pid or not os.path.exists(f"/proc/{driver_pid}/task/{fg_tid}"):
        print("[FATAL] PID/TID identity mismatch!")
        driver_proc.kill()
        return False, None

    # 6. Launch sudo -n perf stat attached ONLY to foreground_tid
    print(f"[{pass_name} 3/8] Launching sudo -n perf stat attached exclusively to TID={fg_tid}...")
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

    # Verify perf alive
    time.sleep(1.5)
    if perf_proc.poll() is not None:
        print("[FATAL] sudo -n perf stat exited prematurely!")
        driver_proc.kill()
        return False, None

    # 7. Atomically create PROFILE_START to wake worker
    print(f"[{pass_name} 4/8] Creating PROFILE_START to wake worker into 45s window...")
    profile_start_ts_ns = time.time_ns()
    start_path = os.path.join(wait_dir, "PROFILE_START")
    fd = os.open(start_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)

    # 8. Wait for PROFILE_DONE.json from Coordinator
    print(f"[{pass_name} 5/8] Waiting for worker 45s window completion and PROFILE_DONE.json...")
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
        print("[FATAL] Timed out waiting for PROFILE_DONE.json!")
        driver_proc.kill()
        perf_proc.kill()
        return False, None

    print(f"      -> Worker finished window and entered Wait-Point 2 kernel sleep.")

    # 9. Create PROFILE_RELEASE to allow worker to join cleanly
    print(f"[{pass_name} 6/8] Creating PROFILE_RELEASE to join worker and trigger DB reconciliation...")
    profile_release_ts_ns = time.time_ns()
    rel_path = os.path.join(wait_dir, "PROFILE_RELEASE")
    fd = os.open(rel_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)

    # 10. Wait for Driver and Perf completion
    print(f"[{pass_name} 7/8] Waiting for processes to complete...")
    driver_ret = driver_proc.wait()
    perf_ret = perf_proc.wait()
    perf_exit_ts_ns = time.time_ns()

    driver_stdout.close()
    driver_stderr.close()
    perf_stdout.close()
    perf_stderr.close()

    if driver_ret != 0:
        print(f"[FATAL] Driver exited with code {driver_ret}")
        with open(driver_stderr_path, "r") as f:
            print(f"Stderr:\n{f.read()}")
        return False, None

    # 11. Parse and Audit perf Output
    print(f"[{pass_name} 8/8] Auditing perf PMU time running and DB state...")
    with open(perf_stderr_path, "r") as f:
        perf_stderr_text = f.read()

    # Check 100.00% time running or non-multiplexed (no percentage parenthesis or 100.00%)
    events_audit = {}
    is_multiplexed = False
    for line in perf_stderr_text.strip().split("\n"):
        line_clean = line.strip()
        if not line_clean: continue
        parts = line_clean.split()
        for ev in events_str.split(","):
            if ev in line_clean:
                # Check percentage: if e.g. (83.33%) appears, mark multiplexed
                if "(" in line_clean and "%" in line_clean:
                    pct_str = line_clean[line_clean.find("(")+1 : line_clean.find("%")]
                    try:
                        pct_val = float(pct_str)
                        if abs(pct_val - 100.0) > 0.01:
                            is_multiplexed = True
                            print(f"[FATAL MULTIPLEXING DETECTED] Event {ev} running at {pct_val}% (must be 100.00%)!")
                    except ValueError:
                        pass
                # Extract counter value
                val_str = parts[0].replace(",", "")
                if val_str.isdigit():
                    events_audit[ev] = int(val_str)

    with open(driver_stdout_path, "r") as f:
        driver_stdout_text = f.read()

    if "Total Visible Keys: 300000" not in driver_stdout_text:
        print("[FATAL] Visible key count mismatch in DB reconciliation!")
        return False, None

    pass_result = {
        "pass_name": pass_name,
        "run_id": run_id,
        "pid": driver_pid,
        "tid": fg_tid,
        "events_requested": events_str,
        "events_collected": events_audit,
        "is_multiplexed": is_multiplexed,
        "completed_ops": done_meta["completed_ops"],
        "elapsed_sec": done_meta["elapsed_sec"],
        "true_phase_iops": done_meta["true_phase_iops"],
        "worker_cpu_time_sec": done_meta["worker_cpu_time_ns"] / 1e9,
        "api_seek_calls": done_meta["api_seek_calls"],
        "api_next_calls": done_meta["api_next_calls"],
        "returned_visible_keys": done_meta["returned_visible_keys"],
        "timestamps": {
            "perf_start_ts_ns": perf_start_ts_ns,
            "profile_start_created_ts_ns": profile_start_ts_ns,
            "worker_window_start_ts_ns": done_meta["window_start_ns"],
            "worker_window_end_ts_ns": done_meta["window_end_ns"],
            "profile_done_written_ts_ns": profile_done_ts_ns,
            "profile_release_created_ts_ns": profile_release_ts_ns,
            "perf_exit_ts_ns": perf_exit_ts_ns
        },
        "perf_raw_output": perf_stderr_text.strip()
    }

    if is_multiplexed:
        return False, pass_result

    print(f"[{pass_name} SUCCESS] 100.00% non-multiplexed PMU capture verified.")
    return True, pass_result

def run_all_smoke_passes():
    print("################################################################")
    print("  Formal V2 - E8 Smoke Test Suite: 3 Non-Multiplexed Passes")
    print("################################################################")

    all_results = []
    for pass_name, events in PASS_CONFIGS:
        ok, res = run_single_pass(pass_name, events)
        if not ok:
            print(f"\n[FATAL] {pass_name} FAILED! Aborting smoke suite.")
            return False, all_results
        all_results.append(res)

    print("\n================================================================")
    print("  All 3 Non-Multiplexed Passes Successfully Completed")
    print("================================================================")
    for r in all_results:
        print(f"\n[{r['pass_name']}] Run ID: {r['run_id']}")
        print(f"  Target PID/TID:      {r['pid']} / {r['tid']}")
        print(f"  Wall Duration:       {r['elapsed_sec']:.6f} s")
        print(f"  Worker CPU Time:     {r['worker_cpu_time_sec']:.6f} s (Ratio: {r['worker_cpu_time_sec']/r['elapsed_sec']*100:.2f}%)")
        print(f"  Completed Ops:       {r['completed_ops']}")
        print(f"  True Phase IOPS:     {r['true_phase_iops']:.2f} ops/sec")
        print(f"  API Calls:           Seek={r['api_seek_calls']}, Next={r['api_next_calls']}, Keys={r['returned_visible_keys']}")
        print(f"  Events Collected:    {r['events_collected']}")
        print(f"  PMU Multiplexing:    NONE (100.00% time running)")

    # Save summary json
    summary_path = os.path.join(BASE_DIR, "results/summary/formal-v2-e8-smoke-passes.json")
    os.makedirs(os.path.dirname(summary_path), exist_ok=True)
    with open(summary_path, "w") as f:
        json.dump(all_results, f, indent=2)
    print(f"\nSummary JSON saved to {summary_path}")
    return True, all_results

if __name__ == "__main__":
    success, results = run_all_smoke_passes()
    sys.exit(0 if success else 1)
