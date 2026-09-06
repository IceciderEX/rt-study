#!/usr/bin/env python3
import os
import sys
import json
import time
import subprocess
import shutil
import csv

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
SEED_DB = os.path.join(STUDY_ROOT, "run-db/m2d_canonical_seed_db")
TRACE_DIR = os.path.join(STUDY_ROOT, "traces/m2d_getonly_dynamic_500k")
BIN_RELEASE = os.path.join(STUDY_ROOT, "bin/m2d_driver_release")
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m2d_diagnostics")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d/diagnostics_m2d1")

DIAGNOSTIC_CONFIGS = [
    ("B64-H16",  64,  16, "Config 1: B64 / H16 (Baseline smoke reproduction, overflow point capture)"),
    ("B64-H32",  64,  32, "Config 2: B64 / H32 (Deep capacity buffer, absorbing surge)"),
    ("B128-H16", 128, 16, "Config 3: B128 / H16 (Double run capacity, reduced intermediate levels)"),
    ("B256-H16", 256, 16, "Config 4: B256 / H16 (Quad run capacity, throughput vs tail latency evaluation)")
]

def run_diagnostic(cfg_name, delta, hard_limit, desc):
    exp_id = f"diag_{cfg_name.lower().replace('-', '_')}"
    db_path = os.path.join(RUN_DB_BASE, exp_id, "db")
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    os.makedirs(RESULTS_DIR, exist_ok=True)

    cmd = [
        "taskset", "-c", "0-19",
        BIN_RELEASE,
        "--exp-id", exp_id,
        "--config", cfg_name,
        "--db-path", db_path,
        "--seed-db", SEED_DB,
        "--trace-dir", TRACE_DIR,
        "--output-dir", RESULTS_DIR,
        "--mode", "diagnostic",
        "--rep", "1",
        "--amtv-delta-tombstones", str(delta),
        "--amtv-hard-layer-limit", str(hard_limit)
    ]

    print("\n" + "=" * 78)
    print(f"STARTING {cfg_name} : {desc}")
    print(f"Command: {' '.join(cmd)}")
    print("=" * 78)

    t0 = time.time()
    res = subprocess.run(cmd)
    elapsed = time.time() - t0

    if res.returncode != 0:
        print(f"\n[FATAL ERROR] Diagnostic run {exp_id} failed with exit code {res.returncode}!", file=sys.stderr)
        sys.exit(res.returncode)

    json_path = os.path.join(RESULTS_DIR, f"{exp_id}.json")
    if not os.path.exists(json_path):
        print(f"[FATAL ERROR] Expected output JSON not found: {json_path}", file=sys.stderr)
        sys.exit(1)

    with open(json_path) as f:
        data = json.load(f)

    print(f"\n>>> FINISHED {exp_id} in {elapsed:.2f} s")
    print(f"    IOPS: {data.get('fg_iops', 0):.2f}, Get P99: {data.get('get_live_p99_us', 0):.2f} us")
    print(f"    Fallback Events: {data.get('amtv_fallback_events', 0)}, Fallback Gets: {data.get('amtv_fallback_gets', 0)}")
    print(f"    Phase B Peak Chunk Arrival Rate: {data.get('phase_b_del_range_chunk_arrival_rate_chunks_per_sec', 0):.2f} chunks/s")
    print(f"    Max Single Merge Wall Time: {data.get('max_single_merge_wall_time_us', 0):.2f} us (Level {data.get('max_single_merge_level', 0)})")
    print(f"    Drained Distribution Matched: {data.get('theoretical_distribution_matched', False)}")

    # Copy timeline to per-run named file and standard amtv_merge_timeline.csv
    src_tl = os.path.join(RESULTS_DIR, f"{exp_id}_timeline.csv")
    dst_tl = os.path.join(RESULTS_DIR, f"amtv_merge_timeline_{cfg_name.lower().replace('-', '_')}.csv")
    if os.path.exists(src_tl):
        shutil.copyfile(src_tl, dst_tl)
        shutil.copyfile(src_tl, os.path.join(RESULTS_DIR, "amtv_merge_timeline.csv"))
        print(f"    Timeline saved to: {dst_tl}")

    return data

def main():
    print("=" * 78)
    print("M2d.1 AMTV Background Run Merge Capacity & Backlog Diagnostics")
    print("Executing 4 single-round (N=1) diagnostic configurations...")
    print("=" * 78)

    all_results = []
    for cfg_name, delta, hard_limit, desc in DIAGNOSTIC_CONFIGS:
        res = run_diagnostic(cfg_name, delta, hard_limit, desc)
        all_results.append(res)

    # Save summary table
    summary_csv = os.path.join(RESULTS_DIR, "m2d1_diagnostics_summary.csv")
    keys = [
        "config_name", "delta_tombstones", "hard_layer_limit", "fg_iops", "fg_elapsed_sec",
        "phase_b_sec", "get_live_p50_us", "get_live_p95_us", "get_live_p99_us", "get_live_p999_us", "get_live_max_us",
        "get_post_fallback_p99_us", "get_post_fallback_max_us", "get_post_fallback_count",
        "amtv_fallback_events", "amtv_fallback_gets", "runs_at_fallback", "tombstones_at_fallback",
        "chunk_interval_min_us", "chunk_interval_p50_us", "chunk_interval_p95_us", "chunk_interval_max_us",
        "phase_b_del_range_arrival_rate_ops_per_sec", "phase_b_del_range_chunk_arrival_rate_chunks_per_sec",
        "phase_b_end_sealed_runs", "phase_b_end_open_delta_len", "phase_b_end_level_hist",
        "drained_sealed_runs", "drained_open_delta_len", "drained_level_hist", "theoretical_distribution_matched",
        "max_single_merge_wall_time_us", "max_single_merge_cpu_time_us", "max_single_merge_level",
        "backlog_regression_slope", "get_probe_avg_sealed_runs", "get_probe_max_sealed_runs",
        "amtv_merge_completed", "amtv_merge_input_tombstones", "amtv_reconstruction_amplification",
        "engine_output_wa", "drain_elapsed_sec", "db_sha256"
    ]

    with open(summary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        for r in all_results:
            writer.writerow(r)

    print("\n" + "=" * 78)
    print(f"M2d.1 DIAGNOSTICS COMPLETED! Summary table saved to: {summary_csv}")
    print("=" * 78)

if __name__ == "__main__":
    main()
