#!/usr/bin/env python3
"""
Plot Generator for RocksDB Range Deletion Study (P1 ~ P5)
Generates high-resolution publication charts in results/plots/
"""

import os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots")
os.makedirs(PLOTS_DIR, exist_ok=True)

plt.style.use('seaborn-v0_8-whitegrid' if 'seaborn-v0_8-whitegrid' in plt.style.available else 'default')
plt.rcParams['font.sans-serif'] = ['DejaVu Sans', 'Arial', 'Liberation Sans']
plt.rcParams['axes.edgecolor'] = '#cccccc'
plt.rcParams['axes.linewidth'] = 0.8

# ----------------------------------------------------------------------
# Plot 1: P1 Ratio Sensitivity
# ----------------------------------------------------------------------
def plot_p1():
    csv_path = os.path.join(SUMMARY_DIR, "p1_summary.csv")
    if not os.path.exists(csv_path): return
    df = pd.read_csv(csv_path)
    
    # Map ratios
    ratio_map = {
        'p1_range_del_ratio_0pct': 0.0,
        'p1_range_del_ratio_0_5pct': 0.5,
        'p1_range_del_ratio_1pct': 1.0,
        'p1_range_del_ratio_2pct': 2.0,
        'p1_range_del_ratio_5pct': 5.0,
        'p1_range_del_ratio_10pct': 10.0,
    }
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    df['ratio'] = df['config_base'].map(ratio_map)
    grouped = df.groupby('ratio').mean(numeric_only=True).reset_index()

    fig, ax1 = plt.subplots(figsize=(8, 5), dpi=300)
    color = '#1f77b4'
    ax1.set_xlabel('DeleteRange Operation Ratio (%)', fontsize=12, fontweight='bold')
    ax1.set_ylabel('Overall IOPS (ops/sec)', color=color, fontsize=12, fontweight='bold')
    ax1.plot(grouped['ratio'], grouped['overall_iops'], marker='o', linewidth=2.2, markersize=8, color=color, label='Overall IOPS')
    ax1.tick_params(axis='y', labelcolor=color)
    ax1.set_yscale('log')
    ax1.grid(True, linestyle='--', alpha=0.6)

    ax2 = ax1.twinx()
    color2 = '#d62728'
    ax2.set_ylabel('RangeScan Cost per Key (μs / key)', color=color2, fontsize=12, fontweight='bold')
    ax2.plot(grouped['ratio'], grouped['scan_us_per_key'], marker='s', linewidth=2.2, markersize=8, color=color2, linestyle='--', label='Scan Cost/Key')
    ax2.tick_params(axis='y', labelcolor=color2)
    ax2.set_yscale('log')
    ax2.grid(False)

    plt.title('P1: Impact of Range Deletion Ratio on Throughput & Scan Cost', fontsize=14, fontweight='bold', pad=15)
    fig.tight_layout()
    out_file = os.path.join(PLOTS_DIR, "p1_range_del_ratio_impact.png")
    plt.savefig(out_file, bbox_inches='tight')
    plt.close()
    print(f"[Plot] Saved P1 plot to: {out_file}")

# ----------------------------------------------------------------------
# Plot 2: P2 Tombstone Organization Patterns
# ----------------------------------------------------------------------
def plot_p2():
    csv_path = os.path.join(SUMMARY_DIR, "p2_summary.csv")
    if not os.path.exists(csv_path): return
    df = pd.read_csv(csv_path)
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('config_base').mean(numeric_only=True).reset_index()

    labels = ['Short\nFragmented', 'Short\nOverlapped', 'Long\nContiguous', 'Long\nOverlapped']
    key_order = ['p2_tombstone_short_fragmented', 'p2_tombstone_short_overlapped', 'p2_tombstone_long_contiguous', 'p2_tombstone_long_overlapped']
    grouped = grouped.set_index('config_base').reindex(key_order).reset_index()

    fig, ax = plt.subplots(figsize=(8, 5), dpi=300)
    x = np.arange(len(labels))
    width = 0.35

    rects1 = ax.bar(x - width/2, grouped['overall_iops'], width, label='Overall IOPS', color='#2ca02c', alpha=0.85)
    ax2 = ax.twinx()
    rects2 = ax2.bar(x + width/2, grouped['scan_us_per_key'], width, label='Scan Cost (μs/key)', color='#ff7f0e', alpha=0.85)

    ax.set_ylabel('Overall IOPS (ops/s)', color='#2ca02c', fontsize=12, fontweight='bold')
    ax2.set_ylabel('Scan Cost per Returned Key (μs/key)', color='#ff7f0e', fontsize=12, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11, fontweight='bold')
    ax.grid(True, linestyle='--', alpha=0.5)
    ax2.grid(False)

    plt.title('P2: Influence of Tombstone Spatial Organization Patterns', fontsize=14, fontweight='bold', pad=15)
    fig.tight_layout()
    out_file = os.path.join(PLOTS_DIR, "p2_tombstone_patterns.png")
    plt.savefig(out_file, bbox_inches='tight')
    plt.close()
    print(f"[Plot] Saved P2 plot to: {out_file}")

# ----------------------------------------------------------------------
# Plot 3: P3 Read Composition
# ----------------------------------------------------------------------
def plot_p3():
    csv_path = os.path.join(SUMMARY_DIR, "p3_summary.csv")
    if not os.path.exists(csv_path): return
    df = pd.read_csv(csv_path)
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('config_base').mean(numeric_only=True).reset_index()

    fig, ax = plt.subplots(figsize=(7, 5), dpi=300)
    labels = ['Point-Heavy\n(Get 75%, Scan 5%)', 'Scan-Heavy\n(Get 10%, Scan 70%)']
    key_order = ['p3_point_heavy', 'p3_scan_heavy']
    grouped = grouped.set_index('config_base').reindex(key_order).reset_index()

    bars = ax.bar(labels, grouped['scan_us_per_key'], color=['#3b528b', '#5ec962'], width=0.45, edgecolor='#333333')
    ax.set_ylabel('Scan Normalized Latency (μs / key)', fontsize=12, fontweight='bold')
    ax.set_title('P3: Read Composition Impact on Scan Efficiency (at 10% DelRange)', fontsize=13, fontweight='bold', pad=15)
    ax.grid(True, linestyle='--', alpha=0.6)

    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:.1f} μs/key',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 5), textcoords="offset points",
                    ha='center', va='bottom', fontsize=11, fontweight='bold')

    fig.tight_layout()
    out_file = os.path.join(PLOTS_DIR, "p3_read_composition.png")
    plt.savefig(out_file, bbox_inches='tight')
    plt.close()
    print(f"[Plot] Saved P3 plot to: {out_file}")

# ----------------------------------------------------------------------
# Plot 4: P4 Deletion Locality
# ----------------------------------------------------------------------
def plot_p4():
    csv_path = os.path.join(SUMMARY_DIR, "p4_summary.csv")
    if not os.path.exists(csv_path): return
    df = pd.read_csv(csv_path)
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('config_base').mean(numeric_only=True).reset_index()

    labels = ['Cold Zone\n(Overlap 0.0)', 'Medium\n(Overlap 0.5)', 'Hot Zone\n(Overlap 1.0)']
    key_order = ['p4_locality_cold', 'p4_locality_medium', 'p4_locality_hot']
    grouped = grouped.set_index('config_base').reindex(key_order).reset_index()

    fig, ax = plt.subplots(figsize=(8, 5), dpi=300)
    x = np.arange(len(labels))
    width = 0.35

    ax.bar(x - width/2, grouped['overall_iops'], width, label='Throughput (IOPS)', color='#440154', alpha=0.85)
    ax2 = ax.twinx()
    ax2.bar(x + width/2, grouped['scan_us_per_key'], width, label='Scan Cost (μs/key)', color='#fde725', alpha=0.85, edgecolor='#666666')

    ax.set_ylabel('Overall IOPS', color='#440154', fontsize=12, fontweight='bold')
    ax2.set_ylabel('Scan Cost (μs / key)', color='#998200', fontsize=12, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11, fontweight='bold')
    ax.grid(True, linestyle='--', alpha=0.5)
    ax2.grid(False)

    plt.title('P4: Deletion Interval Hotspot Locality (Zipf θ=0.99)', fontsize=14, fontweight='bold', pad=15)
    fig.tight_layout()
    out_file = os.path.join(PLOTS_DIR, "p4_deletion_locality.png")
    plt.savefig(out_file, bbox_inches='tight')
    plt.close()
    print(f"[Plot] Saved P4 plot to: {out_file}")

# ----------------------------------------------------------------------
# Plot 5: P5 Compaction Reclaim
# ----------------------------------------------------------------------
def plot_p5():
    csv_path = os.path.join(SUMMARY_DIR, "p5_summary.csv")
    if not os.path.exists(csv_path): return
    df = pd.read_csv(csv_path)
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('config_base').mean(numeric_only=True).reset_index()

    labels = ['Control\n(No CompactRange)', 'High-Heat Zone\nCompaction', 'Low-Heat Zone\nCompaction']
    key_order = ['p5_compaction_control_none', 'p5_compaction_high_heat', 'p5_compaction_low_heat']
    grouped = grouped.set_index('config_base').reindex(key_order).reset_index()

    fig, ax = plt.subplots(figsize=(8, 5), dpi=300)
    bars = ax.bar(labels, grouped['overall_iops'], color=['#7f7f7f', '#e377c2', '#17becf'], width=0.45, edgecolor='#333333')
    ax.set_ylabel('Overall Benchmark IOPS', fontsize=12, fontweight='bold')
    ax.set_title('P5: Effect of Controlled Tombstone Compaction on Performance', fontsize=13, fontweight='bold', pad=15)
    ax.grid(True, linestyle='--', alpha=0.6)

    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:.0f} ops/s',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 5), textcoords="offset points",
                    ha='center', va='bottom', fontsize=11, fontweight='bold')

    fig.tight_layout()
    out_file = os.path.join(PLOTS_DIR, "p5_compaction_reclaim.png")
    plt.savefig(out_file, bbox_inches='tight')
    plt.close()
    print(f"[Plot] Saved P5 plot to: {out_file}")

def main():
    print("=== Generating Analysis Plots ===")
    plot_p1()
    plot_p2()
    plot_p3()
    plot_p4()
    plot_p5()
    print("=== All Plots Generated Successfully ===")

if __name__ == "__main__":
    main()
