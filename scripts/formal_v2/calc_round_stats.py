#!/usr/bin/env python3
import os
import pandas as pd
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
MATRIX_DIR = os.path.join(BASE_DIR, "results", "summary", "e9_dynamic_audit", "matrix")

phase_sum_file = os.path.join(MATRIX_DIR, "audit_phase_summary.csv")
run_sum_file = os.path.join(MATRIX_DIR, "audit_run_summary.csv")

df_phase = pd.read_csv(phase_sum_file)
df_run = pd.read_csv(run_sum_file)

def get_cfg(exp_id):
    if "t0" in exp_id: return "T0"
    if "t512" in exp_id: return "Native-T512"
    if "v2a" in exp_id: return "RTP-MC-V2-A"
    return "Unknown"

df_phase['config'] = df_phase['exp_id'].apply(get_cfg)
df_run['config'] = df_run['exp_id'].apply(get_cfg)

CONFIGS = ["T0", "Native-T512", "RTP-MC-V2-A"]
PHASES = [0, 1, 2]
PHASE_NAMES = {0: "Phase A (读敏感)", 1: "Phase B (写突发)", 2: "Phase C (读恢复)"}

def stats_str(vals):
    arr = np.array(vals, dtype=float)
    mean = np.mean(arr)
    std = np.std(arr, ddof=1) if len(arr) > 1 else 0.0
    med = np.median(arr)
    q75, q25 = np.percentile(arr, [75, 25])
    iqr = q75 - q25
    return f"{mean:.2f} ± {std:.2f}", f"{med:.2f}", f"{iqr:.2f}"

def format_metric_row(metric_name, unit, data_dict):
    # data_dict: {cfg: {p: [r1, r2, r3]}}
    pass

print("=========================================================================================")
print("                   逐轮统计明细与聚合表 (N=3: r1, r2, r3)")
print("=========================================================================================")

# Let us extract per-rep values
# For each config, phase:
results = {}
for cfg in CONFIGS:
    results[cfg] = {}
    for p in PHASES:
        results[cfg][p] = {
            "mat_rate": [],
            "lock_rate": [],
            "aff_rate": [],
            "mat_lock_ratio": [],
            "get_live_p99": [],
            "scan_intersect_p99": []
        }
        for rep in [1, 2, 3]:
            sub = df_phase[(df_phase['config'] == cfg) & (df_phase['rep'] == rep) & (df_phase['phase'] == p)]
            sub_total_reads = sub[sub['op_class'] == 'TOTAL_READS'].iloc[0]
            sub_gl = sub[sub['op_class'] == 'GetLive'].iloc[0]
            sub_si = sub[sub['op_class'] == 'ScanIntersect'].iloc[0]

            reads = sub_total_reads['op_count']
            mat_ops = sub_total_reads['materialized_ops']
            lock_ops = sub_total_reads['lock_contended_ops']
            aff_ops = sub_total_reads['materialization_or_lock_affected_reads']
            
            mat_rate = (mat_ops / reads) * 1000.0
            lock_rate = (lock_ops / reads) * 100.0
            aff_rate = (aff_ops / reads) * 100.0

            tot_lat_ms = sub_total_reads['total_latency_ms']
            mat_lat_ms = sub_total_reads['view_materialization_ms']
            lock_lat_ms = sub_total_reads['lock_wait_ms']
            mat_lock_ratio = ((mat_lat_ms + lock_lat_ms) / tot_lat_ms) * 100.0 if tot_lat_ms > 0 else 0.0

            gl_p99 = sub_gl['p99_us']
            si_p99 = sub_si['p99_us']

            results[cfg][p]["mat_rate"].append(mat_rate)
            results[cfg][p]["lock_rate"].append(lock_rate)
            results[cfg][p]["aff_rate"].append(aff_rate)
            results[cfg][p]["mat_lock_ratio"].append(mat_lock_ratio)
            results[cfg][p]["get_live_p99"].append(gl_p99)
            results[cfg][p]["scan_intersect_p99"].append(si_p99)

    # Full run
    results[cfg]["Full"] = {
        "mat_rate": [],
        "lock_rate": [],
        "aff_rate": [],
        "mat_lock_ratio": [],
        "get_live_p99": [],
        "scan_intersect_p99": []
    }
    for rep in [1, 2, 3]:
        sub_r = df_run[(df_run['config'] == cfg) & (df_run['rep'] == rep)].iloc[0]
        reads = sub_r['total_reads']
        mat_ops = sub_r['materialized_reads']
        lock_ops = sub_r['lock_contended_reads']
        aff_ops = sub_r['materialization_or_lock_affected_reads']
        mat_rate = (mat_ops / reads) * 1000.0
        lock_rate = (lock_ops / reads) * 100.0
        aff_rate = (aff_ops / reads) * 100.0

        tot_lat_ms = sub_r['overall_read_latency_ms']
        mat_lat_ms = sub_r['total_materialization_ms']
        lock_lat_ms = sub_r['total_lock_wait_ms']
        mat_lock_ratio = ((mat_lat_ms + lock_lat_ms) / tot_lat_ms) * 100.0

        # Weighted P99 or mean P99 for run? Usually reported by phase, but let us check
        sub_p_gl = df_phase[(df_phase['config'] == cfg) & (df_phase['rep'] == rep) & (df_phase['op_class'] == 'GetLive')]
        sub_p_si = df_phase[(df_phase['config'] == cfg) & (df_phase['rep'] == rep) & (df_phase['op_class'] == 'ScanIntersect')]
        gl_p99 = sub_p_gl['p99_us'].max()
        si_p99 = sub_p_si['p99_us'].max()

        results[cfg]["Full"]["mat_rate"].append(mat_rate)
        results[cfg]["Full"]["lock_rate"].append(lock_rate)
        results[cfg]["Full"]["aff_rate"].append(aff_rate)
        results[cfg]["Full"]["mat_lock_ratio"].append(mat_lock_ratio)
        results[cfg]["Full"]["get_live_p99"].append(gl_p99)
        results[cfg]["Full"]["scan_intersect_p99"].append(si_p99)

# Print Detailed Round Table
print("\n#### 逐轮原始明细表 (Rep 1, Rep 2, Rep 3)")
print("| Config | Phase | Metric | Rep 1 | Rep 2 | Rep 3 | Mean ± Std | Median | IQR |")
print("| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |")

metric_labels = [
    ("mat_rate", "每千读物化率 (/1k)"),
    ("lock_rate", "锁争用读比例 (%)"),
    ("aff_rate", "affected_union 比例 (%)"),
    ("mat_lock_ratio", "物化+锁等待耗时占比 (%)"),
    ("get_live_p99", "GetLive P99 (us)"),
    ("scan_intersect_p99", "ScanIntersect P99 (us)"),
]

for cfg in CONFIGS:
    for p in [0, 1, 2, "Full"]:
        p_name = PHASE_NAMES.get(p, "Full Run")
        for m_key, m_label in metric_labels:
            vals = results[cfg][p][m_key]
            m_std, med, iqr = stats_str(vals)
            print(f"| **{cfg}** | {p_name} | {m_label} | {vals[0]:.2f} | {vals[1]:.2f} | {vals[2]:.2f} | **{m_std}** | {med} | {iqr} |")
        print("| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |")

# Also print formatted compact table for notes
print("\n#### 综合汇总逐轮统计表 (Mean ± Sample Std, Median, IQR)")
print("| Config | Phase | 物化率 (/1k) [Mean±Std, Med, IQR] | 锁争用比例 (%) [Mean±Std, Med, IQR] | affected_union (%) [Mean±Std, Med, IQR] | 物化+锁等待占比 (%) [Mean±Std, Med, IQR] | GetLive P99 (us) [Mean±Std, Med, IQR] | ScanIntersect P99 (us) [Mean±Std, Med, IQR] |")
print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |")

for cfg in CONFIGS:
    for p in [0, 1, 2, "Full"]:
        p_name = PHASE_NAMES.get(p, "Full Run")
        row_cells = []
        for m_key, _ in metric_labels:
            vals = results[cfg][p][m_key]
            m_std, med, iqr = stats_str(vals)
            row_cells.append(f"{m_std} (Med: {med}, IQR: {iqr})")
        print(f"| **{cfg}** | {p_name} | " + " | ".join(row_cells) + " |")
    print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |")
