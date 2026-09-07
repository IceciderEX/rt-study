#!/usr/bin/env python3
import os
import glob
import json
import csv
import numpy as np

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
RELEASE_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d/release")
SUMMARY_DIR = os.path.join(STUDY_ROOT, "results/summary")
os.makedirs(SUMMARY_DIR, exist_ok=True)

CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]

def safe_float(v):
    if v is None or v == "N/A" or v == "":
        return None
    try:
        return float(v)
    except (ValueError, TypeError):
        return None

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

def stat_dict(valid_vals):
    if not valid_vals:
        return {"mean": "N/A", "std": "N/A", "median": "N/A", "iqr": "N/A", "ci95_low": "N/A", "ci95_high": "N/A"}
    mean_val = np.mean(valid_vals)
    std_val = np.std(valid_vals, ddof=1) if len(valid_vals) > 1 else 0.0
    med_val = np.median(valid_vals)
    iqr_val = np.percentile(valid_vals, 75) - np.percentile(valid_vals, 25) if len(valid_vals) > 1 else 0.0
    ci_low, ci_high = compute_bootstrap_ci(valid_vals)
    return {
        "mean": f"{mean_val:.4f}",
        "std": f"{std_val:.4f}",
        "median": f"{med_val:.4f}",
        "iqr": f"{iqr_val:.4f}",
        "ci95_low": f"{ci_low:.4f}" if isinstance(ci_low, float) else ci_low,
        "ci95_high": f"{ci_high:.4f}" if isinstance(ci_high, float) else ci_high
    }

def main():
    json_files = glob.glob(os.path.join(RELEASE_DIR, "m2d_release_*.json"))
    by_cfg = {}
    for fpath in sorted(json_files):
        with open(fpath) as f:
            data = json.load(f)
        cfg = data["config_name"]
        by_cfg.setdefault(cfg, []).append(data)

    print(f"Loaded {len(json_files)} run JSON files across {len(by_cfg)} configurations.")

    # 1. Primary Metrics Summary
    primary_metrics = [
        "fg_elapsed_sec", "fg_iops",
        "phase_a_sec", "phase_b_sec", "phase_c_sec",
        "get_live_p50_us", "get_live_p95_us", "get_live_p99_us", "get_live_p999_us", "get_live_max_us",
        "put_p50_us", "put_p95_us", "put_p99_us", "put_p999_us", "put_max_us",
        "delete_range_p50_us", "delete_range_p95_us", "delete_range_p99_us", "delete_range_p999_us", "delete_range_max_us",
        "pwa_fg", "pwa_cooldown", "pwa_drain", "pwa_total",
        "window1_foreground_flush_bytes", "window1_foreground_compaction_write_bytes", "window1_foreground_compaction_read_bytes", "window1_foreground_output_bytes",
        "window2_cooldown_flush_bytes", "window2_cooldown_compaction_write_bytes", "window2_cooldown_compaction_read_bytes", "window2_cooldown_output_bytes",
        "window3_drain_flush_bytes", "window3_drain_compaction_write_bytes", "window3_drain_compaction_read_bytes", "window3_drain_output_bytes",
        "three_window_flush_bytes", "three_window_compaction_write_bytes", "three_window_compaction_read_bytes", "three_window_output_bytes",
        "post_measurement_teardown_flush_bytes", "post_measurement_teardown_compaction_write_bytes", "post_measurement_teardown_output",
        "fg_capacity_flushes", "fg_threshold_flushes", "total_capacity_flushes", "total_threshold_flushes",
        "user_cpu_sec", "sys_cpu_sec", "peak_rss_kb", "drain_elapsed_sec",
        "amtv_merge_computed", "amtv_merge_published", "amtv_merge_discarded",
        "amtv_merge_wall_time_us", "amtv_merge_cpu_time_us",
        "amtv_raw_entries_struct_bytes_peak", "amtv_inflight_payload_proxy_bytes_peak"
    ]

    primary_rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        row = {"config_name": cfg, "n_runs": len(runs)}
        for m in primary_metrics:
            vals = [safe_float(r.get(m)) for r in runs]
            valid_vals = [v for v in vals if v is not None]
            st = stat_dict(valid_vals)
            row[f"{m}_mean"] = st["mean"]
            row[f"{m}_std"] = st["std"]
            row[f"{m}_median"] = st["median"]
            row[f"{m}_iqr"] = st["iqr"]
            row[f"{m}_ci95_low"] = st["ci95_low"]
            row[f"{m}_ci95_high"] = st["ci95_high"]
        primary_rows.append(row)

    primary_csv = os.path.join(SUMMARY_DIR, "amtv-m2d-release.csv")
    with open(primary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(primary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(primary_rows)
    print(f"Generated: {primary_csv}")

    # 2. 3-Window IO & Write Amplification Table
    window_csv = os.path.join(SUMMARY_DIR, "amtv-m2d-3window-io-release.csv")
    window_rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        row = {"config_name": cfg, "n_runs": len(runs)}
        for m in [
            "window1_foreground_flush_bytes", "window1_foreground_compaction_write_bytes", "window1_foreground_output_bytes", "pwa_fg",
            "window2_cooldown_flush_bytes", "window2_cooldown_compaction_write_bytes", "window2_cooldown_output_bytes", "pwa_cooldown",
            "window3_drain_flush_bytes", "window3_drain_compaction_write_bytes", "window3_drain_output_bytes", "pwa_drain",
            "three_window_flush_bytes", "three_window_compaction_write_bytes", "three_window_output_bytes", "pwa_total",
            "post_measurement_teardown_output", "total_capacity_flushes", "total_threshold_flushes"
        ]:
            vals = [safe_float(r.get(m)) for r in runs]
            valid = [v for v in vals if v is not None]
            st = stat_dict(valid)
            row[f"{m}_mean"] = st["mean"]
            row[f"{m}_std"] = st["std"]
            row[f"{m}_median"] = st["median"]
            row[f"{m}_iqr"] = st["iqr"]
        window_rows.append(row)

    with open(window_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(window_rows[0].keys()))
        writer.writeheader()
        writer.writerows(window_rows)
    print(f"Generated: {window_csv}")

    # 3. Phase Breakdown
    phase_csv = os.path.join(SUMMARY_DIR, "amtv-m2d-phase-breakdown-release.csv")
    phase_rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        for p, op_cnt in [("a", 100000), ("b", 100000), ("c", 100000)]:
            p_times = [float(r[f"phase_{p}_sec"]) for r in runs]
            p_iops = [op_cnt / t for t in p_times]
            st_t = stat_dict(p_times)
            st_iops = stat_dict(p_iops)
            phase_rows.append({
                "config_name": cfg,
                "phase": p.upper(),
                "duration_mean_sec": st_t["mean"],
                "duration_std_sec": st_t["std"],
                "duration_median_sec": st_t["median"],
                "duration_iqr_sec": st_t["iqr"],
                "iops_mean": st_iops["mean"],
                "iops_std": st_iops["std"],
                "iops_median": st_iops["median"],
                "iops_iqr": st_iops["iqr"],
            })
    with open(phase_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(phase_rows[0].keys()))
        writer.writeheader()
        writer.writerows(phase_rows)
    print(f"Generated: {phase_csv}")

    # 4. Latency Quantiles Summary (Overall and Phase B)
    lat_csv = os.path.join(SUMMARY_DIR, "amtv-m2d-latency-quantiles-release.csv")
    lat_rows = []
    for cfg in CONFIGS:
        runs = by_cfg.get(cfg, [])
        for op in ["get", "put", "del"]:
            for ph in ["phase_a", "phase_b", "phase_c", "overall"]:
                if ph in ["phase_a", "phase_c"] and op == "del":
                    continue
                q_p50 = [r["latencies"][ph][op]["p50"] for r in runs]
                q_p95 = [r["latencies"][ph][op]["p95"] for r in runs]
                q_p99 = [r["latencies"][ph][op]["p99"] for r in runs]
                q_p999 = [r["latencies"][ph][op]["p999"] for r in runs]
                q_max = [r["latencies"][ph][op]["max"] for r in runs]
                lat_rows.append({
                    "config_name": cfg,
                    "phase": ph,
                    "op_type": op.upper(),
                    "p50_us_mean": f"{np.mean(q_p50):.4f}",
                    "p50_us_median": f"{np.median(q_p50):.4f}",
                    "p95_us_mean": f"{np.mean(q_p95):.4f}",
                    "p95_us_median": f"{np.median(q_p95):.4f}",
                    "p99_us_mean": f"{np.mean(q_p99):.4f}",
                    "p99_us_median": f"{np.median(q_p99):.4f}",
                    "p999_us_mean": f"{np.mean(q_p999):.4f}",
                    "p999_us_median": f"{np.median(q_p999):.4f}",
                    "max_us_mean": f"{np.mean(q_max):.4f}",
                    "max_us_median": f"{np.median(q_max):.4f}",
                })
    with open(lat_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(lat_rows[0].keys()))
        writer.writeheader()
        writer.writerows(lat_rows)
    print(f"Generated: {lat_csv}")

    # 5. Paired Comparisons
    paired_csv = os.path.join(SUMMARY_DIR, "amtv-m2d-paired-diffs-release.csv")
    pairs = [
        ("Native-T0", "AMTV-T0", "T0_Pair"),
        ("Native-T512", "AMTV-T512", "T512_Pair"),
    ]
    paired_rows = []
    for c_native, c_amtv, pair_name in pairs:
        native_runs = {r["rep"]: r for r in by_cfg[c_native]}
        amtv_runs = {r["rep"]: r for r in by_cfg[c_amtv]}
        common_reps = sorted(set(native_runs.keys()) & set(amtv_runs.keys()))
        
        diff_specs = [
            ("Phase B Duration Diff (s, Native - AMTV)", [native_runs[rep]["phase_b_sec"] - amtv_runs[rep]["phase_b_sec"] for rep in common_reps]),
            ("Phase B Duration Ratio (Native / AMTV)", [native_runs[rep]["phase_b_sec"] / amtv_runs[rep]["phase_b_sec"] for rep in common_reps]),
            ("Phase B IOPS Ratio (AMTV / Native)", [(100000.0 / amtv_runs[rep]["phase_b_sec"]) / (100000.0 / native_runs[rep]["phase_b_sec"]) for rep in common_reps]),
            ("Foreground Duration Diff (s, Native - AMTV)", [native_runs[rep]["fg_elapsed_sec"] - amtv_runs[rep]["fg_elapsed_sec"] for rep in common_reps]),
            ("Foreground Duration Ratio (Native / AMTV)", [native_runs[rep]["fg_elapsed_sec"] / amtv_runs[rep]["fg_elapsed_sec"] for rep in common_reps]),
            ("Overall IOPS Ratio (AMTV / Native)", [amtv_runs[rep]["fg_iops"] / native_runs[rep]["fg_iops"] for rep in common_reps]),
            ("GetLive P50 Ratio (Native / AMTV)", [native_runs[rep]["get_live_p50_us"] / amtv_runs[rep]["get_live_p50_us"] for rep in common_reps]),
            ("GetLive P95 Ratio (Native / AMTV)", [native_runs[rep]["get_live_p95_us"] / amtv_runs[rep]["get_live_p95_us"] for rep in common_reps]),
            ("GetLive P99 Ratio (Native / AMTV)", [native_runs[rep]["get_live_p99_us"] / amtv_runs[rep]["get_live_p99_us"] for rep in common_reps]),
            ("Phase B GetLive P99 Ratio (Native / AMTV)", [native_runs[rep]["latencies"]["phase_b"]["get"]["p99"] / amtv_runs[rep]["latencies"]["phase_b"]["get"]["p99"] for rep in common_reps]),
            ("Three-Window Write Amp Diff (Native - AMTV)", [native_runs[rep]["pwa_total"] - amtv_runs[rep]["pwa_total"] for rep in common_reps]),
            ("User CPU Diff (s, Native - AMTV)", [native_runs[rep]["user_cpu_sec"] - amtv_runs[rep]["user_cpu_sec"] for rep in common_reps]),
        ]

        for name, series in diff_specs:
            st = stat_dict(series)
            paired_rows.append({
                "pair": pair_name,
                "metric": name,
                "mean": st["mean"],
                "std": st["std"],
                "median": st["median"],
                "iqr": st["iqr"],
                "ci95_low": st["ci95_low"],
                "ci95_high": st["ci95_high"]
            })

    with open(paired_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(paired_rows[0].keys()))
        writer.writeheader()
        writer.writerows(paired_rows)
    print(f"Generated: {paired_csv}")

if __name__ == "__main__":
    main()
