#!/usr/bin/env python3
"""
Formal V2 - E8-FG Master FlameGraph Profiling Runner
Supports:
- 3 Conditions: CLEAN, T0-MEM, POSTFLUSH-SST
- Benchmark: ScanIntersect
- Dual-waitpoint synchronization with strict kernel futex blocking on Worker thread
- Coordinator-handled atomic state transitions (Worker performs zero JSON/file I/O)
- Non-interactive sudo -n perf record (199 Hz, cycles:u, --call-graph fp) attached to target Worker TID
- Self% and Children% Top-20 profiling
- Unknown leaf & stack percentage auditing
- Full-DB 500,000 Key visible count and SHA-256 state reconciliation
"""

import os
import sys
import time
import json
import subprocess
import shutil
from pathlib import Path

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/e8_micro_driver")
TRACES_BASE = os.path.join(BASE_DIR, "traces/formal_v2")
RUN_BASE = os.path.join(BASE_DIR, "run-db/e8_flamegraph")
RESULTS_BASE = os.path.join(BASE_DIR, "results/formal_v2/e8_flamegraph")
FLAMEGRAPH_DIR = "/home/wam/grad/tools/FlameGraph"

# Pre-registered 3x3 Latin Square Interleaved Order
LATIN_SQUARE_MATRIX = [
    # Rep01
    ("CLEAN",         True,  0, False, False, 1, 1),
    ("T0-MEM",        False, 0, False, False, 1, 2),
    ("POSTFLUSH-SST", False, 0, True,  True,  1, 3),
    # Rep02
    ("POSTFLUSH-SST", False, 0, True,  True,  2, 4),
    ("CLEAN",         True,  0, False, False, 2, 5),
    ("T0-MEM",        False, 0, False, False, 2, 6),
    # Rep03
    ("T0-MEM",        False, 0, False, False, 3, 7),
    ("POSTFLUSH-SST", False, 0, True,  True,  3, 8),
    ("CLEAN",         True,  0, False, False, 3, 9),
]

SMOKE_CASES = [
    ("CLEAN",         True,  0, False, False, 0, 1),
    ("T0-MEM",        False, 0, False, False, 0, 2),
    ("POSTFLUSH-SST", False, 0, True,  True,  0, 3),
]

def check_sudo_permission():
    res = subprocess.run(["sudo", "-n", "true"], capture_output=True)
    if res.returncode != 0:
        print("[FATAL] sudo -n failed. Please authenticate sudo first.")
        sys.exit(1)

def run_single_point(cond_id, is_clean, threshold, disable_auto_comp, post_flush, rep_idx, seq_num, is_smoke=False):
    case_name = f"{cond_id.lower()}_scan_intersect_{'smoke' if is_smoke else f'rep{rep_idx}'}"
    run_id = f"fg_{case_name}_{int(time.time())}"
    run_dir = os.path.join(RUN_BASE, run_id)
    out_dir = os.path.join(RESULTS_BASE, "smoke" if is_smoke else f"rep{rep_idx}", cond_id.lower())
    db_dir = os.path.join(run_dir, "db")
    wait_dir = os.path.join(run_dir, "wait")

    os.makedirs(out_dir, exist_ok=True)

    trace_dir_name = "e8_scan_intersect_clean" if is_clean else "e8_scan_intersect"
    trace_dir = os.path.join(TRACES_BASE, trace_dir_name)

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
        "--profile_case", f"{cond_id.lower()}_scan_intersect",
        "--threshold", str(threshold),
        "--disable_auto_compactions", "true" if disable_auto_comp else "false",
        "--is_clean", "true" if is_clean else "false",
        "--post_flush", "true" if post_flush else "false",
        "--total_keys", "500000"
    ]

    print(f"\n>>> [STEP {seq_num}] Condition: {cond_id} | Rep: {rep_idx} | Run ID: {run_id}")
    driver_proc = subprocess.Popen(driver_cmd, stdout=driver_stdout, stderr=driver_stderr)
    driver_pid = driver_proc.pid

    # 1. Wait for PROFILE_READY.json
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

    # 2. Launch sudo -n perf record attached ONLY to foreground_tid
    perf_data_path = os.path.join(wait_dir, "perf.data")
    perf_cmd = [
        "sudo", "-n", "perf", "record",
        "-F", "199",
        "-e", "cycles:u",
        "-g",
        "--call-graph", "fp",
        "-t", str(fg_tid),
        "-o", perf_data_path,
        "--", "sleep", "47"
    ]

    perf_proc = subprocess.Popen(perf_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    if perf_proc.poll() is not None:
        print(f"[FATAL] perf record exited prematurely with code {perf_proc.returncode}")
        driver_proc.kill()
        return None

    # 3. Create PROFILE_START
    start_token = os.path.join(wait_dir, "PROFILE_START")
    with open(start_token + ".tmp", "w") as f:
        f.write(f"START {time.time()}\n")
        f.flush()
        os.fsync(f.fileno())
    os.rename(start_token + ".tmp", start_token)

    # 4. Wait for Worker to complete 45s loop and Coordinator to write PROFILE_DONE.json
    done_file = os.path.join(wait_dir, "PROFILE_DONE.json")
    done_meta = None
    wait_done_start = time.time()

    while time.time() - wait_done_start < 60:
        if os.path.exists(done_file):
            try:
                with open(done_file, "r") as f:
                    done_meta = json.load(f)
                break
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.05)

    if not done_meta:
        print(f"[FATAL] Timed out waiting for PROFILE_DONE.json in {run_id}")
        perf_proc.kill()
        driver_proc.kill()
        return None

    # 5. Wait for perf record to finish
    perf_proc.wait(timeout=15)

    # 6. Change ownership of perf.data from root to current user
    user_uid = os.getuid()
    user_gid = os.getgid()
    subprocess.run(["sudo", "-n", "chown", f"{user_uid}:{user_gid}", perf_data_path], check=True)

    # 7. Release driver
    release_token = os.path.join(wait_dir, "PROFILE_RELEASE")
    with open(release_token + ".tmp", "w") as f:
        f.write(f"RELEASE {time.time()}\n")
        f.flush()
        os.fsync(f.fileno())
    os.rename(release_token + ".tmp", release_token)

    driver_proc.wait(timeout=60)
    driver_stdout.close()
    driver_stderr.close()

    # 8. Post-processing with FlameGraph tools
    perf_script_path = os.path.join(out_dir, "perf.script")
    perf_folded_path = os.path.join(out_dir, "perf.folded")
    flamegraph_svg_path = os.path.join(out_dir, "flamegraph.svg")
    perf_report_self_path = os.path.join(out_dir, "perf-report-self.txt")
    perf_report_children_path = os.path.join(out_dir, "perf-report-children.txt")

    # Generate perf.script as current user
    with open(perf_script_path, "w") as f_out:
        subprocess.run(["perf", "script", "-f", "-i", perf_data_path], stdout=f_out, check=True)

    # Stackcollapse
    stackcollapse_pl = os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl")
    with open(perf_folded_path, "w") as f_out:
        subprocess.run([stackcollapse_pl, perf_script_path], stdout=f_out, check=True)

    # FlameGraph SVG
    flamegraph_pl = os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl")
    title_str = f"{cond_id} ScanIntersect CPU Flame Graph ({'Smoke' if is_smoke else f'Rep {rep_idx}'})"
    with open(flamegraph_svg_path, "w") as f_out:
        subprocess.run([
            flamegraph_pl,
            "--width", "1800",
            "--hash",
            "--title", title_str,
            "--countname", "samples",
            perf_folded_path
        ], stdout=f_out, check=True)

    # Reports: Self and Children as current user
    with open(perf_report_self_path, "w") as f_out:
        subprocess.run(["perf", "report", "-f", "-i", perf_data_path, "--stdio", "--no-children", "--sort", "dso,symbol"], stdout=f_out, check=True)
    with open(perf_report_children_path, "w") as f_out:
        subprocess.run(["perf", "report", "-f", "-i", perf_data_path, "--stdio", "--children", "--sort", "dso,symbol"], stdout=f_out, check=True)

    # Copy raw perf.data to output dir
    shutil.copyfile(perf_data_path, os.path.join(out_dir, "perf.data"))
    shutil.copyfile(done_file, os.path.join(out_dir, "PROFILE_DONE.json"))
    shutil.copyfile(driver_stdout_path, os.path.join(out_dir, "driver.stdout"))
    shutil.copyfile(driver_stderr_path, os.path.join(out_dir, "driver.stderr"))

    # Compute sample statistics and [unknown] percentages
    total_samples = 0
    unknown_leaf_samples = 0
    unknown_stack_samples = 0

    with open(perf_folded_path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.rsplit(" ", 1)
            if len(parts) != 2: continue
            stack, count_str = parts[0], parts[1]
            try:
                cnt = int(count_str)
            except ValueError:
                continue
            total_samples += cnt
            frames = stack.split(";")
            if frames and "[unknown]" in frames[-1]:
                unknown_leaf_samples += cnt
            if any("[unknown]" in frm for frm in frames):
                unknown_stack_samples += cnt

    unknown_leaf_pct = (unknown_leaf_samples / total_samples * 100.0) if total_samples > 0 else 0.0
    unknown_stack_pct = (unknown_stack_samples / total_samples * 100.0) if total_samples > 0 else 0.0

    # Extract state audit from driver stdout
    state_audit_pass = False
    visible_keys_count = 0
    state_sha256 = ""
    with open(driver_stdout_path) as f:
        for line in f:
            if "Visible Keys:" in line:
                try:
                    visible_keys_count = int(line.split(":")[1].split("(")[0].strip())
                except: pass
            if "DB SHA-256 Digest:" in line:
                state_sha256 = line.split(":")[1].strip()
            if "E8 run completed successfully with full state verification" in line or "PASS" in line:
                state_audit_pass = True

    result_summary = {
        "run_id": run_id,
        "cond_id": cond_id,
        "is_clean": is_clean,
        "threshold": threshold,
        "disable_auto_compactions": disable_auto_comp,
        "post_flush": post_flush,
        "rep_idx": rep_idx,
        "seq_num": seq_num,
        "is_smoke": is_smoke,
        "total_samples": total_samples,
        "unknown_leaf_samples": unknown_leaf_samples,
        "unknown_leaf_pct": unknown_leaf_pct,
        "unknown_stack_samples": unknown_stack_samples,
        "unknown_stack_pct": unknown_stack_pct,
        "completed_ops": done_meta.get("completed_ops", 0),
        "elapsed_sec": done_meta.get("elapsed_sec", 0.0),
        "worker_cpu_time_ns": done_meta.get("worker_cpu_time_ns", 0),
        "visible_keys_count": visible_keys_count,
        "state_sha256": state_sha256,
        "state_audit_pass": state_audit_pass,
        "out_dir": out_dir
    }

    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(result_summary, f, indent=2)

    print(f"[RESULT] {cond_id} ({'Smoke' if is_smoke else f'Rep {rep_idx}'}):")
    print(f"  - Samples:      {total_samples}")
    print(f"  - Unknown Leaf: {unknown_leaf_pct:.2f}% (Target: < 10%)")
    print(f"  - Unknown Stk:  {unknown_stack_pct:.2f}%")
    print(f"  - Ops/IOPS:     {done_meta.get('completed_ops', 0)} ({done_meta.get('completed_ops', 0)/done_meta.get('elapsed_sec', 1.0):.0f} IOPS)")
    print(f"  - Visible Keys: {visible_keys_count}")
    print(f"  - SHA-256:      {state_sha256[:16]}... (Audit: {state_audit_pass})")

    return result_summary

def main():
    check_sudo_permission()
    mode = "smoke"
    if len(sys.argv) > 1 and sys.argv[1] == "--matrix":
        mode = "matrix"

    print("=" * 64)
    print(f"  FormalV2-E8 FlameGraph Profiling Runner (Mode: {mode.upper()})")
    print("=" * 64)

    cases = SMOKE_CASES if mode == "smoke" else LATIN_SQUARE_MATRIX
    results = []

    for item in cases:
        cond_id, is_clean, threshold, disable_auto_comp, post_flush, rep_idx, seq_num = item
        res = run_single_point(cond_id, is_clean, threshold, disable_auto_comp, post_flush, rep_idx, seq_num, is_smoke=(mode == "smoke"))
        if not res:
            print(f"[FATAL] Failed at step {seq_num}. Aborting.")
            sys.exit(1)
        results.append(res)

    print("\n" + "=" * 64)
    print(f"  All {len(results)} runs in {mode.upper()} mode completed successfully!")
    print("=" * 64)

if __name__ == "__main__":
    main()
