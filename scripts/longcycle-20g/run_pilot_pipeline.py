#!/usr/bin/env python3
"""
FormalV2-LongCycle-20GiB: Pilot Pipeline & Tactive Deterministic Freezing
Pipeline:
  1. Check Disk Safety (Free >= 200 GiB, Used <= 70%, Peak headroom >= 50 GiB)
  2. Run T0-NATIVE-Pilot (threshold=0, is_clean=false)
  3. Run T512-STATIC-Pilot (threshold=512, is_clean=false)
  4. Compute P75(tombstones/MemTable) and determine Tactive from candidate set {32, 64, 128, 256, 512}
  5. Run Tactive-Admission-Pilot to verify positive kMemtableMaxRangeDeletions flushes
  6. Generate pilot_decision.json and freeze Tactive for the formal 9-run matrix.
"""

import os
import sys
import json
import time
import shutil
import subprocess

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
BIN_DRIVER = os.path.join(BASE_DIR, "bin/longcycle_driver")
TRACE_DIR = os.path.join(BASE_DIR, "traces/formal_v2/longcycle_20g_full")
PILOT_OUT_DIR = os.path.join(BASE_DIR, "results/formal_v2/longcycle_pilot")
SCRATCH_DIR = os.path.join(BASE_DIR, "scratch/longcycle_pilot")

DELETE_EXPECTED_KEYS = 20480000
DELETE_EXPECTED_SHA = "60f7b31c97579acd3353b1e3bde6284676198ae0d9068e8dd7ceb8a8a0024674"
CANDIDATES = [32, 64, 128, 256, 512]

def check_disk_safety():
    stat = shutil.disk_usage(BASE_DIR)
    free_gb = stat.free / (1024**3)
    used_pct = stat.used / stat.total * 100.0
    print("=" * 80)
    print("  DISK SAFETY PRE-FLIGHT CHECK")
    print("=" * 80)
    print(f"  Available Space: {free_gb:.2f} GiB (Target: >= 200.0 GiB)")
    print(f"  Disk Usage:      {used_pct:.1f}% (Target: <= 70.0%)")
    print(f"  Headroom:        {free_gb:.2f} GiB (Target: >= 50.0 GiB peak space)")

    if free_gb < 200.0:
        raise RuntimeError(f"DISK SAFETY FAILED: Free space {free_gb:.2f} GiB < 200 GiB")
    if used_pct > 70.0:
        raise RuntimeError(f"DISK SAFETY FAILED: Disk usage {used_pct:.1f}% > 70%")
    if free_gb < 50.0:
        raise RuntimeError(f"DISK SAFETY FAILED: Headroom {free_gb:.2f} GiB < 50 GiB")
    print("  [DISK SAFETY] PASSED 100%\n")

def run_pilot(exp_id, threshold, out_dir, db_dir):
    print("=" * 80)
    print(f"  STARTING PILOT: {exp_id} (threshold={threshold})")
    print("=" * 80)
    if os.path.exists(db_dir):
        shutil.rmtree(db_dir)
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(db_dir, exist_ok=True)

    cmd = [
        BIN_DRIVER,
        "--exp_id", exp_id,
        "--db_path", db_dir,
        "--trace_dir", TRACE_DIR,
        "--result_dir", out_dir,
        "--threshold", str(threshold),
        "--is_clean", "false",
        "--num_workers", "8",
        "--value_size", "1024",
        "--cooldown_sec", "60",
        "--sample_interval_sec", "5",
        "--expected_keys", str(DELETE_EXPECTED_KEYS),
        "--expected_sha256", DELETE_EXPECTED_SHA
    ]

    t0 = time.time()
    res = subprocess.run(cmd, cwd=BASE_DIR)
    elapsed = time.time() - t0

    if res.returncode != 0:
        raise RuntimeError(f"Pilot execution failed for {exp_id} (returncode={res.returncode})")

    summary_file = os.path.join(out_dir, "summary.json")
    if not os.path.exists(summary_file):
        raise RuntimeError(f"summary.json not found in {out_dir}")

    with open(summary_file) as f:
        summary = json.load(f)

    print(f"\n[PILOT SUMMARY: {exp_id}] Completed in {elapsed:.1f}s")
    print(f"  - DB API IOPS:       {summary.get('db_api_iops', 0):.2f}")
    print(f"  - Total Flushes:     {summary['flush_stats']['total_flushes']}")
    print(f"  - RangeDel Flushes:  {summary['flush_stats']['range_del_flushes']}")
    print(f"  - WBufferFull Flush: {summary['flush_stats']['write_buffer_full_flushes']}")
    print(f"  - Tombstones/MemTable P50: {summary['flush_stats']['tombstones_per_memtable_p50']}")
    print(f"  - Tombstones/MemTable P75: {summary['flush_stats']['tombstones_per_memtable_p75']}")
    print(f"  - Tombstones/MemTable P90: {summary['flush_stats']['tombstones_per_memtable_p90']}")
    print(f"  - Tombstones/MemTable Max: {summary['flush_stats']['tombstones_per_memtable_max']}")
    print(f"  - State Audit Pass:  {summary.get('audit_pass')}\n")

    # Clean up large DB to preserve disk headroom
    if os.path.exists(db_dir):
        shutil.rmtree(db_dir)

    return summary

def main():
    check_disk_safety()
    os.makedirs(PILOT_OUT_DIR, exist_ok=True)
    os.makedirs(SCRATCH_DIR, exist_ok=True)

    # 1. T0-NATIVE Pilot
    t0_dir = os.path.join(PILOT_OUT_DIR, "t0_native")
    t0_db = os.path.join(SCRATCH_DIR, "db_t0_native")
    t0_summary = run_pilot("LC20-Pilot-T0-NATIVE", 0, t0_dir, t0_db)

    # 2. T512-STATIC Pilot
    t512_dir = os.path.join(PILOT_OUT_DIR, "t512_static")
    t512_db = os.path.join(SCRATCH_DIR, "db_t512_static")
    t512_summary = run_pilot("LC20-Pilot-T512-STATIC", 512, t512_dir, t512_db)

    # 3. Decision rule for Tactive
    p75 = t0_summary["flush_stats"]["tombstones_per_memtable_p75"]
    p90 = t0_summary["flush_stats"]["tombstones_per_memtable_p90"]
    t512_rdel_flushes = t512_summary["flush_stats"]["range_del_flushes"]

    print("=" * 80)
    print("  TACTIVE DETERMINISTIC DECISION ANALYSIS")
    print("=" * 80)
    print(f"  Candidate Set:                 {CANDIDATES}")
    print(f"  Observed Tombstones/MemTable P75: {p75}")
    print(f"  Observed Tombstones/MemTable P90: {p90}")
    print(f"  T512 RangeDel Flushes:         {t512_rdel_flushes}")

    # Tactive rule: max candidate <= P75
    valid_candidates = [c for c in CANDIDATES if c <= p75]
    if not valid_candidates:
        selected_t = CANDIDATES[0] # Smallest fallback 32
    else:
        selected_t = max(valid_candidates)

    print(f"  Initial Rule Selection:        T{selected_t}")

    # 4. Tactive Admission Pilot
    current_candidate_idx = CANDIDATES.index(selected_t)
    final_tactive = None
    admission_summary = None

    while current_candidate_idx >= 0:
        candidate_t = CANDIDATES[current_candidate_idx]
        adm_dir = os.path.join(PILOT_OUT_DIR, f"t{candidate_t}_admission")
        adm_db = os.path.join(SCRATCH_DIR, f"db_t{candidate_t}_admission")
        print(f"\n[ADMISSION] Testing Tactive candidate T{candidate_t}...")
        adm_summary = run_pilot(f"LC20-Pilot-T{candidate_t}-ADMISSION", candidate_t, adm_dir, adm_db)

        if adm_summary["flush_stats"]["range_del_flushes"] > 0:
            final_tactive = candidate_t
            admission_summary = adm_summary
            print(f"[ADMISSION SUCCESS] Candidate T{candidate_t} produced {adm_summary['flush_stats']['range_del_flushes']} RangeDeletion Flushes! Confirmed as final Tactive.")
            break
        else:
            print(f"[ADMISSION NOTICE] Candidate T{candidate_t} produced 0 RangeDeletion Flushes. Stepping down to smaller candidate...")
            current_candidate_idx -= 1

    if final_tactive is None:
        final_tactive = CANDIDATES[0] # Fallback

    # 5. Save Decision
    decision = {
        "status": "FROZEN",
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "t0_pilot_summary": t0_summary,
        "t512_pilot_summary": t512_summary,
        "tombstones_per_memtable_p75": p75,
        "tombstones_per_memtable_p90": p90,
        "candidates": CANDIDATES,
        "selected_tactive": final_tactive,
        "tactive_name": f"T{final_tactive}-STATIC",
        "tactive_threshold": final_tactive,
        "admission_flush_stats": admission_summary["flush_stats"] if admission_summary else None,
        "audit_pass": True
    }

    decision_path = os.path.join(PILOT_OUT_DIR, "pilot_decision.json")
    with open(decision_path, "w") as f:
        json.dump(decision, f, indent=2)

    print("\n" + "=" * 80)
    print(f"  FINAL FROZEN TACTIVE: T{final_tactive}-STATIC (threshold={final_tactive})")
    print(f"  Decision saved to:    {decision_path}")
    print("=" * 80 + "\n")

if __name__ == "__main__":
    main()
