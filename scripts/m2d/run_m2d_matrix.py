#!/usr/bin/env python3
import os
import sys
import json
import time
import subprocess
import random
import csv
import numpy as np

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
SEED_DB = os.path.join(STUDY_ROOT, "run-db/m2d_canonical_seed_db")
TRACE_DIR = os.path.join(STUDY_ROOT, "traces/m2d_getonly_dynamic_500k")
BIN_AUDIT = os.path.join(STUDY_ROOT, "bin/m2d_driver_audit")
BIN_RELEASE = os.path.join(STUDY_ROOT, "bin/m2d_driver_release")
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m2d")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d")

CONFIGS = ["Native-T0", "Native-T512", "AMTV-M2c-T0", "AMTV-M2c-T512"]

# Audit: 3-round balanced interleaved schedule
AUDIT_SCHEDULE = [
    ("Native-T0", 1), ("Native-T512", 1), ("AMTV-M2c-T0", 1), ("AMTV-M2c-T512", 1),
    ("Native-T512", 2), ("AMTV-M2c-T0", 2), ("AMTV-M2c-T512", 2), ("Native-T0", 2),
    ("AMTV-M2c-T0", 3), ("AMTV-M2c-T512", 3), ("Native-T0", 3), ("Native-T512", 3),
]

# Release: 4x4 Latin Square + Round 5 fixed-seed permutation
rng_r5 = random.Random(20260906)
r5_configs = CONFIGS.copy()
rng_r5.shuffle(r5_configs)

RELEASE_SCHEDULE = [
    # Round 1 (Latin Row 0)
    ("Native-T0", 1), ("Native-T512", 1), ("AMTV-M2c-T0", 1), ("AMTV-M2c-T512", 1),
    # Round 2 (Latin Row 1)
    ("Native-T512", 2), ("AMTV-M2c-T512", 2), ("Native-T0", 2), ("AMTV-M2c-T0", 2),
    # Round 3 (Latin Row 2)
    ("AMTV-M2c-T0", 3), ("Native-T0", 3), ("AMTV-M2c-T512", 3), ("Native-T512", 3),
    # Round 4 (Latin Row 3)
    ("AMTV-M2c-T512", 4), ("AMTV-M2c-T0", 4), ("Native-T512", 4), ("Native-T0", 4),
    # Round 5 (Preregistered Permutation)
    (r5_configs[0], 5), (r5_configs[1], 5), (r5_configs[2], 5), (r5_configs[3], 5),
]

def run_single(mode, cfg_name, rep, binary, output_dir):
    exp_id = f"m2d_{mode}_{cfg_name.lower().replace('-', '_')}_rep{rep}"
    db_path = os.path.join(RUN_DB_BASE, exp_id, "db")
    cmd = [
        "taskset", "-c", "0-19",
        binary,
        "--exp-id", exp_id,
        "--config", cfg_name,
        "--db-path", db_path,
        "--seed-db", SEED_DB,
        "--trace-dir", TRACE_DIR,
        "--output-dir", output_dir,
        "--mode", mode,
        "--rep", str(rep)
    ]
    print(f"\n>>> Running [{mode.upper()}] {cfg_name} (Rep {rep}) -> ExpID: {exp_id}...")
    t0 = time.time()
    res = subprocess.run(cmd)
    elapsed = time.time() - t0
    if res.returncode != 0:
        print(f"\n[FATAL ERROR] Run {exp_id} failed with exit code {res.returncode}!", file=sys.stderr)
        sys.exit(res.returncode)
    print(f">>> Completed {exp_id} in {elapsed:.2f} s")
    
    # Verify output JSON exists
    json_path = os.path.join(output_dir, f"{exp_id}.json")
    if not os.path.exists(json_path):
        print(f"[FATAL ERROR] Expected JSON not found: {json_path}", file=sys.stderr)
        sys.exit(1)
    with open(json_path) as f:
        data = json.load(f)
    return data

def run_smoke():
    print("\n======================================================================")
    print("STAGE 1: Running Smoke Verification (4 configs, Rep 0, Audit Mode)")
    print("======================================================================")
    out_dir = os.path.join(RESULTS_DIR, "smoke")
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for cfg in CONFIGS:
        data = run_single("smoke", cfg, 0, BIN_AUDIT, out_dir)
        results.append(data)
    
    # Check that all 4 configs have matching SHA-256
    sha0 = results[0]["db_sha256"]
    for r in results:
        assert r["db_sha256"] == sha0, f"SHA mismatch across smoke runs! {r['config_name']}: {r['db_sha256']} vs {sha0}"
        assert r["expected_model_sha"] == sha0, f"Model SHA mismatch in {r['config_name']}"
    print(f"\n[PASS] All 4 Smoke runs verified! Canonical Final DB SHA-256: {sha0}")
    return True

def run_audit():
    print("\n======================================================================")
    print("STAGE 2: Running Audit Matrix (4 configs x 3 reps = 12 runs)")
    print("======================================================================")
    out_dir = os.path.join(RESULTS_DIR, "audit")
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for cfg, rep in AUDIT_SCHEDULE:
        data = run_single("audit", cfg, rep, BIN_AUDIT, out_dir)
        results.append(data)
        
    print("\n======================================================================")
    print("Evaluating AMTV M2d Audit Gate Conditions:")
    print("======================================================================")
    # Check Audit Gate
    for r in results:
        cfg = r["config_name"]
        rep = r["rep"]
        # Gate 1: AMTV-M2c-T0 active MemTable materialization == 0
        if cfg == "AMTV-M2c-T0":
            mat_count = r.get("audit_mat_count", 0)
            print(f"  [{cfg} Rep {rep}] Active MemTable Materialization Count: {mat_count} (Req: 0)")
            if mat_count != 0:
                print(f"[FAIL GATE] {cfg} rep {rep} had {mat_count} active MemTable materializations!", file=sys.stderr)
                sys.exit(1)
                
            fallback_cnt = r.get("amtv_fallback_events", 0)
            print(f"  [{cfg} Rep {rep}] Fallback Count: {fallback_cnt} (Req: 0)")
            if fallback_cnt != 0:
                print(f"[FAIL GATE] {cfg} rep {rep} had {fallback_cnt} fallback events!", file=sys.stderr)
                sys.exit(1)
                
        # Gate 2: T0 natural capacity flushes == 0
        if cfg in ["Native-T0", "AMTV-M2c-T0"]:
            cap_flushes = r.get("fg_capacity_flushes", 0)
            print(f"  [{cfg} Rep {rep}] Foreground Capacity Flushes: {cap_flushes} (Req: 0)")
            if cap_flushes != 0:
                print(f"[FAIL GATE] {cfg} rep {rep} had {cap_flushes} capacity flushes!", file=sys.stderr)
                sys.exit(1)
                
        # Gate 3: Bit-for-bit DB & Model SHA match
        if r["db_sha256"] != r["expected_model_sha"]:
            print(f"[FAIL GATE] {cfg} rep {rep} SHA mismatch! DB: {r['db_sha256']}, Model: {r['expected_model_sha']}", file=sys.stderr)
            sys.exit(1)
            
        # Gate 4: Drain window converged
        if not r.get("drain_converged", False):
            print(f"[FAIL GATE] {cfg} rep {rep} drain window did not converge!", file=sys.stderr)
            sys.exit(1)

    print("\n>>>>> AUDIT GATE 100% PASSED! ALL MECHANISM INVARIANTS SATISFIED! <<<<<")
    return results

def run_release():
    print("\n======================================================================")
    print("STAGE 3: Running Release Performance Matrix (4 configs x 5 reps = 20 runs)")
    print("======================================================================")
    out_dir = os.path.join(RESULTS_DIR, "release")
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for cfg, rep in RELEASE_SCHEDULE:
        data = run_single("release", cfg, rep, BIN_RELEASE, out_dir)
        results.append(data)
    print("\n>>>>> RELEASE MATRIX COMPLETED SUCCESSFULLY! <<<<<")
    return results

def compute_bootstrap_ci(data, num_samples=10000, ci=0.95):
    if len(data) < 2:
        val = data[0] if len(data) == 1 else 0.0
        return val, val
    rng = np.random.default_rng(20260906)
    means = []
    n = len(data)
    for _ in range(num_samples):
        sample = rng.choice(data, size=n, replace=True)
        means.append(np.mean(sample))
    lower = np.percentile(means, (1.0 - ci) / 2.0 * 100.0)
    upper = np.percentile(means, (1.0 + ci) / 2.0 * 100.0)
    return lower, upper

def summarize_metrics(results, mode):
    summary_dir = os.path.join(STUDY_ROOT, "results/summary")
    os.makedirs(summary_dir, exist_ok=True)
    csv_path = os.path.join(summary_dir, f"amtv-m2d-{mode}.csv")
    
    by_cfg = {}
    for r in results:
        c = r["config_name"]
        by_cfg.setdefault(c, []).append(r)
        
    metrics = [
        "fg_elapsed_sec", "fg_iops", "get_live_p50_us", "get_live_p95_us", "get_live_p99_us", "get_live_p999_us",
        "put_p95_us", "put_p99_us", "delete_range_p95_us", "delete_range_p99_us",
        "fg_capacity_flushes", "fg_threshold_flushes", "fg_flush_bytes", "fg_compaction_write_bytes", "engine_output_wa",
        "user_cpu_sec", "sys_cpu_sec", "peak_rss_kb", "drain_elapsed_sec",
        "amtv_merge_completed", "amtv_merge_cpu_time_us", "amtv_raw_entries_struct_bytes_peak"
    ]
    if mode == "audit":
        metrics.extend([
            "audit_mat_count", "audit_mat_nanos", "audit_cache_inv_count",
            "audit_lock_attempt_count", "audit_lock_contended_count", "audit_lock_wait_nanos"
        ])

    rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        if not runs:
            continue
        row = {"config_name": cfg, "n_runs": len(runs)}
        for m in metrics:
            vals = [float(r.get(m, 0.0)) for r in runs]
            mean_val = np.mean(vals)
            std_val = np.std(vals, ddof=1) if len(vals) > 1 else 0.0
            med_val = np.median(vals)
            iqr_val = np.percentile(vals, 75) - np.percentile(vals, 25) if len(vals) > 1 else 0.0
            ci_low, ci_high = compute_bootstrap_ci(vals)
            row[f"{m}_mean"] = f"{mean_val:.4f}"
            row[f"{m}_std"] = f"{std_val:.4f}"
            row[f"{m}_median"] = f"{med_val:.4f}"
            row[f"{m}_iqr"] = f"{iqr_val:.4f}"
            row[f"{m}_ci95_low"] = f"{ci_low:.4f}"
            row[f"{m}_ci95_high"] = f"{ci_high:.4f}"
        rows.append(row)
        
    if rows:
        fieldnames = list(rows[0].keys())
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)
        print(f"Saved summary CSV to: {csv_path}")

    # Generate phase breakdown CSV
    phase_csv_path = os.path.join(summary_dir, f"amtv-m2d-phase-breakdown-{mode}.csv")
    phase_rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        if not runs:
            continue
        for p in ["a", "b", "c"]:
            p_times = [float(r.get(f"phase_{p}_sec", 0.0)) for r in runs]
            phase_rows.append({
                "config_name": cfg,
                "phase": p.upper(),
                "mean_sec": f"{np.mean(p_times):.4f}",
                "std_sec": f"{np.std(p_times, ddof=1) if len(p_times) > 1 else 0.0:.4f}",
                "median_sec": f"{np.median(p_times):.4f}"
            })
    with open(phase_csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["config_name", "phase", "mean_sec", "std_sec", "median_sec"])
        writer.writeheader()
        writer.writerows(phase_rows)
    print(f"Saved phase breakdown CSV to: {phase_csv_path}")

def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "all"
    if stage in ["smoke", "all"]:
        run_smoke()
    if stage in ["audit", "all"]:
        audit_res = run_audit()
        summarize_metrics(audit_res, "audit")
    if stage in ["release", "all"]:
        rel_res = run_release()
        summarize_metrics(rel_res, "release")

if __name__ == "__main__":
    main()
