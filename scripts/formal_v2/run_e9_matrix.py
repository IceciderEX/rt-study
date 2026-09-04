#!/usr/bin/env python3
"""
E9-DIA: Dynamic Read Path Audit Formal Mechanism Matrix Runner
9 runs: T0 (N=3), Native-T512 (N=3), RTP-MC-V2-A (N=3)
Execution order: Pre-registered Latin-Square interleaved schedule
"""

import os
import sys
import time
import shutil
import subprocess
import pandas as pd

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")
CONFIG_DIR = os.path.join(BASE_DIR, "configs", "formal_v2", "e9_dynamic_audit", "matrix")
MATRIX_OUT_DIR = os.path.join(BASE_DIR, "results", "summary", "e9_dynamic_audit", "matrix")
RUN_DB_ROOT = os.path.join(BASE_DIR, "run-db", "e9_dynamic_audit")
TRACE_DIR = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_range")

# Pre-registered 3x3 Latin-Square Schedule:
# Round 1: T0 -> T512 -> V2A
# Round 2: T512 -> V2A -> T0
# Round 3: V2A -> T0 -> T512
SCHEDULE = [
    ("T0",          1, "e9_t0_r1"),
    ("Native-T512", 1, "e9_t512_r1"),
    ("RTP-MC-V2-A", 1, "e9_v2a_r1"),
    ("Native-T512", 2, "e9_t512_r2"),
    ("RTP-MC-V2-A", 2, "e9_v2a_r2"),
    ("T0",          2, "e9_t0_r2"),
    ("RTP-MC-V2-A", 3, "e9_v2a_r3"),
    ("T0",          3, "e9_t0_r3"),
    ("Native-T512", 3, "e9_t512_r3"),
]

def generate_ini(group_name: str, rep: int, exp_id: str) -> str:
    ini_path = os.path.join(CONFIG_DIR, f"{exp_id}.ini")
    db_path = os.path.join(RUN_DB_ROOT, exp_id, "db")
    run_dir = os.path.join(MATRIX_OUT_DIR, exp_id)
    
    lines = [
        f"# E9 Matrix: {group_name} Repetition {rep}",
        f"exp_id={exp_id}",
        f"group_name={group_name}",
        f"desc=E9 Dynamic Audit Matrix ({group_name} rep {rep})",
        f"trace_dir={TRACE_DIR}",
        f"db_path={db_path}",
        f"result_dir={run_dir}",
        f"summary_csv={os.path.join(run_dir, 'summary.csv')}",
        f"events_csv={os.path.join(run_dir, 'events.csv')}",
        f"phases_csv={os.path.join(run_dir, 'phases.csv')}",
        f"audit_output_dir={run_dir}",
        f"rep={rep}",
        "",
        "total_keys=500000",
        "value_size=256",
        "num_workers=8",
        "random_seed=90001",
        "",
    ]

    if group_name == "T0":
        lines += [
            "memtable_max_range_deletions=0",
            "memtable_op_scan_flush_trigger=0",
            "enable_range_tombstone_controller=false",
            "range_tombstone_controller_observe_only=true",
        ]
    elif group_name == "Native-T512":
        lines += [
            "memtable_max_range_deletions=512",
            "memtable_op_scan_flush_trigger=0",
            "enable_range_tombstone_controller=false",
            "range_tombstone_controller_observe_only=true",
            "range_tombstone_controller_min_range_deletions=512",
            "range_tombstone_controller_min_memtable_bytes=8388608",
            "range_tombstone_controller_cooldown_micros=1000000",
        ]
    elif group_name == "RTP-MC-V2-A":
        lines += [
            f"windows_csv={os.path.join(run_dir, 'controller_windows.csv')}",
            f"actions_csv={os.path.join(run_dir, 'controller_actions.csv')}",
            "memtable_max_range_deletions=0",
            "memtable_op_scan_flush_trigger=0",
            "rtp_mc_mode=active_v2a",
            "control_epoch_ms=50",
            "range_del_checkpoint=128",
            "min_scan_samples=50",
            "min_get_samples=100",
            "min_put_samples=50",
            "ref_read_rate=10000.0",
            "rolling_window_ms=500",
            "scan_slo_us=500.0",
            "getlive_slo_us=200.0",
            "put_slo_us=150.0",
            "l0_soft_limit=4",
            "l0_hard_limit=8",
            "pending_compaction_soft_bytes=67108864",
            "pending_compaction_hard_bytes=268435456",
            "max_tombstone_residency_ms=5000",
            "max_range_del_deadman=2048",
            "capacity_flush_imminent_ms=200",
            "read_critical_multiplier=2.0",
            "read_critical_consecutive_windows=3",
        ]

    lines += [
        "",
        "# RocksDB Base Settings",
        "write_buffer_size=67108864",
        "max_write_buffer_number=4",
        "level0_file_num_compaction_trigger=4",
        "level0_slowdown_writes_trigger=8",
        "level0_stop_writes_trigger=12",
        "block_cache_size=134217728",
        "target_file_size_base=67108864",
        "max_bytes_for_level_base=268435456",
        "max_background_jobs=8",
        ""
    ]

    with open(ini_path, "w") as f:
        f.write("\n".join(lines))
    return ini_path

def run_single(group_name: str, rep: int, exp_id: str, ini_path: str):
    run_dir = os.path.join(MATRIX_OUT_DIR, exp_id)
    db_path = os.path.join(RUN_DB_ROOT, exp_id, "db")
    os.makedirs(run_dir, exist_ok=True)

    if os.path.exists(db_path):
        print(f"[Cleanup] Removing old db_path: {db_path}")
        shutil.rmtree(db_path)

    log_path = os.path.join(run_dir, "driver.log")
    cmd = [DRIVER_BIN, "--config", ini_path]

    t0 = time.time()
    print(f"[{time.strftime('%H:%M:%S')}] >>> Starting Run: {exp_id} ({group_name}, rep {rep}) ...", flush=True)

    with open(log_path, "w") as log_f:
        p = subprocess.run(cmd, stdout=log_f, stderr=subprocess.STDOUT, text=True)

    elapsed = time.time() - t0
    if p.returncode != 0:
        print(f"[ERROR] Run {exp_id} FAILED with exit code {p.returncode}! Log: {log_path}", file=sys.stderr)
        sys.exit(1)

    with open(log_path, "r") as log_f:
        content = log_f.read()
        if ">> PASS <<" not in content:
            print(f"[ERROR] Verification failed for {exp_id}! '>> PASS <<' not in log.", file=sys.stderr)
            sys.exit(1)

    print(f"[{time.strftime('%H:%M:%S')}] [PASS] {exp_id} completed in {elapsed:.2f} s. Verified OK.", flush=True)

def consolidate_csvs():
    print("\n[Consolidation] Consolidating CSV outputs across all 9 matrix runs...")
    files_to_merge = [
        ("audit_run_summary.csv", os.path.join(MATRIX_OUT_DIR, "audit_run_summary.csv")),
        ("audit_phase_summary.csv", os.path.join(MATRIX_OUT_DIR, "audit_phase_summary.csv")),
        ("audit_worker_snapshots.csv", os.path.join(MATRIX_OUT_DIR, "audit_worker_snapshots.csv")),
        ("audit_materialization_events.csv", os.path.join(MATRIX_OUT_DIR, "audit_materialization_events.csv")),
        ("phases.csv", os.path.join(MATRIX_OUT_DIR, "phases.csv")),
        ("summary.csv", os.path.join(MATRIX_OUT_DIR, "summary.csv")),
    ]

    for fname, out_path in files_to_merge:
        merged_dfs = []
        for group_name, rep, exp_id in SCHEDULE:
            run_file = os.path.join(MATRIX_OUT_DIR, exp_id, fname)
            if os.path.exists(run_file):
                df = pd.read_csv(run_file)
                merged_dfs.append(df)
            else:
                print(f"[WARNING] Missing {run_file}")
        if merged_dfs:
            combined = pd.concat(merged_dfs, ignore_index=True)
            combined.to_csv(out_path, index=False)
            print(f"  Merged {len(combined)} rows -> {out_path}")

    # Verify mathematical identity on merged summaries
    run_sum = pd.read_csv(os.path.join(MATRIX_OUT_DIR, "audit_run_summary.csv"))
    print("\n[Verification Check on Consolidated audit_run_summary.csv]")
    for idx, row in run_sum.iterrows():
        aff = row['materialization_or_lock_affected_reads']
        mat = row['materialized_reads']
        lock = row['lock_contended_reads']
        both = row['both_reads']
        assert aff == mat + lock - both, f"Identity violated in run_summary row {idx}: {aff} != {mat} + {lock} - {both}"
        print(f"  {row['exp_id']} (rep {row['rep']}): aff={aff} == mat({mat}) + lock({lock}) - both({both}) [OK]")

    phase_sum = pd.read_csv(os.path.join(MATRIX_OUT_DIR, "audit_phase_summary.csv"))
    print("\n[Verification Check on Consolidated audit_phase_summary.csv]")
    for idx, row in phase_sum.iterrows():
        aff = row['materialization_or_lock_affected_reads']
        mat = row['materialized_ops']
        lock = row['lock_contended_ops']
        both = row['both_count']
        assert aff == mat + lock - both, f"Identity violated in phase_summary row {idx}: {aff} != {mat} + {lock} - {both}"
    print(f"  All {len(phase_sum)} phase rows passed identity check [OK]")

def main():
    os.makedirs(CONFIG_DIR, exist_ok=True)
    os.makedirs(MATRIX_OUT_DIR, exist_ok=True)

    print("================================================================================")
    print("E9-DIA Dynamic Read Path Audit: Formal Mechanism Matrix (9 Runs)")
    print("================================================================================")
    print(f"Latin-Square Interleaved Execution Order (N=3 per config):")
    for step_i, (grp, rep, exp_id) in enumerate(SCHEDULE, 1):
        print(f"  Step {step_i}: {exp_id} ({grp}, rep {rep})")
    print("================================================================================")

    for step_i, (grp, rep, exp_id) in enumerate(SCHEDULE, 1):
        ini_path = generate_ini(grp, rep, exp_id)
        print(f"\n--- [Step {step_i}/9] {exp_id} ({grp}, rep {rep}) ---")
        run_single(grp, rep, exp_id, ini_path)
        if step_i < len(SCHEDULE):
            print("  Cooling down for 5 seconds ...")
            time.sleep(5)

    consolidate_csvs()
    print("\n================================================================================")
    print("ALL 9 MATRIX RUNS COMPLETED AND VERIFIED SUCCESSFULLY!")
    print("================================================================================")

if __name__ == "__main__":
    main()
