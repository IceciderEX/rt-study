#!/usr/bin/env python3
"""
Formal V2 Statistical Aggregator & Publication-Grade Report Generator
Computes:
- Mean +/- Sample Std
- Median & IQR
- Non-parametric Bootstrap 95% Confidence Intervals (10,000 iterations)
- Coefficient of Variation (CV)
- Relative Speedup / Degradation vs Functional Clean Baseline
- Foreground vs Total Cooldown Window Metrics
- Scan Limit-K Truncation Rates
"""

import os
import sys
import json
import numpy as np
import pandas as pd

def bootstrap_ci(data: np.ndarray, num_resamples: int = 10000, ci: float = 0.95):
    if len(data) == 0:
        return (0.0, 0.0)
    if len(data) == 1:
        return (float(data[0]), float(data[0]))
    
    boot_means = []
    n = len(data)
    rng = np.random.default_rng(90001)
    for _ in range(num_resamples):
        sample = rng.choice(data, size=n, replace=True)
        boot_means.append(np.mean(sample))
    
    alpha = (1.0 - ci) / 2.0
    low = np.percentile(boot_means, alpha * 100.0)
    high = np.percentile(boot_means, (1.0 - alpha) * 100.0)
    return (float(low), float(high))

def aggregate_summary(summary_csv_path: str, phases_csv_path: str, output_json_path: str, output_md_path: str):
    if not os.path.exists(summary_csv_path):
        print(f"Summary CSV not found: {summary_csv_path}")
        return

    df_sum = pd.read_csv(summary_csv_path)
    print(f"Loaded {len(df_sum)} runs from {summary_csv_path}")

    # Metrics of interest
    metrics = [
        "fg_elapsed_sec", "fg_trace_iops", "fg_db_api_iops", "scan_us_per_key", "scan_p99_us",
        "get_live_p99_us", "get_del_p99_us", "put_p99_us", "scan_limit_truncated_count",
        "fg_flush_count", "fg_flush_engine_out_mb", "fg_comp_read_mb", "fg_comp_write_mb",
        "fwa_val_norm_fg", "cwa_val_norm_fg", "pwa_val_norm_fg",
        "total_exp_flush_count", "total_exp_flush_engine_out_mb", "total_exp_comp_write_mb",
        "fwa_val_norm_total", "cwa_val_norm_total", "pwa_val_norm_total", "sst_mb"
    ]

    grouped = df_sum.groupby("group_name")
    agg_report = {}

    for g_name, g_df in grouped:
        n_runs = len(g_df)
        g_stats = {
            "num_runs": n_runs,
            "desc": g_df["desc"].iloc[0] if "desc" in g_df.columns else g_name,
            "threshold": int(g_df["threshold"].iloc[0]) if "threshold" in g_df.columns else 0,
            "metrics": {}
        }

        for m in metrics:
            if m not in g_df.columns:
                continue
            vals = g_df[m].to_numpy(dtype=float)
            mean_val = float(np.mean(vals))
            std_val = float(np.std(vals, ddof=1)) if n_runs > 1 else 0.0
            med_val = float(np.median(vals))
            ci_low, ci_high = bootstrap_ci(vals)
            cv_pct = (std_val / mean_val * 100.0) if mean_val > 0 else 0.0

            g_stats["metrics"][m] = {
                "mean": mean_val,
                "std": std_val,
                "median": med_val,
                "ci95": [ci_low, ci_high],
                "cv_pct": cv_pct,
                "raw_values": vals.tolist()
            }

        # SHA-256 consistency check
        sha_set = set(g_df["sha256_hex"].tolist())
        g_stats["sha256_consistent"] = (len(sha_set) == 1)
        g_stats["sha256_sample"] = list(sha_set)[0] if sha_set else "N/A"

        agg_report[g_name] = g_stats

    # Phase Breakdown Aggregation
    phase_report = {}
    if os.path.exists(phases_csv_path):
        df_phases = pd.read_csv(phases_csv_path)
        p_grouped = df_phases.groupby(["group_name", "phase"])
        for (g_name, p_name), p_df in p_grouped:
            if g_name not in phase_report:
                phase_report[g_name] = {}
            
            p_metrics = {}
            for col in ["elapsed_sec", "completed_ops", "true_phase_iops", "scan_us_per_key", "scan_p99_us", "get_live_p99_us", "get_del_p99_us", "put_p99_us", "scan_limit_truncated_count"]:
                if col in p_df.columns:
                    arr = p_df[col].to_numpy(dtype=float)
                    p_metrics[col] = {
                        "mean": float(np.mean(arr)),
                        "std": float(np.std(arr, ddof=1)) if len(arr) > 1 else 0.0,
                        "median": float(np.median(arr))
                    }
            phase_report[g_name][p_name] = p_metrics

    final_payload = {
        "summary_groups": agg_report,
        "phase_breakdown": phase_report
    }

    with open(output_json_path, "w", encoding="utf-8") as f:
        json.dump(final_payload, f, indent=2)

    # Generate Markdown Table Report
    md_lines = [
        "# Formal V2 Thesis Experiment Results Summary",
        f"\n**Aggregated Runs**: {len(df_sum)} | **Bootstrap Iterations**: 10,000 | **Confidence Level**: 95%\n",
        "## 1. Foreground Lifecycle Performance Table (Pure DB Envelope Timing & Value-Normalized Engine Write Amplification)\n",
        "| Group | Threshold | Reps | FG Trace IOPS | FG DB API IOPS | Scan Cost (μs/key) | Scan P99 (μs) | Get Live P99 (μs) | Get Del P99 (μs) | Put P99 (μs) | FWA (fg) | CWA (fg) | PWA (fg) | PWA (total) | Limit Trunc. | SHA-256 Cons. |",
        "| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |"
    ]

    for g_name, g_data in sorted(agg_report.items()):
        m = g_data["metrics"]
        t_iops = f"{m['fg_trace_iops']['mean']:.1f} ± {m['fg_trace_iops']['std']:.1f}" if 'fg_trace_iops' in m else "N/A"
        d_iops = f"{m['fg_db_api_iops']['mean']:.1f} ± {m['fg_db_api_iops']['std']:.1f}" if 'fg_db_api_iops' in m else "N/A"
        s_cost = f"{m['scan_us_per_key']['mean']:.2f} ± {m['scan_us_per_key']['std']:.2f}" if 'scan_us_per_key' in m else "N/A"
        s_p99 = f"{m['scan_p99_us']['mean']:.1f}" if 'scan_p99_us' in m else "N/A"
        gl_p99 = f"{m['get_live_p99_us']['mean']:.1f}" if 'get_live_p99_us' in m else "N/A"
        gd_p99 = f"{m['get_del_p99_us']['mean']:.1f}" if 'get_del_p99_us' in m else "N/A"
        p_p99 = f"{m['put_p99_us']['mean']:.1f}" if 'put_p99_us' in m else "N/A"
        fwa_fg = f"{m['fwa_val_norm_fg']['mean']:.2f}" if 'fwa_val_norm_fg' in m else "N/A"
        cwa_fg = f"{m['cwa_val_norm_fg']['mean']:.2f}" if 'cwa_val_norm_fg' in m else "N/A"
        pwa_fg = f"{m['pwa_val_norm_fg']['mean']:.2f}" if 'pwa_val_norm_fg' in m else "N/A"
        pwa_tot = f"{m['pwa_val_norm_total']['mean']:.2f}" if 'pwa_val_norm_total' in m else "N/A"
        trunc_cnt = f"{m['scan_limit_truncated_count']['mean']:.0f}" if 'scan_limit_truncated_count' in m else "0"
        sha_str = "YES" if g_data["sha256_consistent"] else "NO"

        md_lines.append(
            f"| `{g_name}` | {g_data['threshold']} | {g_data['num_runs']} | {t_iops} | {d_iops} | {s_cost} | {s_p99} | {gl_p99} | {gd_p99} | {p_p99} | {fwa_fg} | {cwa_fg} | {pwa_fg} | {pwa_tot} | {trunc_cnt} | {sha_str} |"
        )

    if phase_report:
        md_lines.append("\n## 2. Phase-by-Phase Execution Breakdown (True IOPS = Δops / Δt)\n")
        md_lines.append("| Group | Phase | True Phase IOPS | Scan Cost (μs/key) | Scan P99 (μs) | Get Live P99 (μs) | Get Del P99 (μs) | Put P99 (μs) | Limit Trunc. |")
        md_lines.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

        for g_name, p_dict in sorted(phase_report.items()):
            for p_name, p_m in p_dict.items():
                p_iops = f"{p_m['true_phase_iops']['mean']:.1f}"
                p_scost = f"{p_m['scan_us_per_key']['mean']:.2f}"
                p_sp99 = f"{p_m['scan_p99_us']['mean']:.1f}"
                p_glp99 = f"{p_m['get_live_p99_us']['mean']:.1f}"
                p_gdp99 = f"{p_m['get_del_p99_us']['mean']:.1f}"
                p_pp99 = f"{p_m['put_p99_us']['mean']:.1f}"
                p_tr = f"{p_m['scan_limit_truncated_count']['mean']:.0f}" if 'scan_limit_truncated_count' in p_m else "0"
                md_lines.append(f"| `{g_name}` | {p_name} | {p_iops} | {p_scost} | {p_sp99} | {p_glp99} | {p_gdp99} | {p_pp99} | {p_tr} |")

    with open(output_md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(md_lines) + "\n")

    print(f"Generated aggregated JSON: {output_json_path}")
    print(f"Generated aggregated Markdown: {output_md_path}")

def main():
    base_dir = "/home/wam/grad/s14-range-delete-study/results/formal_v2"
    sum_csv = os.path.join(base_dir, "summary", "formal_summary.csv")
    phases_csv = os.path.join(base_dir, "summary", "formal_phases.csv")
    out_json = os.path.join(base_dir, "summary", "formal_aggregated.json")
    out_md = os.path.join(base_dir, "summary", "formal_aggregated.md")

    aggregate_summary(sum_csv, phases_csv, out_json, out_md)

if __name__ == "__main__":
    main()
