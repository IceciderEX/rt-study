#!/usr/bin/env python3
import os
import sys
import json
import time
import subprocess
import csv
import numpy as np

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
SEED_DB = os.path.join(STUDY_ROOT, "run-db/m2d_canonical_seed_db")
TRACE_DIR = os.path.join(STUDY_ROOT, "traces/m2d_getonly_dynamic_500k")
BIN_AUDIT = os.path.join(STUDY_ROOT, "bin/m2d_driver_audit")
BIN_RELEASE = os.path.join(STUDY_ROOT, "bin/m2d_driver_release")
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m2d")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d")

CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]

AUDIT_SEEDS = {
    1: 210001,
    2: 220001,
    3: 230001,
}

AUDIT_SCHEDULE = [
    # Rep 1 (seed 210001): Native-T0 -> Native-T512 -> AMTV-T0 -> AMTV-T512
    ("Native-T0", 1), ("Native-T512", 1), ("AMTV-T0", 1), ("AMTV-T512", 1),
    # Rep 2 (seed 220001): Native-T512 -> AMTV-T0 -> AMTV-T512 -> Native-T0
    ("Native-T512", 2), ("AMTV-T0", 2), ("AMTV-T512", 2), ("Native-T0", 2),
    # Rep 3 (seed 230001): AMTV-T0 -> AMTV-T512 -> Native-T0 -> Native-T512
    ("AMTV-T0", 3), ("AMTV-T512", 3), ("Native-T0", 3), ("Native-T512", 3),
]

RELEASE_SEEDS = {
    1: 310001,
    2: 320001,
    3: 330001,
    4: 340001,
    5: 350001,
}

# Release: 5-round balanced preregistered interleaved schedule
RELEASE_SCHEDULE = [
    # Rep 1 (seed 310001): Native-T0 -> Native-T512 -> AMTV-T0 -> AMTV-T512
    ("Native-T0", 1), ("Native-T512", 1), ("AMTV-T0", 1), ("AMTV-T512", 1),
    # Rep 2 (seed 320001): Native-T512 -> AMTV-T0 -> AMTV-T512 -> Native-T0
    ("Native-T512", 2), ("AMTV-T0", 2), ("AMTV-T512", 2), ("Native-T0", 2),
    # Rep 3 (seed 330001): AMTV-T0 -> AMTV-T512 -> Native-T0 -> Native-T512
    ("AMTV-T0", 3), ("AMTV-T512", 3), ("Native-T0", 3), ("Native-T512", 3),
    # Rep 4 (seed 340001): AMTV-T512 -> Native-T0 -> Native-T512 -> AMTV-T0
    ("AMTV-T512", 4), ("Native-T0", 4), ("Native-T512", 4), ("AMTV-T0", 4),
    # Rep 5 (seed 350001): Native-T0 -> AMTV-T512 -> AMTV-T0 -> Native-T512
    ("Native-T0", 5), ("AMTV-T512", 5), ("AMTV-T0", 5), ("Native-T512", 5),
]

def safe_float(v):
    if v is None or v == "N/A" or v == "":
        return None
    try:
        return float(v)
    except (ValueError, TypeError):
        return None

def run_single(mode, cfg_name, rep, binary, output_dir, trace_dir=None):
    exp_id = f"m2d_{mode}_{cfg_name.lower().replace('-', '_')}_rep{rep}"
    db_path = os.path.join(RUN_DB_BASE, exp_id, "db")
    if trace_dir is None:
        if mode == "audit" and rep in AUDIT_SEEDS:
            trace_dir = os.path.join(STUDY_ROOT, f"traces/m2d_audit_rep{rep}_seed{AUDIT_SEEDS[rep]}")
        elif mode == "release" and rep in RELEASE_SEEDS:
            trace_dir = os.path.join(STUDY_ROOT, f"traces/m2d_release_rep{rep}_seed{RELEASE_SEEDS[rep]}")
        else:
            trace_dir = TRACE_DIR
    cmd = [
        "taskset", "-c", "0-19",
        binary,
        "--exp-id", exp_id,
        "--config", cfg_name,
        "--db-path", db_path,
        "--seed-db", SEED_DB,
        "--trace-dir", trace_dir,
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

def run_smoke(binary=BIN_RELEASE, mode="smoke_release"):
    print("\n======================================================================")
    print("STAGE 1: Running Release Smoke Verification (4 configs, Rep 0)")
    print("======================================================================")
    out_dir = os.path.join(RESULTS_DIR, mode)
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for cfg in CONFIGS:
        data = run_single(mode, cfg, 0, binary, out_dir)
        results.append(data)
    
    # Check that all 4 configs have matching SHA-256
    sha0 = results[0]["db_sha256"]
    for r in results:
        cfg = r["config_name"]
        assert r["db_sha256"] == sha0, f"SHA mismatch across smoke runs! {cfg}: {r['db_sha256']} vs {sha0}"
        assert r["expected_model_sha"] == sha0, f"Model SHA mismatch in {cfg}"
        assert r.get("drain_converged", False), f"{cfg} drain did not converge!"
        
        # Check T0/T512 flush gating
        if "T0" in cfg:
            assert r.get("total_capacity_flushes", 0) == 0, f"{cfg} had capacity flushes: {r.get('total_capacity_flushes')}"
        if "T512" in cfg:
            assert r.get("total_capacity_flushes", 0) == 0, f"{cfg} had capacity flushes: {r.get('total_capacity_flushes')}"
            assert r.get("total_threshold_flushes", 0) > 0, f"{cfg} had 0 threshold flushes!"

        # Check fallback
        if "AMTV" in cfg:
            assert r.get("amtv_fallback_events", 0) == 0, f"{cfg} had fallback events: {r.get('amtv_fallback_events')}"
            assert r.get("amtv_fallback_gets", 0) == 0, f"{cfg} had fallback gets: {r.get('amtv_fallback_gets')}"
        else:
            assert r.get("amtv_fallback_events") == "N/A", f"Native {cfg} did not have N/A for fallback events!"
            assert r.get("amtv_sealed_runs") == "N/A", f"Native {cfg} did not have N/A for sealed runs!"

        # Verify 3-window write amplification is present
        assert "pwa_fg" in r, f"Missing pwa_fg in {cfg}"
        assert "pwa_cooldown" in r, f"Missing pwa_cooldown in {cfg}"
        assert "pwa_drain" in r, f"Missing pwa_drain in {cfg}"
        assert "pwa_total" in r, f"Missing pwa_total in {cfg}"
        assert "post_measurement_teardown_output" in r, f"Missing post_measurement_teardown_output in {cfg}"

    print(f"\n[PASS] All 4 Release Smoke runs verified! Canonical Final DB SHA-256: {sha0}")
    return True

def run_release():
    print("\n======================================================================")
    print("STAGE 2: Running Release Performance Matrix (4 configs x 5 reps = 20 runs)")
    print("======================================================================")
    out_dir = os.path.join(RESULTS_DIR, "release")
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for cfg, rep in RELEASE_SCHEDULE:
        data = run_single("release", cfg, rep, BIN_RELEASE, out_dir)
        results.append(data)
        
        # Fail-fast check after each run
        assert data["db_sha256"] == data["expected_model_sha"], f"Model SHA mismatch in {cfg} rep {rep}!"
        assert data.get("drain_converged", False), f"Drain not converged in {cfg} rep {rep}!"
        if "T0" in cfg:
            assert data.get("total_capacity_flushes", 0) == 0, f"{cfg} rep {rep} capacity flushes > 0!"
        if "T512" in cfg:
            assert data.get("total_capacity_flushes", 0) == 0, f"{cfg} rep {rep} capacity flushes > 0!"
            assert data.get("total_threshold_flushes", 0) > 0, f"{cfg} rep {rep} threshold flushes == 0!"
        if "AMTV" in cfg:
            assert data.get("amtv_fallback_events", 0) == 0, f"{cfg} rep {rep} fallback events > 0!"
            assert data.get("amtv_fallback_gets", 0) == 0, f"{cfg} rep {rep} fallback gets > 0!"

    print("\n>>>>> RELEASE MATRIX COMPLETED SUCCESSFULLY (20/20 RUNS PASSED)! <<<<<")
    return results

def compute_bootstrap_ci(data, num_samples=10000, ci=0.95):
    valid = [x for x in data if x is not None]
    if len(valid) == 0:
        return "N/A", "N/A"
    if len(valid) < 2:
        return valid[0], valid[0]
    rng = np.random.default_rng(20260906)
    means = []
    n = len(valid)
    for _ in range(num_samples):
        sample = rng.choice(valid, size=n, replace=True)
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
        "fg_elapsed_sec", "fg_iops",
        "phase_a_sec", "phase_b_sec", "phase_c_sec",
        "get_live_p50_us", "get_live_p90_us", "get_live_p95_us", "get_live_p99_us", "get_live_p999_us", "get_live_max_us",
        "put_p50_us", "put_p95_us", "put_p99_us", "put_max_us",
        "delete_range_p50_us", "delete_range_p95_us", "delete_range_p99_us", "delete_range_max_us",
        "pwa_fg", "pwa_cooldown", "pwa_drain", "pwa_total",
        "w1_flush_bytes", "w1_compaction_read_bytes", "w1_compaction_write_bytes",
        "w2_flush_bytes", "w2_compaction_read_bytes", "w2_compaction_write_bytes",
        "w3_flush_bytes", "w3_compaction_read_bytes", "w3_compaction_write_bytes",
        "w_total_flush_bytes", "w_total_compaction_read_bytes", "w_total_compaction_write_bytes",
        "post_teardown_flush_bytes", "post_teardown_compaction_write_bytes",
        "fg_capacity_flushes", "fg_threshold_flushes", "total_capacity_flushes", "total_threshold_flushes",
        "user_cpu_sec", "sys_cpu_sec", "peak_rss_kb", "drain_elapsed_sec",
        "amtv_merge_computed", "amtv_merge_published", "amtv_merge_discarded",
        "amtv_merge_wall_time_us", "amtv_merge_cpu_time_us",
        "amtv_raw_entries_struct_bytes_peak", "amtv_inflight_payload_proxy_bytes_peak"
    ]

    rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        if not runs:
            continue
        row = {"config_name": cfg, "n_runs": len(runs)}
        for m in metrics:
            vals = [safe_float(r.get(m)) for r in runs]
            valid_vals = [v for v in vals if v is not None]
            if len(valid_vals) == 0:
                row[f"{m}_mean"] = "N/A"
                row[f"{m}_std"] = "N/A"
                row[f"{m}_median"] = "N/A"
                row[f"{m}_iqr"] = "N/A"
                row[f"{m}_ci95_low"] = "N/A"
                row[f"{m}_ci95_high"] = "N/A"
            else:
                mean_val = np.mean(valid_vals)
                std_val = np.std(valid_vals, ddof=1) if len(valid_vals) > 1 else 0.0
                med_val = np.median(valid_vals)
                iqr_val = np.percentile(valid_vals, 75) - np.percentile(valid_vals, 25) if len(valid_vals) > 1 else 0.0
                ci_low, ci_high = compute_bootstrap_ci(valid_vals)
                row[f"{m}_mean"] = f"{mean_val:.4f}"
                row[f"{m}_std"] = f"{std_val:.4f}"
                row[f"{m}_median"] = f"{med_val:.4f}"
                row[f"{m}_iqr"] = f"{iqr_val:.4f}"
                row[f"{m}_ci95_low"] = f"{ci_low:.4f}" if isinstance(ci_low, float) else ci_low
                row[f"{m}_ci95_high"] = f"{ci_high:.4f}" if isinstance(ci_high, float) else ci_high
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

    # Generate paired comparison summary
    paired_csv_path = os.path.join(summary_dir, f"amtv-m2d-paired-diffs-{mode}.csv")
    pairs = [
        ("Native-T0", "AMTV-T0", "T0_Pair"),
        ("Native-T512", "AMTV-T512", "T512_Pair"),
    ]
    paired_rows = []
    for c_native, c_amtv, pair_name in pairs:
        native_runs = {r["rep"]: r for r in by_cfg.get(c_native, [])}
        amtv_runs = {r["rep"]: r for r in by_cfg.get(c_amtv, [])}
        common_reps = sorted(set(native_runs.keys()) & set(amtv_runs.keys()))
        if not common_reps:
            continue
        
        # Diff metrics: Phase B time ratio, Overall time ratio, IOPS ratio, P99 latency ratio, PWA diff
        phase_b_diffs = [native_runs[rep]["phase_b_sec"] - amtv_runs[rep]["phase_b_sec"] for rep in common_reps]
        phase_b_ratios = [native_runs[rep]["phase_b_sec"] / amtv_runs[rep]["phase_b_sec"] for rep in common_reps]
        fg_time_ratios = [native_runs[rep]["fg_elapsed_sec"] / amtv_runs[rep]["fg_elapsed_sec"] for rep in common_reps]
        iops_ratios = [amtv_runs[rep]["fg_iops"] / native_runs[rep]["fg_iops"] for rep in common_reps]
        p99_ratios = [native_runs[rep]["get_live_p99_us"] / amtv_runs[rep]["get_live_p99_us"] for rep in common_reps]
        
        for name, series in [
            ("Phase B Elapsed Diff (s)", phase_b_diffs),
            ("Phase B Elapsed Ratio (Native/AMTV)", phase_b_ratios),
            ("Foreground Elapsed Ratio (Native/AMTV)", fg_time_ratios),
            ("IOPS Ratio (AMTV/Native)", iops_ratios),
            ("GetLive P99 Ratio (Native/AMTV)", p99_ratios),
        ]:
            paired_rows.append({
                "pair": pair_name,
                "metric": name,
                "mean": f"{np.mean(series):.4f}",
                "std": f"{np.std(series, ddof=1) if len(series) > 1 else 0.0:.4f}",
                "median": f"{np.median(series):.4f}",
                "iqr": f"{(np.percentile(series, 75) - np.percentile(series, 25)):.4f}" if len(series) > 1 else "0.0000"
            })
    if paired_rows:
        with open(paired_csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["pair", "metric", "mean", "std", "median", "iqr"])
            writer.writeheader()
            writer.writerows(paired_rows)
        print(f"Saved paired diffs CSV to: {paired_csv_path}")

def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "release"
    if stage in ["smoke", "all"]:
        run_smoke()
    if stage in ["release", "all"]:
        rel_res = run_release()
        summarize_metrics(rel_res, "release")

if __name__ == "__main__":
    main()
