#!/usr/bin/env python3
"""
Analysis script for E9 Formal Mechanism Matrix
Calculates all required table metrics and distributions across T0, Native-T512, and RTP-MC-V2-A
"""

import os
import sys
import json
import pandas as pd
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
MATRIX_DIR = os.path.join(BASE_DIR, "results", "summary", "e9_dynamic_audit", "matrix")

PHASE_MAP = {0: "Phase A", 1: "Phase B", 2: "Phase C"}
CONFIGS = ["T0", "Native-T512", "RTP-MC-V2-A"]

def analyze():
    run_sum_path = os.path.join(MATRIX_DIR, "audit_run_summary.csv")
    phase_sum_path = os.path.join(MATRIX_DIR, "audit_phase_summary.csv")
    events_path = os.path.join(MATRIX_DIR, "audit_materialization_events.csv")
    phases_metric_path = os.path.join(MATRIX_DIR, "phases.csv")
    summary_metric_path = os.path.join(MATRIX_DIR, "summary.csv")

    if not os.path.exists(phase_sum_path) or not os.path.exists(events_path):
        print("[ERROR] Matrix CSV files not found!")
        return

    df_phase = pd.read_csv(phase_sum_path)
    df_run = pd.read_csv(run_sum_path)
    df_events = pd.read_csv(events_path)
    df_phases_metric = pd.read_csv(phases_metric_path) if os.path.exists(phases_metric_path) else None
    df_summary_metric = pd.read_csv(summary_metric_path) if os.path.exists(summary_metric_path) else None

    # Map exp_id to config
    def get_cfg(exp_id):
        if "t0" in exp_id: return "T0"
        if "t512" in exp_id: return "Native-T512"
        if "v2a" in exp_id: return "RTP-MC-V2-A"
        return "Unknown"

    df_phase['config'] = df_phase['exp_id'].apply(get_cfg)
    df_run['config'] = df_run['exp_id'].apply(get_cfg)
    df_events['config'] = df_events['run_id'].apply(get_cfg)
    if df_phases_metric is not None:
        df_phases_metric['config'] = df_phases_metric['exp_id'].apply(get_cfg)

    print("=========================================================================================")
    print("                      E9 FORMAL MECHANISM MATRIX DETAILED ANALYSIS                       ")
    print("=========================================================================================")

    # 1. Main Table: Per-Config and Per-Phase metrics
    print("\n### 1. Mechanism Metrics Summary Table (Mean ± Std over N=3)")
    print("| Config | Phase | Total Reads | Mat Rate (/1k) | Lock Contended Rate | Affected Union Rate | Both Count | Mat Lat Ratio | Lock Lat Ratio | Mat+Lock Lat Ratio |")
    print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

    for cfg in CONFIGS:
        cfg_df = df_phase[df_phase['config'] == cfg]
        for p in [0, 1, 2]:
            sub = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'TOTAL_READS')]
            if sub.empty: continue
            
            reads_mean = sub['op_count'].mean()
            mat_ops_mean = sub['materialized_ops'].mean()
            mat_rate_mean = (mat_ops_mean / reads_mean) * 1000.0
            
            lock_ops_mean = sub['lock_contended_ops'].mean()
            lock_rate_mean = (lock_ops_mean / reads_mean) * 100.0
            
            aff_ops_mean = sub['materialization_or_lock_affected_reads'].mean()
            aff_rate_mean = (aff_ops_mean / reads_mean) * 100.0
            
            both_ops_mean = sub['both_count'].mean()
            
            tot_lat_ms = sub['total_latency_ms'].mean()
            mat_lat_ms = sub['view_materialization_ms'].mean()
            lock_lat_ms = sub['lock_wait_ms'].mean()
            
            mat_lat_ratio = (mat_lat_ms / tot_lat_ms) * 100.0 if tot_lat_ms > 0 else 0.0
            lock_lat_ratio = (lock_lat_ms / tot_lat_ms) * 100.0 if tot_lat_ms > 0 else 0.0
            comb_lat_ratio = ((mat_lat_ms + lock_lat_ms) / tot_lat_ms) * 100.0 if tot_lat_ms > 0 else 0.0

            print(f"| **{cfg}** | {PHASE_MAP[p]} | {int(reads_mean):,} | {mat_rate_mean:.2f} | {lock_rate_mean:.2f}% | {aff_rate_mean:.2f}% | {both_ops_mean:.1f} | {mat_lat_ratio:.2f}% | {lock_lat_ratio:.2f}% | **{comb_lat_ratio:.2f}%** |")
        
        # Overall Run
        sub_run = df_run[df_run['config'] == cfg]
        if not sub_run.empty:
            r_reads = sub_run['total_reads'].mean()
            r_mat = sub_run['materialized_reads'].mean()
            r_mat_rate = (r_mat / r_reads) * 1000.0
            r_lock = sub_run['lock_contended_reads'].mean()
            r_lock_rate = (r_lock / r_reads) * 100.0
            r_aff = sub_run['materialization_or_lock_affected_reads'].mean()
            r_aff_rate = (r_aff / r_reads) * 100.0
            r_both = sub_run['both_reads'].mean()
            r_tot_lat = sub_run['overall_read_latency_ms'].mean()
            r_mat_lat = sub_run['total_materialization_ms'].mean()
            r_lock_lat = sub_run['total_lock_wait_ms'].mean()
            r_mat_ratio = (r_mat_lat / r_tot_lat) * 100.0 if r_tot_lat > 0 else 0.0
            r_lock_ratio = (r_lock_lat / r_tot_lat) * 100.0 if r_tot_lat > 0 else 0.0
            r_comb_ratio = ((r_mat_lat + r_lock_lat) / r_tot_lat) * 100.0 if r_tot_lat > 0 else 0.0
            print(f"| **{cfg}** | **Full Run** | {int(r_reads):,} | **{r_mat_rate:.2f}** | **{r_lock_rate:.2f}%** | **{r_aff_rate:.2f}%** | **{r_both:.1f}** | {r_mat_ratio:.2f}% | {r_lock_ratio:.2f}% | **{r_comb_ratio:.2f}%** |")
        print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

    # 2. Latency P99 by Operation Class
    print("\n### 2. Operation P99 Latency (us) across Phases")
    print("| Config | Phase | GetLive P99 | GetDeleted P99 | ScanIntersect P99 | ScanNonIntersect P99 | Put P99 |")
    print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: |")

    for cfg in CONFIGS:
        cfg_df = df_phase[df_phase['config'] == cfg]
        for p in [0, 1, 2]:
            gl = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'GetLive')]['p99_us'].mean()
            gd = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'GetDeleted')]['p99_us'].mean()
            si = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'ScanIntersect')]['p99_us'].mean()
            sn = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'ScanNonIntersect')]['p99_us'].mean()
            pt = cfg_df[(cfg_df['phase'] == p) & (cfg_df['op_class'] == 'Put')]['p99_us'].mean()
            print(f"| **{cfg}** | {PHASE_MAP[p]} | {gl:.2f} | {gd:.2f} | {si:.2f} | {sn:.2f} | {pt:.2f} |")
        print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: |")

    # 3. Active MemTable Tombstone Distribution on Actual Materialization
    print("\n### 3. Active MemTable Tombstone Count Distribution on Materialization Events")
    print("| Config | Phase | Mat Event Count | Min Tombstones | P25 | Median | P75 | P90 | P99 | Max Tombstones | Mean ± Std |")
    print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

    mat_only = df_events[df_events['materialized'] == 1]
    for cfg in CONFIGS:
        cfg_events = mat_only[mat_only['config'] == cfg]
        for p in [0, 1, 2]:
            sub_ev = cfg_events[cfg_events['phase'] == p]
            cnt = len(sub_ev)
            if cnt == 0:
                print(f"| **{cfg}** | {PHASE_MAP[p]} | 0 | - | - | - | - | - | - | - | - |")
                continue
            ts = sub_ev['active_mem_tombstones'].values
            print(f"| **{cfg}** | {PHASE_MAP[p]} | {cnt} | {np.min(ts)} | {np.percentile(ts, 25):.0f} | {np.median(ts):.0f} | {np.percentile(ts, 75):.0f} | {np.percentile(ts, 90):.0f} | {np.percentile(ts, 99):.0f} | {np.max(ts)} | {np.mean(ts):.1f} ± {np.std(ts):.1f} |")
        
        # All phases for config
        all_cnt = len(cfg_events)
        if all_cnt > 0:
            ts = cfg_events['active_mem_tombstones'].values
            print(f"| **{cfg}** | **All Phases** | {all_cnt} | {np.min(ts)} | {np.percentile(ts, 25):.0f} | {np.median(ts):.0f} | {np.percentile(ts, 75):.0f} | {np.percentile(ts, 90):.0f} | {np.percentile(ts, 99):.0f} | {np.max(ts)} | **{np.mean(ts):.1f} ± {np.std(ts):.1f}** |")
        print("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

    # 4. Engine Health: Flushes, L0 Files, Pending Compaction Bytes
    if df_phases_metric is not None:
        print("\n### 4. Engine Health & Compaction Indicators (phases.csv)")
        print("| Config | Phase | Elapsed (s) | True IOPS | Num L0 Files | Pending Compaction (MB) |")
        print("| :--- | :--- | :---: | :---: | :---: | :---: |")
        for cfg in CONFIGS:
            cfg_pm = df_phases_metric[df_phases_metric['config'] == cfg]
            for p_str, p_name in [("Phase A", "Phase A"), ("Phase B (Write Burst)", "Phase B"), ("Phase C (Read Recovery)", "Phase C")]:
                sub = cfg_pm[cfg_pm['phase'].str.startswith(p_str)]
                if sub.empty: continue
                el = sub['elapsed_sec'].mean()
                iops = sub['true_phase_iops'].mean()
                l0 = sub['num_l0_files'].mean()
                pend_mb = sub['pending_compaction_bytes'].mean() / (1024.0 * 1024.0)
                print(f"| **{cfg}** | {p_name} | {el:.4f} | {iops:.1f} | {l0:.1f} | {pend_mb:.2f} MB |")
            print("| :--- | :--- | :---: | :---: | :---: | :---: |")

    # 5. Flush Counts from summary.csv
    if df_summary_metric is not None:
        print("\n### 5. Flush Summary (summary.csv)")
        print("| Config | Rep | Total Flush Count | FG Wallclock (s) | Foreground IOPS | DB SHA-256 | Verification |")
        print("| :--- | :---: | :---: | :---: | :---: | :---: | :---: |")
        for idx, row in df_summary_metric.iterrows():
            print(f"| **{row['group_name']}** | rep {row['exp_id'].split('_r')[-1]} | {row['total_exp_flush_count']} | {row['foreground_wallclock_sec']:.4f} s | {row['fg_db_api_iops']:.1f} | `{row['sha256_hex'][:16]}...` | {row['verification_status']} |")

if __name__ == "__main__":
    analyze()
