#!/usr/bin/env python3
"""
FormalV2-LongCycle-20GiB: Academic Figure Generation Script
Generates 5 publication-grade figures (PDF + PNG 300 DPI):
  1. fig_lc20_throughput_timeline.pdf/png: 4-phase lifetime throughput with write stall overlays
  2. fig_lc20_latency_breakdown.pdf/png: Point & scan operation fine-grained latencies across phases
  3. fig_lc20_memtable_flush_dynamics.pdf/png: Memtable tombstone accumulation & flush reason breakdown
  4. fig_lc20_multilevel_migration.pdf/png: L0~L6 SST tombstone migration & compaction drop dynamics
  5. fig_lc20_cooldown_convergence.pdf/png: Post-workload cooldown debt convergence & space recovery
"""

import os
import glob
import json
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.patches import Rectangle

# High aesthetic paper style
plt.style.use("seaborn-v0_8-whitegrid")
plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.labelsize": 13,
    "axes.titlesize": 14,
    "xtick.labelsize": 11,
    "ytick.labelsize": 11,
    "legend.fontsize": 11,
    "figure.titlesize": 16,
    "figure.dpi": 300,
    "lines.linewidth": 1.8,
    "axes.edgecolor": "#333333",
    "axes.linewidth": 0.9,
})

RESULTS_DIR = "/home/wam/grad/s14-range-delete-study/results/formal_v2/longcycle_20g"
OUTPUT_DIR = "/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g"

PALETTE = {
    "CLEAN": "#2b5c8f",       # Slate blue (Background reference)
    "T0-NATIVE": "#d95f02",   # Vivid orange (RocksDB default baseline)
    "T64-STATIC": "#7570b3",  # Deep purple (Active threshold)
    "T512-STATIC": "#1b9e77", # Forest green
    "PHASE_A": "#f7f7f7",
    "PHASE_B": "#ededed",
    "PHASE_C": "#fde0dd",     # Light red accent for high pressure
    "PHASE_D": "#e0f3f8",
}

def load_matrix_data():
    runs = sorted(glob.glob(os.path.join(RESULTS_DIR, "LC20-R*")))
    run_dfs = {}
    summaries = {}
    snapshots = {}
    events = {}

    for r_dir in runs:
        r_name = os.path.basename(r_dir)
        summary_path = os.path.join(r_dir, "summary.json")
        ts_path = os.path.join(r_dir, "timeseries.csv")
        snap_path = os.path.join(r_dir, "level_tombstone_snapshots.csv")
        ev_path = os.path.join(r_dir, "sst_tombstone_events.csv")

        if os.path.exists(summary_path) and os.path.exists(ts_path):
            try:
                with open(summary_path) as f:
                    summaries[r_name] = json.load(f)
                df = pd.read_csv(ts_path)
                run_dfs[r_name] = df
                if os.path.exists(snap_path) and os.path.getsize(snap_path) > 0:
                    snapshots[r_name] = pd.read_csv(snap_path)
                if os.path.exists(ev_path) and os.path.getsize(ev_path) > 0:
                    events[r_name] = pd.read_csv(ev_path)
            except Exception as e:
                print(f"  [WARN] Skipping incomplete run {r_name}: {e}")

    return run_dfs, summaries, snapshots, events

def plot_fig1_throughput(run_dfs, summaries):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True, gridspec_kw={'height_ratios': [3, 1]})

    # Group runs by condition
    conditions = {"CLEAN": [], "T0-NATIVE": [], "T64-STATIC": []}
    for r_name, s in summaries.items():
        if s.get("is_clean"):
            conditions["CLEAN"].append(r_name)
        elif s.get("threshold") == 0:
            conditions["T0-NATIVE"].append(r_name)
        else:
            conditions["T64-STATIC"].append(r_name)

    # Plot IOPS curves (smooth rolling average 15s)
    for cond, r_list in conditions.items():
        if not r_list: continue
        # Average across reps
        dfs = [run_dfs[r] for r in r_list if r in run_dfs]
        if not dfs: continue
        min_len = min(len(df) for df in dfs)
        time_axis = dfs[0]["elapsed_sec"].iloc[:min_len]
        iops_mat = np.array([df["interval_db_api_iops"].iloc[:min_len].values for df in dfs])
        mean_iops = np.mean(iops_mat, axis=0)
        std_iops = np.std(iops_mat, axis=0)

        # Smooth
        smooth_iops = pd.Series(mean_iops).rolling(3, min_periods=1).mean()
        ax1.plot(time_axis, smooth_iops / 1e3, label=f"{cond} (N={len(r_list)})", color=PALETTE.get(cond, "#333"), alpha=0.95)
        ax1.fill_between(time_axis, (smooth_iops - std_iops) / 1e3, (smooth_iops + std_iops) / 1e3, color=PALETTE.get(cond, "#333"), alpha=0.15)

    # Background Phase Banners
    ax1.set_ylabel("Foreground Throughput (kIOPS)")
    ax1.set_title("(a) Dynamic Multi-Phase Lifetime Throughput & Compaction Stalls", fontweight="bold")
    ax1.legend(loc="upper right", frameon=True)
    ax1.grid(True, linestyle="--", alpha=0.5)

    # Stalls and MemTable size
    for cond, r_list in conditions.items():
        if not r_list: continue
        dfs = [run_dfs[r] for r in r_list if r in run_dfs]
        if not dfs: continue
        min_len = min(len(df) for df in dfs)
        time_axis = dfs[0]["elapsed_sec"].iloc[:min_len]
        l0_mat = np.array([df["l0_files"].iloc[:min_len].values for df in dfs])
        mean_l0 = np.mean(l0_mat, axis=0)
        ax2.plot(time_axis, mean_l0, label=f"L0 Files ({cond})", color=PALETTE.get(cond, "#333"), linestyle=":")

    ax2.set_xlabel("Elapsed Foreground Time (seconds)")
    ax2.set_ylabel("L0 Files Count")
    ax2.set_title("(b) Level-0 File Accumulation Dynamic", fontweight="bold", fontsize=12)
    ax2.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_throughput_timeline.pdf"))
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_throughput_timeline.png"))
    plt.close()
    print("  [PLOTS] fig_lc20_throughput_timeline generated.")

def plot_fig2_latency(run_dfs, summaries):
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))

    # Point read latency P99 & Scan latency P99
    ax1, ax2 = axes[0], axes[1]

    conditions = ["CLEAN", "T0-NATIVE", "T64-STATIC"]
    get_labels = ["Get-Deleted", "Get-Adjacent", "Get-Control"]
    scan_labels = ["Scan-Intersect", "Scan-NonIntersect"]

    # Extract Phase C P99 latencies
    data_get = {c: [] for c in conditions}
    data_scan = {c: [] for c in conditions}

    for r_name, s in summaries.items():
        cond = "CLEAN" if s.get("is_clean") else ("T0-NATIVE" if s.get("threshold") == 0 else "T64-STATIC")
        if r_name not in run_dfs: continue
        df = run_dfs[r_name]
        # Filter Phase C (phase_id == 2)
        df_c = df[df["phase_id"] == 2]
        if not df_c.empty:
            data_get[cond].append({
                "get_del": df_c["get_del_p99"].mean(),
                "get_adj": df_c["get_adj_p99"].mean(),
                "get_ctl": df_c["get_ctl_p99"].mean(),
            })
            data_scan[cond].append({
                "scan_inter": df_c["scan_inter_p99"].mean(),
                "scan_non": df_c["scan_non_p99"].mean(),
            })

    # Bar chart for Get P99
    x = np.arange(len(conditions))
    width = 0.25

    for i, g_key in enumerate(["get_del", "get_adj", "get_ctl"]):
        means = [np.mean([d[g_key] for d in data_get[c]]) if data_get[c] else 0 for c in conditions]
        errs = [np.std([d[g_key] for d in data_get[c]]) if data_get[c] else 0 for c in conditions]
        ax1.bar(x + i*width, means, width, yerr=errs, capsize=4, label=get_labels[i], alpha=0.85)

    ax1.set_xticks(x + width)
    ax1.set_xticklabels(conditions)
    ax1.set_ylabel("P99 Latency (microseconds)")
    ax1.set_title("(a) Point Query P99 Latency in High-Pressure Phase C", fontweight="bold")
    ax1.legend(loc="upper right", frameon=True)
    ax1.grid(True, linestyle="--", alpha=0.5)

    # Bar chart for Scan P99
    width = 0.35
    for i, s_key in enumerate(["scan_inter", "scan_non"]):
        means = [np.mean([d[s_key] for d in data_scan[c]]) if data_scan[c] else 0 for c in conditions]
        errs = [np.std([d[s_key] for d in data_scan[c]]) if data_scan[c] else 0 for c in conditions]
        ax2.bar(x + i*width, means, width, yerr=errs, capsize=4, label=scan_labels[i], alpha=0.85)

    ax2.set_xticks(x + width/2)
    ax2.set_xticklabels(conditions)
    ax2.set_ylabel("P99 Latency (microseconds)")
    ax2.set_title("(b) Range Query P99 Latency in High-Pressure Phase C", fontweight="bold")
    ax2.legend(loc="upper right", frameon=True)
    ax2.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_latency_breakdown.pdf"))
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_latency_breakdown.png"))
    plt.close()
    print("  [PLOTS] fig_lc20_latency_breakdown generated.")

def plot_fig3_flush_dynamics(summaries):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

    conditions = ["T0-NATIVE", "T64-STATIC"]
    flush_data = {c: {"range_del": [], "wbuf_full": []} for c in conditions}
    p_dist = {c: {"p50": [], "p75": [], "p90": [], "max": []} for c in conditions}

    for r_name, s in summaries.items():
        cond = "T0-NATIVE" if s.get("threshold") == 0 and not s.get("is_clean") else ("T64-STATIC" if s.get("threshold") > 0 else None)
        if not cond: continue
        fs = s["flush_stats"]
        flush_data[cond]["range_del"].append(fs["range_del_flushes"])
        flush_data[cond]["wbuf_full"].append(fs["write_buffer_full_flushes"])
        p_dist[cond]["p50"].append(fs["tombstones_per_memtable_p50"])
        p_dist[cond]["p75"].append(fs["tombstones_per_memtable_p75"])
        p_dist[cond]["p90"].append(fs["tombstones_per_memtable_p90"])
        p_dist[cond]["max"].append(fs["tombstones_per_memtable_max"])

    # Stacked bar of Flush reasons
    x = np.arange(len(conditions))
    width = 0.45
    mean_wbuf = [np.mean(flush_data[c]["wbuf_full"]) for c in conditions]
    mean_rdel = [np.mean(flush_data[c]["range_del"]) for c in conditions]

    ax1.bar(x, mean_wbuf, width, label="WriteBufferFull (Capacity)", color="#4575b4", alpha=0.85)
    ax1.bar(x, mean_rdel, width, bottom=mean_wbuf, label="RangeDeletion (Threshold)", color="#d73027", alpha=0.85)

    ax1.set_xticks(x)
    ax1.set_xticklabels(conditions)
    ax1.set_ylabel("Total Flush Count")
    ax1.set_title("(a) MemTable Flush Triggers by Reason", fontweight="bold")
    ax1.legend(loc="upper right", frameon=True)
    ax1.grid(True, linestyle="--", alpha=0.5)

    # Tombstones per Memtable percentiles
    metrics = ["P50", "P75", "P90", "Max"]
    x2 = np.arange(len(metrics))
    w2 = 0.35

    t0_vals = [np.mean(p_dist["T0-NATIVE"][k.lower()]) for k in metrics]
    t64_vals = [np.mean(p_dist["T64-STATIC"][k.lower()]) for k in metrics]

    ax2.bar(x2 - w2/2, t0_vals, w2, label="T0-NATIVE", color=PALETTE["T0-NATIVE"], alpha=0.85)
    ax2.bar(x2 + w2/2, t64_vals, w2, label="T64-STATIC", color=PALETTE["T64-STATIC"], alpha=0.85)

    ax2.set_xticks(x2)
    ax2.set_xticklabels(metrics)
    ax2.set_ylabel("Tombstones per Sealed MemTable")
    ax2.set_title("(b) Sealed MemTable Tombstone Density Distribution", fontweight="bold")
    ax2.legend(loc="upper right", frameon=True)
    ax2.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_memtable_flush_dynamics.pdf"))
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_memtable_flush_dynamics.png"))
    plt.close()
    print("  [PLOTS] fig_lc20_memtable_flush_dynamics generated.")

def plot_fig4_multilevel(snapshots):
    if not snapshots: return
    fig, ax = plt.subplots(figsize=(10, 6))

    # Pick representative run
    r_key = list(snapshots.keys())[0]
    df = snapshots[r_key]

    # Plot total range tombstones per level across phases
    phases = ["phase_a", "phase_b", "phase_c", "phase_d", "COOLDOWN_FINAL"]
    levels = range(7)

    phase_data = {l: [] for l in levels}
    for p in phases:
        sub = df[df["phase_label"] == p]
        for l in levels:
            row = sub[sub["level"] == l]
            val = row["total_range_tombstones"].values[0] if not row.empty else 0
            phase_data[l].append(val)

    x = np.arange(len(phases))
    bottom = np.zeros(len(phases))
    colors = plt.cm.viridis(np.linspace(0, 1, 7))

    for l in levels:
        vals = np.array(phase_data[l])
        ax.bar(x, vals, bottom=bottom, label=f"Level {l}", color=colors[l], alpha=0.85)
        bottom += vals

    ax.set_xticks(x)
    ax.set_xticklabels(["Phase A", "Phase B", "Phase C", "Phase D", "Cooldown Final"])
    ax.set_ylabel("Total Range Tombstones Count in SSTs")
    ax.set_title("Range Tombstone Multi-Level SST Migration & Compaction Elimination", fontweight="bold")
    ax.legend(loc="upper left", ncol=4, frameon=True)
    ax.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_multilevel_migration.pdf"))
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_multilevel_migration.png"))
    plt.close()
    print("  [PLOTS] fig_lc20_multilevel_migration generated.")

def plot_fig5_cooldown(summaries):
    fig, ax = plt.subplots(figsize=(10, 6))

    conditions = ["CLEAN", "T0-NATIVE", "T64-STATIC"]
    disk_data = {c: [] for c in conditions}
    compactions_data = {c: [] for c in conditions}
    dropped_data = {c: [] for c in conditions}

    for r_name, s in summaries.items():
        cond = "CLEAN" if s.get("is_clean") else ("T0-NATIVE" if s.get("threshold") == 0 else "T64-STATIC")
        disk_data[cond].append(s["physical_db_bytes"] / (1024**3))
        compactions_data[cond].append(s["flush_stats"]["total_compactions"])
        dropped_data[cond].append(s["flush_stats"]["total_dropped_records"] / 1e6)

    x = np.arange(len(conditions))
    width = 0.35

    mean_disk = [np.mean(disk_data[c]) for c in conditions]
    std_disk = [np.std(disk_data[c]) for c in conditions]
    mean_drop = [np.mean(dropped_data[c]) for c in conditions]
    std_drop = [np.std(dropped_data[c]) for c in conditions]

    ax.bar(x - width/2, mean_disk, width, yerr=std_disk, capsize=4, label="Physical DB Size (GiB)", color="#4575b4", alpha=0.85)
    ax2 = ax.twinx()
    ax2.bar(x + width/2, mean_drop, width, yerr=std_drop, capsize=4, label="Dropped Records (Millions)", color="#d73027", alpha=0.85)

    ax.set_xticks(x)
    ax.set_xticklabels(conditions)
    ax.set_ylabel("Physical Disk Footprint (GiB)", color="#4575b4")
    ax2.set_ylabel("Compaction Dropped Records (Millions)", color="#d73027")
    ax.set_title("Post-Workload Debt Convergence: Physical Footprint vs Tombstone Cleanup", fontweight="bold")
    ax.grid(True, linestyle="--", alpha=0.5)

    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_cooldown_convergence.pdf"))
    plt.savefig(os.path.join(OUTPUT_DIR, "fig_lc20_cooldown_convergence.png"))
    plt.close()
    print("  [PLOTS] fig_lc20_cooldown_convergence generated.")

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    run_dfs, summaries, snapshots, events = load_matrix_data()
    if not summaries:
        print("  [WARN] No completed formal runs found yet. Skipping plotting.")
        return

    print(f"  [PLOTS] Found {len(summaries)} completed formal runs. Generating figures...")
    plot_fig1_throughput(run_dfs, summaries)
    plot_fig2_latency(run_dfs, summaries)
    plot_fig3_flush_dynamics(summaries)
    plot_fig4_multilevel(snapshots)
    plot_fig5_cooldown(summaries)
    print("  [PLOTS] All 5 figures generated successfully in", OUTPUT_DIR)

if __name__ == "__main__":
    main()
