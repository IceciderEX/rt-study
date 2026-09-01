#!/usr/bin/env python3
"""
Generates visualization plots for S1, S2, S3 supplement experiments.
"""
import os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary", "supplement")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "supplement")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "all-runs.csv")
TIMESERIES_CSV = os.path.join(SUMMARY_DIR, "timeseries.csv")

def plot_s1(df):
    s1_df = df[df['exp_id'].str.startswith('s1_')].copy()
    s1_df['group'] = s1_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = s1_df.groupby('group').mean(numeric_only=True)

    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    fig.patch.set_facecolor('#0f172a')
    for ax in axes:
        ax.set_facecolor('#1e293b')
        ax.tick_params(colors='#e2e8f0', labelsize=10)
        ax.xaxis.label.set_color('#e2e8f0')
        ax.yaxis.label.set_color('#e2e8f0')
        ax.title.set_color('#38bdf8')
        for spine in ax.spines.values():
            spine.set_color('#334155')

    labels = ['Clean\n(Point)', 'Tombstone\n(Point)', 'Clean\n(Scan)', 'Tombstone\n(Scan)']
    keys = ['s1_clean_point_heavy', 's1_tombstone_point_heavy', 's1_clean_scan_heavy', 's1_tombstone_scan_heavy']
    colors = ['#38bdf8', '#f43f5e', '#38bdf8', '#f43f5e']

    # 1. Scan us_per_key
    vals_scan = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in keys]
    axes[0].bar(labels, vals_scan, color=colors, width=0.55, edgecolor='#64748b')
    axes[0].set_title('RangeScan Normalized Cost (μs/key)', fontsize=12, fontweight='bold')
    axes[0].set_ylabel('μs per Returned Key')
    for i, v in enumerate(vals_scan):
        axes[0].text(i, v + 0.03, f"{v:.2f}", ha='center', color='#f8fafc', fontweight='bold', fontsize=10)

    # 2. Get Control P99
    vals_get = [grouped.loc[k, 'get_ctrl_p99_us'] if k in grouped.index else 0 for k in keys]
    axes[1].bar(labels, vals_get, color=colors, width=0.55, edgecolor='#64748b')
    axes[1].set_title('Get (Control) P99 Latency (μs)', fontsize=12, fontweight='bold')
    axes[1].set_ylabel('Latency (μs)')
    for i, v in enumerate(vals_get):
        axes[1].text(i, v + 0.3, f"{v:.1f}", ha='center', color='#f8fafc', fontweight='bold', fontsize=10)

    # 3. Block Cache Misses
    vals_miss = [grouped.loc[k, 'block_cache_misses'] / 1e6 if k in grouped.index else 0 for k in keys]
    axes[2].bar(labels, vals_miss, color=colors, width=0.55, edgecolor='#64748b')
    axes[2].set_title('Block Cache Misses (Millions)', fontsize=12, fontweight='bold')
    axes[2].set_ylabel('Misses (x10^6)')
    for i, v in enumerate(vals_miss):
        axes[2].text(i, v + 0.05, f"{v:.2f}M", ha='center', color='#f8fafc', fontweight='bold', fontsize=10)

    plt.tight_layout()
    out_file = os.path.join(PLOTS_DIR, 's1_static_tombstone_cost.png')
    plt.savefig(out_file, dpi=200, facecolor=fig.get_facecolor())
    plt.close()
    print(f"Saved S1 plot to {out_file}")

def plot_s2(df):
    s2_df = df[df['exp_id'].str.startswith('s2_')].copy()
    s2_df['group'] = s2_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = s2_df.groupby('group').mean(numeric_only=True)

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.patch.set_facecolor('#0f172a')
    for ax in axes:
        ax.set_facecolor('#1e293b')
        ax.tick_params(colors='#e2e8f0', labelsize=10)
        ax.xaxis.label.set_color('#e2e8f0')
        ax.yaxis.label.set_color('#e2e8f0')
        ax.title.set_color('#a855f7')
        for spine in ax.spines.values():
            spine.set_color('#334155')

    labels = ['seg-20\n(20k len)', 'seg-200\n(2k len)', 'seg-2000\n(200 len)']
    pt_keys = ['s2_seg20_point_heavy', 's2_seg200_point_heavy', 's2_seg2000_point_heavy']
    sc_keys = ['s2_seg20_scan_heavy', 's2_seg200_scan_heavy', 's2_seg2000_scan_heavy']

    x = np.arange(len(labels))
    width = 0.35

    # 1. Scan Cost across fragmentation
    pt_scan = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in pt_keys]
    sc_scan = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in sc_keys]
    axes[0].bar(x - width/2, pt_scan, width, label='Point-Heavy', color='#38bdf8', edgecolor='#64748b')
    axes[0].bar(x + width/2, sc_scan, width, label='Scan-Heavy', color='#ec4899', edgecolor='#64748b')
    axes[0].set_title('Scan Cost vs Tombstone Granularity (μs/key)', fontsize=12, fontweight='bold')
    axes[0].set_xticks(x)
    axes[0].set_xticklabels(labels)
    axes[0].set_ylabel('μs per Returned Key')
    axes[0].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc')

    # 2. Get Affected P99
    pt_get = [grouped.loc[k, 'get_aff_p99_us'] if k in grouped.index else 0 for k in pt_keys]
    sc_get = [grouped.loc[k, 'get_aff_p99_us'] if k in grouped.index else 0 for k in sc_keys]
    axes[1].bar(x - width/2, pt_get, width, label='Point-Heavy', color='#38bdf8', edgecolor='#64748b')
    axes[1].bar(x + width/2, sc_get, width, label='Scan-Heavy', color='#ec4899', edgecolor='#64748b')
    axes[1].set_title('Get (Affected Zone) P99 Latency (μs)', fontsize=12, fontweight='bold')
    axes[1].set_xticks(x)
    axes[1].set_xticklabels(labels)
    axes[1].set_ylabel('Latency (μs)')
    axes[1].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc')

    plt.tight_layout()
    out_file = os.path.join(PLOTS_DIR, 's2_fragmentation_comparison.png')
    plt.savefig(out_file, dpi=200, facecolor=fig.get_facecolor())
    plt.close()
    print(f"Saved S2 plot to {out_file}")

def plot_s3():
    if not os.path.exists(TIMESERIES_CSV):
        return
    ts_df = pd.read_csv(TIMESERIES_CSV)
    if ts_df.empty:
        return

    ts_df['config_base'] = ts_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = ts_df.groupby(['config_base', 'second_idx']).mean(numeric_only=True).reset_index()

    fig, axes = plt.subplots(2, 2, figsize=(16, 10))
    fig.patch.set_facecolor('#0f172a')

    colors = {'s3_control': '#38bdf8', 's3_hot_reclaim': '#22c55e', 's3_cold_reclaim': '#f59e0b'}
    labels = {'s3_control': 'S3 Control (No Compact)', 's3_hot_reclaim': 'S3 Hot Reclaim (t=20s)', 's3_cold_reclaim': 'S3 Cold Reclaim (t=20s)'}

    for ax in axes.flat:
        ax.set_facecolor('#1e293b')
        ax.tick_params(colors='#e2e8f0', labelsize=10)
        ax.xaxis.label.set_color('#e2e8f0')
        ax.yaxis.label.set_color('#e2e8f0')
        for spine in ax.spines.values():
            spine.set_color('#334155')

    for cfg in ['s3_control', 's3_hot_reclaim', 's3_cold_reclaim']:
        sub = grouped[grouped['config_base'] == cfg].sort_values('second_idx')
        if sub.empty: continue
        c = colors.get(cfg, '#ffffff')
        lbl = labels.get(cfg, cfg)

        # 1. Total IOPS (Get+Scan+Put)
        total_ops = sub['get_ctrl_ops'] + sub['get_aff_ops'] + sub['scan_ops'] + sub['put_ops']
        axes[0, 0].plot(sub['second_idx'], total_ops, label=lbl, color=c, lw=2)

        # 2. Scan P99
        axes[0, 1].plot(sub['second_idx'], sub['scan_p99_us'], label=lbl, color=c, lw=2)

        # 3. Compaction Write MB Delta
        axes[1, 0].plot(sub['second_idx'], sub['compaction_write_mb_delta'], label=lbl, color=c, lw=2)

        # 4. Get Affected P99
        axes[1, 1].plot(sub['second_idx'], sub['get_aff_p99_us'], label=lbl, color=c, lw=2)

    axes[0, 0].set_title('Frontend Throughput Time Series (IOPS)', color='#38bdf8', fontweight='bold')
    axes[0, 0].set_xlabel('Time (Seconds)')
    axes[0, 0].set_ylabel('IOPS')
    axes[0, 0].axvline(x=20, color='#f43f5e', linestyle='--', alpha=0.7, label='CompactRange Trigger (t=20s)')
    axes[0, 0].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc', fontsize=9)

    axes[0, 1].set_title('RangeScan P99 Latency (μs)', color='#38bdf8', fontweight='bold')
    axes[0, 1].set_xlabel('Time (Seconds)')
    axes[0, 1].set_ylabel('P99 Latency (μs)')
    axes[0, 1].axvline(x=20, color='#f43f5e', linestyle='--', alpha=0.7)
    axes[0, 1].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc', fontsize=9)

    axes[1, 0].set_title('Compaction Write Bandwidth Delta (MB/s)', color='#38bdf8', fontweight='bold')
    axes[1, 0].set_xlabel('Time (Seconds)')
    axes[1, 0].set_ylabel('MB/s Delta')
    axes[1, 0].axvline(x=20, color='#f43f5e', linestyle='--', alpha=0.7)
    axes[1, 0].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc', fontsize=9)

    axes[1, 1].set_title('Get (Affected Zone) P99 Latency (μs)', color='#38bdf8', fontweight='bold')
    axes[1, 1].set_xlabel('Time (Seconds)')
    axes[1, 1].set_ylabel('P99 Latency (μs)')
    axes[1, 1].axvline(x=20, color='#f43f5e', linestyle='--', alpha=0.7)
    axes[1, 1].legend(facecolor='#1e293b', edgecolor='#64748b', labelcolor='#f8fafc', fontsize=9)

    plt.tight_layout()
    out_file = os.path.join(PLOTS_DIR, 's3_timeseries_comparison.png')
    plt.savefig(out_file, dpi=200, facecolor=fig.get_facecolor())
    plt.close()
    print(f"Saved S3 plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        plot_s1(df)
        plot_s2(df)
    plot_s3()
