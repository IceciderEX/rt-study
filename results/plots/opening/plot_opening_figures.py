#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plot_opening_figures.py
Master Thesis Opening Report Formal Figures Generator
Generates 4 publication-quality scientific figures for thesis and defense slides.
Complies with strict academic integrity, Okabe-Ito colorblind palette, and exact statistical data.
"""

import os
import sys
import json
import hashlib
import numpy as np
import pandas as pd
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as patches
import matplotlib.font_manager as fm
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle
from matplotlib.lines import Line2D
from matplotlib.font_manager import FontProperties
from matplotlib.offsetbox import TextArea, HPacker, VPacker, AnchoredOffsetbox, DrawingArea

# Register local font paths
LOCAL_FONT_DIR = os.path.expanduser("~/.local/share/fonts")
TIMES_TTF = os.path.join(LOCAL_FONT_DIR, "times.ttf")
TIMESBD_TTF = os.path.join(LOCAL_FONT_DIR, "timesbd.ttf")
SIMSUN_TTF = os.path.join(LOCAL_FONT_DIR, "simsun.ttf")

for f_path in [TIMES_TTF, TIMESBD_TTF, SIMSUN_TTF]:
    if os.path.exists(f_path):
        fm.fontManager.addfont(f_path)

# Configure matplotlib publication parameters
matplotlib.rcParams["svg.fonttype"] = "none"  # Preserve text in SVG
matplotlib.rcParams["pdf.fonttype"] = 42    # Embed TrueType Type 42 fonts in PDF
matplotlib.rcParams["ps.fonttype"] = 42
matplotlib.rcParams["axes.unicode_minus"] = False

# Font selection hierarchy
font_candidates = ["Noto Sans CJK SC", "Microsoft YaHei", "DejaVu Sans", "Arial", "sans-serif"]
matplotlib.rcParams["font.sans-serif"] = font_candidates
matplotlib.rcParams["font.family"] = "sans-serif"

# Base directory
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.join(BASE_DIR, "source-data")

# Color palette: Okabe-Ito (colorblind safe)
COLORS = {
    "T0": "#666666",        # Dark Gray (Default Off)
    "T64": "#D55E00",       # Vermilion / Red-Orange (Aggressive)
    "T256": "#E69F00",      # Orange (Intermediate)
    "T512": "#0072B2",      # Blue (Balanced Tradeoff)
    "T1024": "#56B4E9",     # Sky Blue
    "T2048": "#009E73",     # Bluish Green (Conservative)
    "grid": "#E0E0E0",      # Grid line
    "border": "#333333",    # Frame border
    "annot": "#222222",     # Annotation text
    "gray_line": "#999999", # Secondary line
    "orange_path": "#D55E00",
    "blue_path": "#0072B2",
    "pink_cost": "#C0392B",
}

# Marker mapping for grayscale differentiation
MARKERS = {
    "T0": "o",
    "T64": "^",
    "T256": "D",
    "T512": "s",
    "T1024": "v",
    "T2048": "p",
}

HATCHES = {
    "T0": "",
    "T64": "///",
    "T256": "\\\\\\",
    "T512": "...",
    "T1024": "xxx",
    "T2048": "+++"
}


def compute_sha256(filepath):
    """Compute SHA-256 hash of a file."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(8192):
            h.update(chunk)
    return h.hexdigest()


# ==============================================================================
# Figure 4-1: Range Tombstone Mechanism and Maintenance Trade-off Diagram
# ==============================================================================
def plot_figure_4_1():
    """Generate Fig 4-1: Qualitative mechanism diagram for Range Tombstones."""
    fig, ax = plt.subplots(figsize=(13.6, 7.6), dpi=300)
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 100)
    ax.axis("off")

    def draw_box(x, y, w, h, title, subtitle="", bg="#FFFFFF", border="#333333", lw=1.5, title_color="#111111", sub_color="#444444", radius=2.2):
        box = FancyBboxPatch(
            (x, y), w, h,
            boxstyle=f"round,pad=0.2,rounding_size={radius}",
            facecolor=bg, edgecolor=border, linewidth=lw,
            zorder=3
        )
        ax.add_patch(box)
        cx = x + w / 2.0
        if subtitle:
            cy_t = y + h * 0.64
            cy_s = y + h * 0.28
            ax.text(cx, cy_t, title, ha="center", va="center", fontsize=9.2, fontweight="bold", color=title_color, zorder=4)
            ax.text(cx, cy_s, subtitle, ha="center", va="center", fontsize=8.0, color=sub_color, zorder=4)
        else:
            cy_t = y + h / 2.0
            ax.text(cx, cy_t, title, ha="center", va="center", fontsize=9.2, fontweight="bold", color=title_color, zorder=4)

    def draw_arrow(x1, y1, x2, y2, color="#555555", lw=1.6, style="-|>", rad=0.0):
        connectionstyle = f"arc3,rad={rad}" if rad != 0.0 else "arc3,rad=0"
        arrow = FancyArrowPatch(
            (x1, y1), (x2, y2),
            arrowstyle=style, mutation_scale=12,
            color=color, linewidth=lw,
            connectionstyle=connectionstyle,
            zorder=2
        )
        ax.add_patch(arrow)

    # --- Background Zone Containers ---
    # Zone 1: Main Problem Path (Top)
    zone1 = FancyBboxPatch((2.0, 62.0), 96.0, 35.0, boxstyle="round,pad=0.5,rounding_size=3", facecolor="#FFF9F5", edgecolor="#F0B27A", linewidth=1.2, linestyle="--", zorder=1)
    ax.add_patch(zone1)
    ax.text(4.0, 94.0, "【主问题路径】高密度范围删除下的读性能退化机制", fontsize=10.2, fontweight="bold", color="#B95C00", zorder=2)

    # Zone 2: Maintenance Path (Bottom Left)
    zone2 = FancyBboxPatch((2.0, 18.0), 47.0, 41.0, boxstyle="round,pad=0.5,rounding_size=3", facecolor="#F4FAFE", edgecolor="#85C1E9", linewidth=1.2, linestyle="--", zorder=1)
    ax.add_patch(zone2)
    ax.text(4.0, 56.0, "【主动维护路径】提前 Flush 与物理回收", fontsize=10.2, fontweight="bold", color="#005B94", zorder=2)

    # Zone 3: Maintenance Cost (Bottom Right)
    zone3 = FancyBboxPatch((51.0, 18.0), 47.0, 41.0, boxstyle="round,pad=0.5,rounding_size=3", facecolor="#FDF4F5", edgecolor="#F1948A", linewidth=1.2, linestyle="--", zorder=1)
    ax.add_patch(zone3)
    ax.text(53.0, 56.0, "【维护代价】激进下刷引发的写放大与资源争抢", fontsize=10.2, fontweight="bold", color="#B03A2E", zorder=2)

    # --- Step 1: Main Path Nodes (Horizontal flow, Top) ---
    draw_box(4.0, 71.5, 13.0, 16.5, "小 Value 负载", "高频 RangeDelete 写入", bg="#FFFFFF", border="#E69F00", lw=1.6, title_color="#B95C00")
    draw_box(19.8, 71.5, 14.5, 16.5, "墓碑物理体积小", "但逻辑覆盖范围广", bg="#FFFFFF", border="#E69F00", lw=1.6, title_color="#B95C00")
    draw_box(37.0, 71.5, 15.5, 16.5, "活跃 MemTable", "未达容量 Flush 阈值", bg="#FFFFFF", border="#D55E00", lw=2.2, title_color="#A04000")
    draw_box(55.5, 71.5, 14.5, 16.5, "墓碑持续滞留", "参与 Get/Scan 读取链路", bg="#FFFFFF", border="#E69F00", lw=1.6, title_color="#B95C00")
    draw_box(72.5, 71.5, 12.0, 16.5, "内部迭代比对增加", "可见性判断开销剧增", bg="#FFFFFF", border="#E69F00", lw=1.6, title_color="#B95C00")
    draw_box(87.0, 71.5, 9.5, 16.5, "读吞吐骤降", "读尾延迟暴增", bg="#FDEDEC", border="#C0392B", lw=2.0, title_color="#900C3F")

    # Arrows in Main Path
    draw_arrow(17.0, 79.75, 19.8, 79.75, color="#E69F00", lw=1.8)
    draw_arrow(34.3, 79.75, 37.0, 79.75, color="#E69F00", lw=1.8)
    draw_arrow(52.5, 79.75, 55.5, 79.75, color="#E69F00", lw=1.8)
    draw_arrow(70.0, 79.75, 72.5, 79.75, color="#E69F00", lw=1.8)
    draw_arrow(84.5, 79.75, 87.0, 79.75, color="#C0392B", lw=1.8)

    # --- Step 2: Maintenance Path & Bifurcation Layout ---
    # Downward straight branch from "活跃 MemTable" to "提前触发 Flush"
    draw_arrow(44.75, 71.5, 44.75, 50.0, color="#0072B2", lw=2.2)
    ax.text(45.8, 61.5, "阈值干预", fontsize=8.8, fontweight="bold", color="#0072B2")

    # Center-left bifurcation node: 提前触发 Flush
    draw_box(36.5, 36.5, 12.0, 14.5, "提前触发 Flush", "定量/定容主动下刷", bg="#EBF5FB", border="#0072B2", lw=2.0, title_color="#005B94")
    draw_box(20.0, 36.5, 12.5, 14.5, "墓碑固化至 L0", "脱离活跃 MemTable", bg="#FFFFFF", border="#0072B2", lw=1.8, title_color="#005B94")
    draw_box(4.0, 36.5, 13.0, 14.5, "推进 Compaction", "失效数据物理回收", bg="#FFFFFF", border="#0072B2", lw=1.8, title_color="#005B94")

    # Flow in Maintenance Path (Right to Left: Flush -> L0 -> Compaction)
    draw_arrow(36.5, 43.75, 32.5, 43.75, color="#0072B2", lw=1.8)
    draw_arrow(20.0, 43.75, 17.0, 43.75, color="#0072B2", lw=1.8)

    # Benefit banner inside maintenance zone
    benefit_box = FancyBboxPatch((4.0, 20.5), 43.0, 11.5, boxstyle="round,pad=0.2,rounding_size=1.5", facecolor="#E8F8F5", edgecolor="#009E73", linewidth=1.2, zorder=3)
    ax.add_patch(benefit_box)
    ax.text(25.5, 26.25, "✓ 消除活跃 MemTable 范围墓碑，恢复 Get/Scan 读性能", ha="center", va="center", fontsize=8.3, fontweight="bold", color="#0E6251", zorder=4)

    # --- Step 3: Cost Path Nodes (Branching Rightward from 提前 Flush) ---
    draw_arrow(48.5, 43.75, 53.0, 43.75, color="#D55E00", lw=2.0)
    ax.text(49.2, 46.2, "伴生代价", fontsize=8.5, fontweight="bold", color="#D55E00")

    draw_box(53.0, 36.5, 10.0, 14.5, "碎片小 SST 增加", "L0 文件数迅速累积", bg="#FFFFFF", border="#D55E00", lw=1.6, title_color="#A04000")
    draw_box(65.0, 36.5, 11.5, 14.5, "Compaction 激增", "写放大 (WA) 显著上升", bg="#FFFFFF", border="#D55E00", lw=1.6, title_color="#A04000")
    draw_box(78.5, 36.5, 9.5, 14.5, "CPU / IO 争抢", "系统后台压力加剧", bg="#FFFFFF", border="#D55E00", lw=1.6, title_color="#A04000")
    draw_box(90.0, 36.5, 7.0, 14.5, "Put 尾延迟", "写停顿风险", bg="#FDEDEC", border="#C0392B", lw=2.0, title_color="#900C3F")

    draw_arrow(63.0, 43.75, 65.0, 43.75, color="#D55E00", lw=1.6)
    draw_arrow(76.5, 43.75, 78.5, 43.75, color="#D55E00", lw=1.6)
    draw_arrow(88.0, 43.75, 90.0, 43.75, color="#C0392B", lw=1.6)

    # Cost penalty banner inside cost zone
    penalty_box = FancyBboxPatch((53.0, 20.5), 44.0, 11.5, boxstyle="round,pad=0.2,rounding_size=1.5", facecolor="#FADBD8", edgecolor="#C0392B", linewidth=1.2, zorder=3)
    ax.add_patch(penalty_box)
    ax.text(75.0, 26.25, "⚠ 激进提前 Flush 产生高昂写放大 (WA 达数十倍) 与写停顿代价", ha="center", va="center", fontsize=8.3, fontweight="bold", color="#78281F", zorder=4)

    # --- Step 4: Bottom Boundary Description Box ---
    boundary_box = FancyBboxPatch(
        (2.0, 2.0), 96.0, 13.5,
        boxstyle="round,pad=0.4,rounding_size=2.5",
        facecolor="#F8F9FA", edgecolor="#BDC3C7", linewidth=1.2, zorder=1
    )
    ax.add_patch(boundary_box)
    ax.text(4.0, 11.2, "【适用边界与权衡本质】", fontsize=9.2, fontweight="bold", color="#2C3E50", zorder=2)
    boundary_text = (
        "• 静态落盘墓碑或低强度 RangeDelete 通常被 SST 索引与布隆过滤器有效剪枝，不一定引发严重读退化；\n"
        "• 读性能退化与写开销权衡的核心矛盾，主要集中在「高密度动态删除」与「活跃 MemTable 生命周期」强耦合的场景；\n"
        "• 盲目提前 Flush 会将内存读开销转化为严重的写放大与写停顿，因此需要探索兼顾读延迟与写放大的自适应控制机制。"
    )
    ax.text(4.0, 5.8, boundary_text, fontsize=8.2, color="#34495E", va="center", linespacing=1.45, zorder=2)

    plt.tight_layout()
    output_prefix = os.path.join(BASE_DIR, "fig4-1-range-tombstone-mechanism")
    fig.savefig(f"{output_prefix}.svg", format="svg", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.pdf", format="pdf", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.png", format="png", dpi=600, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("✓ Figure 4-1 generated.")


# ==============================================================================
# Figure 4-2: DeleteRange Ratio Sensitivity
# ==============================================================================
def plot_figure_4_2():
    """Generate Fig 4-2: DeleteRange ratio sensitivity (Bar Chart: Throughput, Scan, Get P99)."""
    csv_path = os.path.join(SOURCE_DIR, "fig4-2-delete-ratio.csv")
    df = pd.read_csv(csv_path)

    times_98 = FontProperties(fname=TIMES_TTF, size=9.8)
    times_90 = FontProperties(fname=TIMES_TTF, size=9.0)
    times_105 = FontProperties(fname=TIMES_TTF, size=10.5)
    timesbd_105 = FontProperties(fname=TIMESBD_TTF, size=10.5)

    simsun_98 = FontProperties(fname=SIMSUN_TTF, size=9.8)
    simsun_105 = FontProperties(fname=SIMSUN_TTF, size=10.5)

    fig, axes = plt.subplots(1, 3, figsize=(12.0, 3.8), dpi=300)

    ratios = df["delete_ratio_pct"].values
    x_pos = np.arange(len(ratios))
    x_labels = [f"{r:g}%" for r in ratios]

    # Helper for X-axis label
    def set_mixed_xlabel(ax):
        x1 = TextArea("DeleteRange", textprops=dict(fontproperties=times_98))
        x2 = TextArea("比例", textprops=dict(fontproperties=simsun_98))
        x3 = TextArea("(%)", textprops=dict(fontproperties=times_98))
        x_packer = HPacker(children=[x1, x2, x3], align="baseline", pad=0, sep=0)
        anchored_x = AnchoredOffsetbox(loc="upper center", child=x_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, -0.13), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_x)

    # Helper for Y-axis label (stacked bottom to top)
    def set_mixed_ylabel(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp, rotation=90)) for t, fp in parts]
        y_packer = VPacker(children=text_areas[::-1], align="baseline", pad=0, sep=0)
        anchored_y = AnchoredOffsetbox(loc="center right", child=y_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(-0.15, 0.5), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_y)

    # Helper for Title
    def set_mixed_title(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp)) for t, fp in parts]
        t_packer = HPacker(children=text_areas, align="baseline", pad=0, sep=0)
        anchored_t = AnchoredOffsetbox(loc="lower center", child=t_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, 1.02), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_t)

    # --- Subplot (a): 总体吞吐 ---
    ax = axes[0]
    y = df["throughput_mean_iops"].values
    y_err = df["throughput_std_iops"].values
    ax.bar(
        x_pos, y, yerr=y_err,
        width=0.55, color="#0072B2", edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_yscale("log")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(x_labels, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(a) ", timesbd_105), ("总体吞吐", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("总体吞吐", simsun_98), ("(ops/s", times_98), ("，对数坐标)", simsun_98)])

    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(800, 1000000)

    # --- Subplot (b): Scan单位有效键开销 ---
    ax = axes[1]
    y_scan = df["scan_mean_us_per_key"].values
    y_scan_err = df["scan_std_us_per_key"].values
    ax.bar(
        x_pos, y_scan, yerr=y_scan_err,
        width=0.55, color="#D55E00", edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_yscale("log")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(x_labels, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(b) ", timesbd_105), ("Scan", times_105), ("单位有效键开销", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Scan", times_98), ("开销", simsun_98), ("(μs/key", times_98), ("，对数坐标)", simsun_98)])

    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(0.3, 500)

    # --- Subplot (c): Get(Control)P99延迟 ---
    ax = axes[2]
    y_get = df["get_ctrl_p99_mean_us"].values
    y_get_err = df["get_ctrl_p99_std_us"].values
    ax.bar(
        x_pos, y_get, yerr=y_get_err,
        width=0.55, color="#E69F00", edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_yscale("log")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(x_labels, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(c) ", timesbd_105), ("Get(Control)P99", times_105), ("延迟", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Get(Control)P99", times_98), ("(μs", times_98), ("，对数坐标)", simsun_98)])

    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(10, 35000)

    plt.tight_layout()
    output_prefix = os.path.join(BASE_DIR, "fig4-2-delete-ratio")
    fig.savefig(f"{output_prefix}.svg", format="svg", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.pdf", format="pdf", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.png", format="png", dpi=600, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("✓ Figure 4-2 generated.")


# ==============================================================================
# Figure 4-3: Fixed Threshold Trade-off (Formal V2 F1 Data)
# ==============================================================================
def plot_figure_4_3():
    """Generate Fig 4-3: Static threshold tradeoff (Bar Charts: Scan P99, Put P99, Output Write Amp)."""
    csv_path = os.path.join(SOURCE_DIR, "fig4-3-threshold-tradeoff.csv")
    df = pd.read_csv(csv_path)

    configs = df["config"].values
    x_pos = np.arange(len(configs))

    times_98 = FontProperties(fname=TIMES_TTF, size=9.8)
    times_90 = FontProperties(fname=TIMES_TTF, size=9.0)
    times_105 = FontProperties(fname=TIMES_TTF, size=10.5)
    timesbd_105 = FontProperties(fname=TIMESBD_TTF, size=10.5)
    times_75 = FontProperties(fname=TIMES_TTF, size=7.5)

    simsun_98 = FontProperties(fname=SIMSUN_TTF, size=9.8)
    simsun_105 = FontProperties(fname=SIMSUN_TTF, size=10.5)
    simsun_75 = FontProperties(fname=SIMSUN_TTF, size=7.5)

    bar_colors = [COLORS[cfg] for cfg in configs]

    fig, axes = plt.subplots(1, 3, figsize=(13.0, 3.9), dpi=300)

    # Helper for X-axis label
    def set_mixed_xlabel(ax, text="范围删除下刷数量阈值"):
        x1 = TextArea(text, textprops=dict(fontproperties=simsun_98))
        anchored_x = AnchoredOffsetbox(loc="upper center", child=x1, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, -0.13), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_x)

    # Helper for Y-axis label (stacked bottom to top)
    def set_mixed_ylabel(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp, rotation=90)) for t, fp in parts]
        y_packer = VPacker(children=text_areas[::-1], align="baseline", pad=0, sep=0)
        anchored_y = AnchoredOffsetbox(loc="center right", child=y_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(-0.15, 0.5), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_y)

    # Helper for Title
    def set_mixed_title(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp)) for t, fp in parts]
        t_packer = HPacker(children=text_areas, align="baseline", pad=0, sep=0)
        anchored_t = AnchoredOffsetbox(loc="lower center", child=t_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, 1.02), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_t)

    # --- Subplot (a): Scan P99 ---
    ax = axes[0]
    y_scan = df["scan_p99_mean_us"].values
    y_scan_err = df["scan_p99_std_us"].values
    ax.bar(
        x_pos, y_scan, yerr=y_scan_err,
        width=0.55, color=bar_colors, edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_yscale("log")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(configs, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(a) ", timesbd_105), ("Scan P99", times_105), ("读取尾延迟", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Scan P99", times_98), ("延迟", simsun_98), ("(μs", times_98), ("，对数坐标)", simsun_98)])

    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(30, 15000)

    # --- Subplot (b): Put P99 ---
    ax = axes[1]
    y_put = df["put_p99_mean_us"].values
    y_put_err = df["put_p99_std_us"].values
    ax.bar(
        x_pos, y_put, yerr=y_put_err,
        width=0.55, color=bar_colors, edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_yscale("log")
    ax.set_xticks(x_pos)
    ax.set_xticklabels(configs, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(b) ", timesbd_105), ("Put P99", times_105), ("写入尾延迟", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Put P99", times_98), ("延迟", simsun_98), ("(μs", times_98), ("，对数坐标)", simsun_98)])

    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(10, 30000)

    # --- Subplot (c): Foreground Output Write Amplification ---
    ax = axes[2]
    y_wa = df["fwa_val_norm_mean"].values
    y_wa_err = df["fwa_val_norm_std"].values

    ax.bar(
        x_pos, y_wa, yerr=y_wa_err,
        width=0.55, color=bar_colors, edgecolor="#222222", linewidth=0.9,
        capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"),
        zorder=3
    )
    ax.set_xticks(x_pos)
    ax.set_xticklabels(configs, fontproperties=times_90)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(c) ", timesbd_105), ("前台阶段引擎输出写放大", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("前台引擎输出写放大", simsun_98), ("(线性坐标)", simsun_98)])

    ax.grid(True, which="major", axis="y", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.set_ylim(0, 60)

    # Note directly under Subplot (c) in smaller font size
    n1 = TextArea("* 注：", textprops=dict(fontproperties=simsun_75, color="#555555"))
    n2 = TextArea("T0", textprops=dict(fontproperties=times_75, color="#555555"))
    n3 = TextArea("表示前台阶段无", textprops=dict(fontproperties=simsun_75, color="#555555"))
    n4 = TextArea("Flush/Compaction", textprops=dict(fontproperties=times_75, color="#555555"))
    n5 = TextArea("引擎输出，不代表无物理写入。", textprops=dict(fontproperties=simsun_75, color="#555555"))

    note_packer = HPacker(children=[n1, n2, n3, n4, n5], align="baseline", pad=0, sep=0)
    anchored_note = AnchoredOffsetbox(loc="upper center", child=note_packer, pad=0.0, frameon=False,
                                      bbox_to_anchor=(0.5, -0.23), bbox_transform=ax.transAxes, borderpad=0.0)
    ax.add_artist(anchored_note)

    plt.tight_layout()
    output_prefix = os.path.join(BASE_DIR, "fig4-3-threshold-tradeoff")
    fig.savefig(f"{output_prefix}.svg", format="svg", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.pdf", format="pdf", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.png", format="png", dpi=600, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("✓ Figure 4-3 generated.")


# ==============================================================================
# Figure 4-4: Scale Validation (500K vs 24GiB)
# ==============================================================================
def plot_figure_4_4():
    """Generate Fig 4-4: Scale validation (Grouped Bar Chart: 500K vs 24GiB, T0 vs T512)."""
    csv_path = os.path.join(SOURCE_DIR, "fig4-4-scale-validation.csv")
    df = pd.read_csv(csv_path)

    times_98 = FontProperties(fname=TIMES_TTF, size=9.8)
    times_90 = FontProperties(fname=TIMES_TTF, size=9.0)
    times_75 = FontProperties(fname=TIMES_TTF, size=7.5)
    times_105 = FontProperties(fname=TIMES_TTF, size=10.5)
    timesbd_105 = FontProperties(fname=TIMESBD_TTF, size=10.5)

    simsun_98 = FontProperties(fname=SIMSUN_TTF, size=9.8)
    simsun_90 = FontProperties(fname=SIMSUN_TTF, size=9.0)
    simsun_75 = FontProperties(fname=SIMSUN_TTF, size=7.5)
    simsun_105 = FontProperties(fname=SIMSUN_TTF, size=10.5)

    fig, axes = plt.subplots(1, 3, figsize=(13.2, 4.0), dpi=300)

    x_pos = np.array([0.0, 1.0])
    w = 0.28
    offset = w / 2 + 0.02

    # Filter data
    d_500k_t0 = df[(df["scale"] == "500K") & (df["config"] == "T0")].iloc[0]
    d_500k_t512 = df[(df["scale"] == "500K") & (df["config"] == "T512")].iloc[0]
    d_24g_t0 = df[(df["scale"] == "24GiB") & (df["config"] == "T0")].iloc[0]
    d_24g_t512 = df[(df["scale"] == "24GiB") & (df["config"] == "T512")].iloc[0]

    c_t0 = "#56B4E9"   # Sky Blue
    c_t512 = "#0072B2" # Deep Blue

    def set_mixed_xlabel(ax, text="数据规模"):
        x1 = TextArea(text, textprops=dict(fontproperties=simsun_98))
        anchored_x = AnchoredOffsetbox(loc="upper center", child=x1, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, -0.13), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_x)

    def set_mixed_ylabel(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp, rotation=90)) for t, fp in parts]
        y_packer = VPacker(children=text_areas[::-1], align="baseline", pad=0, sep=0)
        anchored_y = AnchoredOffsetbox(loc="center right", child=y_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(-0.15, 0.5), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_y)

    def set_mixed_title(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp)) for t, fp in parts]
        t_packer = HPacker(children=text_areas, align="baseline", pad=0, sep=0)
        anchored_t = AnchoredOffsetbox(loc="lower center", child=t_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, 1.02), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_t)

    def format_xtick_labels(ax):
        ax.set_xticks(x_pos)
        ax.set_xticklabels([])
        t1 = TextArea("500K Key", textprops=dict(fontproperties=times_90))
        a1 = AnchoredOffsetbox(loc="upper center", child=t1, pad=0.0, frameon=False,
                               bbox_to_anchor=(0.0, -0.03), bbox_transform=ax.get_xaxis_transform(), borderpad=0.0)
        ax.add_artist(a1)
        k1 = TextArea("24GiB (", textprops=dict(fontproperties=times_90))
        k2 = TextArea("约", textprops=dict(fontproperties=simsun_90))
        k3 = TextArea("100M Key)", textprops=dict(fontproperties=times_90))
        p_k = HPacker(children=[k1, k2, k3], align="baseline", pad=0, sep=0)
        a2 = AnchoredOffsetbox(loc="upper center", child=p_k, pad=0.0, frameon=False,
                               bbox_to_anchor=(1.0, -0.03), bbox_transform=ax.get_xaxis_transform(), borderpad=0.0)
        ax.add_artist(a2)

    def add_compact_legend(ax, loc="upper right", bbox_to_anchor=(0.98, 0.98)):
        da1 = DrawingArea(9, 7, 0, 0)
        r1 = Rectangle((0, 1), 8, 5.5, facecolor=c_t0, edgecolor="#222222", lw=0.6)
        da1.add_artist(r1)
        t1_en = TextArea("T0", textprops=dict(fontproperties=times_75))
        t1_sp = TextArea(" ", textprops=dict(fontproperties=times_75))
        t1_cn = TextArea("(默认关闭)", textprops=dict(fontproperties=simsun_75))
        row1 = HPacker(children=[da1, t1_en, t1_sp, t1_cn], align="center", pad=0, sep=1.5)

        da2 = DrawingArea(9, 7, 0, 0)
        r2 = Rectangle((0, 1), 8, 5.5, facecolor=c_t512, edgecolor="#222222", lw=0.6)
        da2.add_artist(r2)
        t2_en = TextArea("T512", textprops=dict(fontproperties=times_75))
        t2_sp = TextArea(" ", textprops=dict(fontproperties=times_75))
        t2_cn = TextArea("(固定阈值下刷)", textprops=dict(fontproperties=simsun_75))
        row2 = HPacker(children=[da2, t2_en, t2_sp, t2_cn], align="center", pad=0, sep=1.5)

        box = VPacker(children=[row1, row2], align="left", pad=1.5, sep=2.0)
        anchored_box = AnchoredOffsetbox(loc=loc, child=box, pad=0.15, frameon=True,
                                         bbox_to_anchor=bbox_to_anchor, bbox_transform=ax.transAxes, borderpad=0.15)
        anchored_box.patch.set_boxstyle("round,pad=0.15,rounding_size=0.1")
        anchored_box.patch.set_facecolor("white")
        anchored_box.patch.set_edgecolor("#D0D0D0")
        anchored_box.patch.set_linewidth(0.6)
        anchored_box.patch.set_alpha(0.92)
        ax.add_artist(anchored_box)

    # --- Subplot (a): Trace IOPS ---
    ax = axes[0]
    y_t0 = np.array([d_500k_t0["trace_iops_mean"], d_24g_t0["trace_iops_mean"]])
    y_t0_err = np.array([d_500k_t0["trace_iops_std"], d_24g_t0["trace_iops_std"]])
    y_t512 = np.array([d_500k_t512["trace_iops_mean"], d_24g_t512["trace_iops_mean"]])
    y_t512_err = np.array([d_500k_t512["trace_iops_std"], d_24g_t512["trace_iops_std"]])

    ax.bar(x_pos - offset, y_t0, yerr=y_t0_err, width=w, color=c_t0, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)
    ax.bar(x_pos + offset, y_t512, yerr=y_t512_err, width=w, color=c_t512, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)

    ax.set_yscale("log")
    format_xtick_labels(ax)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(a) ", timesbd_105), ("前台", simsun_105), ("Trace IOPS", times_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("前台", simsun_98), ("Trace IOPS(ops/s", times_98), ("，对数坐标)", simsun_98)])
    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(600, 800000)
    ax.set_xlim(-0.45, 1.45)
    add_compact_legend(ax, loc="upper right", bbox_to_anchor=(0.98, 0.98))

    # --- Subplot (b): Scan P99 ---
    ax = axes[1]
    y_s_t0 = np.array([d_500k_t0["scan_p99_mean_ms"], d_24g_t0["scan_p99_mean_ms"]])
    y_s_t0_err = np.array([d_500k_t0["scan_p99_std_ms"], d_24g_t0["scan_p99_std_ms"]])
    y_s_t512 = np.array([d_500k_t512["scan_p99_mean_ms"], d_24g_t512["scan_p99_mean_ms"]])
    y_s_t512_err = np.array([d_500k_t512["scan_p99_std_ms"], d_24g_t512["scan_p99_std_ms"]])

    ax.bar(x_pos - offset, y_s_t0, yerr=y_s_t0_err, width=w, color=c_t0, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)
    ax.bar(x_pos + offset, y_s_t512, yerr=y_s_t512_err, width=w, color=c_t512, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)

    ax.set_yscale("log")
    format_xtick_labels(ax)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(b) ", timesbd_105), ("Scan P99", times_105), ("读取尾延迟", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Scan P99", times_98), ("延迟", simsun_98), ("(ms", times_98), ("，对数坐标)", simsun_98)])
    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(0.03, 40)
    ax.set_xlim(-0.45, 1.45)

    # --- Subplot (c): Get(Deleted) P99 ---
    ax = axes[2]
    y_g_t0 = np.array([d_500k_t0["get_del_p99_mean_ms"], d_24g_t0["get_del_p99_mean_ms"]])
    y_g_t0_err = np.array([d_500k_t0["get_del_p99_std_ms"], d_24g_t0["get_del_p99_std_ms"]])
    y_g_t512 = np.array([d_500k_t512["get_del_p99_mean_ms"], d_24g_t512["get_del_p99_mean_ms"]])
    y_g_t512_err = np.array([d_500k_t512["get_del_p99_std_ms"], d_24g_t512["get_del_p99_std_ms"]])

    ax.bar(x_pos - offset, y_g_t0, yerr=y_g_t0_err, width=w, color=c_t0, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)
    ax.bar(x_pos + offset, y_g_t512, yerr=y_g_t512_err, width=w, color=c_t512, edgecolor="#222222", linewidth=0.9,
           capsize=3.5, error_kw=dict(elinewidth=1.3, capthick=1.0, ecolor="#222222"), zorder=3)

    ax.set_yscale("log")
    format_xtick_labels(ax)
    for lbl in ax.get_yticklabels():
        lbl.set_fontproperties(times_90)

    set_mixed_title(ax, [("(c) ", timesbd_105), ("Get(Deleted) P99", times_105), ("尾延迟", simsun_105)])
    set_mixed_xlabel(ax)
    set_mixed_ylabel(ax, [("Get(Deleted) P99", times_98), ("延迟", simsun_98), ("(ms", times_98), ("，对数坐标)", simsun_98)])
    ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
    ax.grid(False, which="minor")
    ax.set_ylim(0.015, 40)
    ax.set_xlim(-0.45, 1.45)

    plt.tight_layout()
    output_prefix = os.path.join(BASE_DIR, "fig4-4-scale-validation")
    fig.savefig(f"{output_prefix}.svg", format="svg", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.pdf", format="pdf", bbox_inches="tight", facecolor="white")
    fig.savefig(f"{output_prefix}.png", format="png", dpi=600, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("✓ Figure 4-4 generated.")


# ==============================================================================
# Figure 4-5: LongCycle-20GiB Single Exploratory Run Lifecycle Evolution
# ==============================================================================
def plot_figure_4_5():
    """Generate Fig 4-5: LongCycle-20GiB lifecycle dynamics for T0 vs T64."""
    csv_file = os.path.join(SOURCE_DIR, "fig4-5-longcycle-lifecycle.csv")
    if not os.path.exists(csv_file):
        r1_path = os.path.join(BASE_DIR, "../../formal_v2/longcycle_20g/LC20-R1-T0-NATIVE")
        r3_path = os.path.join(BASE_DIR, "../../formal_v2/longcycle_20g/LC20-R3-T64-STATIC")
        total_ops = 56278568

        def process_run(run_dir, config_name):
            ts = pd.read_csv(os.path.join(run_dir, "timeseries.csv"))
            ev = pd.read_csv(os.path.join(run_dir, "sst_tombstone_events.csv"))
            flushes = ev[ev["event_type"] == "flush"].sort_values("elapsed_sec")
            
            ts["cum_ops"] = ts["interval_trace_ops"].cumsum()
            ts["progress_pct"] = (ts["cum_ops"] / total_ops * 100.0).round(4)
            
            active_tombs = []
            for _, row in ts.iterrows():
                t = row["elapsed_sec"]
                cum_d = row["cum_del_ranges"]
                flushed = flushes[flushes["elapsed_sec"] <= t]["num_range_deletions"].sum()
                active_tombs.append(int(max(0, cum_d - flushed)))
            
            return pd.DataFrame({
                "config": config_name,
                "sample_idx": range(len(ts)),
                "elapsed_sec": ts["elapsed_sec"],
                "phase_id": ts["phase_id"],
                "progress_pct": ts["progress_pct"],
                "active_mem_tombstones": active_tombs,
                "l0_files": ts["l0_files"].astype(int),
                "put_p99_us": ts["put_p99"].round(1),
                "is_slowdown": (ts["l0_files"] >= 20).astype(int)
            })

        df_t0_csv = process_run(r1_path, "T0")
        df_t64_csv = process_run(r3_path, "T64")
        df_combined = pd.concat([df_t0_csv, df_t64_csv], ignore_index=True)
        os.makedirs(SOURCE_DIR, exist_ok=True)
        df_combined.to_csv(csv_file, index=False)

    df = pd.read_csv(csv_file)
    df_t0 = df[df["config"] == "T0"].copy()
    df_t64 = df[df["config"] == "T64"].copy()

    # Calculate slowdown intervals from T64
    slowdown_rows = df_t64[df_t64["is_slowdown"] == 1].index.tolist()
    slowdown_intervals = []
    curr_start = None
    curr_end = None
    for idx in slowdown_rows:
        p_end = df_t64.loc[idx, "progress_pct"]
        prev_p = df_t64.loc[idx - 1, "progress_pct"] if idx > df_t64.index[0] else 0.0
        if curr_start is None:
            curr_start = prev_p
            curr_end = p_end
        else:
            if idx - 1 in slowdown_rows:
                curr_end = p_end
            else:
                slowdown_intervals.append((curr_start, curr_end))
                curr_start = prev_p
                curr_end = p_end
    if curr_start is not None:
        slowdown_intervals.append((curr_start, curr_end))

    # Fonts
    times_95 = FontProperties(fname=TIMES_TTF, size=9.5)
    times_85 = FontProperties(fname=TIMES_TTF, size=8.5)
    times_105 = FontProperties(fname=TIMES_TTF, size=10.5)
    timesbd_105 = FontProperties(fname=TIMESBD_TTF, size=10.5)

    simsun_95 = FontProperties(fname=SIMSUN_TTF, size=9.5)
    simsun_85 = FontProperties(fname=SIMSUN_TTF, size=8.5)
    simsun_105 = FontProperties(fname=SIMSUN_TTF, size=10.5)

    # Dimensions: 16.0cm x 14.0cm => (6.299 x 5.512 inches)
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(6.30, 5.51), sharex=True, dpi=300)

    c_t0 = "#56B4E9"   # Sky Blue
    c_t64 = "#0072B2"  # Deep Blue
    c_sd = "#D9D9D9"   # Light gray slowdown
    c_th = "#666666"   # Slowdown threshold line

    def set_mixed_title_5(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp)) for t, fp in parts]
        t_packer = HPacker(children=text_areas, align="baseline", pad=0, sep=0)
        anchored_t = AnchoredOffsetbox(loc="lower left", child=t_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.0, 1.02), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_t)

    def set_mixed_ylabel_5(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp, rotation=90)) for t, fp in parts]
        y_packer = VPacker(children=text_areas[::-1], align="baseline", pad=0, sep=0)
        anchored_y = AnchoredOffsetbox(loc="center right", child=y_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(-0.11, 0.5), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_y)

    def set_mixed_xlabel_5(ax, parts):
        text_areas = [TextArea(t, textprops=dict(fontproperties=fp)) for t, fp in parts]
        x_packer = HPacker(children=text_areas, align="baseline", pad=0, sep=0)
        anchored_x = AnchoredOffsetbox(loc="upper center", child=x_packer, pad=0.0, frameon=False,
                                       bbox_to_anchor=(0.5, -0.26), bbox_transform=ax.transAxes, borderpad=0.0)
        ax.add_artist(anchored_x)

    phase_bounds = [
        (0.0, 16.25, "Phase A"),
        (16.25, 39.02, "Phase B"),
        (39.02, 71.56, "Phase C"),
        (71.56, 100.0, "Phase D")
    ]

    for ax in [ax1, ax2, ax3]:
        for s, e in slowdown_intervals:
            ax.axvspan(s, e, color=c_sd, alpha=0.45, zorder=0, lw=0)
        
        for pb in [16.25, 39.02, 71.56]:
            ax.axvline(pb, color="#CCCCCC", linestyle=":", linewidth=0.8, zorder=1)
        
        ax.grid(True, which="major", linestyle="--", linewidth=0.6, color="#E0E0E0", alpha=0.8, zorder=1)
        ax.grid(False, which="minor")
        ax.set_xlim(0, 100)

    # Top Phase Labels with dedicated space
    for s, e, lbl in phase_bounds:
        center = (s + e) / 2.0
        t_area = TextArea(lbl, textprops=dict(fontproperties=times_85, color="#333333"))
        a_box = AnchoredOffsetbox(loc="lower center", child=t_area, pad=0.0, frameon=False,
                                  bbox_to_anchor=(center / 100.0, 1.20), bbox_transform=ax1.transAxes, borderpad=0.0)
        ax1.add_artist(a_box)

    # --- Subplot (a): 活跃MemTable范围墓碑数 ---
    y_t0_a = np.maximum(df_t0["active_mem_tombstones"].values, 1.0)
    y_t64_a = np.maximum(df_t64["active_mem_tombstones"].values, 1.0)

    ax1.plot(df_t0["progress_pct"], y_t0_a, color=c_t0, linestyle="--", linewidth=1.8, zorder=3)
    ax1.plot(df_t64["progress_pct"], y_t64_a, color=c_t64, linestyle="-", linewidth=2.0, zorder=3)

    ax1.set_yscale("log")
    ax1.set_ylim(0.8, 4000)
    ax1.set_yticks([1, 10, 100, 1000])
    ax1.set_yticklabels(["0", "10", "100", "1000"])
    for lbl in ax1.get_yticklabels():
        lbl.set_fontproperties(times_85)

    set_mixed_title_5(ax1, [("(a) ", timesbd_105), ("活跃", simsun_105), ("MemTable", times_105), ("范围墓碑数", simsun_105)])
    set_mixed_ylabel_5(ax1, [("范围墓碑数", simsun_95), ("(条，对数坐标)", simsun_95)])

    # Legend for (a)
    da1 = DrawingArea(15, 8, 0, 0)
    l1 = Line2D([0, 14], [4, 4], color=c_t0, linestyle="--", linewidth=1.8)
    da1.add_artist(l1)
    t1_en = TextArea("T0", textprops=dict(fontproperties=times_85))
    t1_cn = TextArea("(原生默认)", textprops=dict(fontproperties=simsun_85))
    row1 = HPacker(children=[da1, t1_en, t1_cn], align="center", pad=0, sep=2.0)

    da2 = DrawingArea(15, 8, 0, 0)
    l2 = Line2D([0, 14], [4, 4], color=c_t64, linestyle="-", linewidth=2.0)
    da2.add_artist(l2)
    t2_en = TextArea("T64", textprops=dict(fontproperties=times_85))
    t2_cn = TextArea("(固定阈值下刷)", textprops=dict(fontproperties=simsun_85))
    row2 = HPacker(children=[da2, t2_en, t2_cn], align="center", pad=0, sep=2.0)

    box1 = VPacker(children=[row1, row2], align="left", pad=1.5, sep=2.0)
    anchored_box1 = AnchoredOffsetbox(loc="upper left", child=box1, pad=0.15, frameon=True,
                                      bbox_to_anchor=(0.02, 0.94), bbox_transform=ax1.transAxes, borderpad=0.15)
    anchored_box1.patch.set_boxstyle("round,pad=0.15,rounding_size=0.1")
    anchored_box1.patch.set_facecolor("white")
    anchored_box1.patch.set_edgecolor("#D0D0D0")
    anchored_box1.patch.set_linewidth(0.6)
    anchored_box1.patch.set_alpha(0.92)
    ax1.add_artist(anchored_box1)

    # --- Subplot (b): L0文件数 ---
    ax2.plot(df_t0["progress_pct"], df_t0["l0_files"], color=c_t0, linestyle="--", linewidth=1.8, zorder=3)
    ax2.plot(df_t64["progress_pct"], df_t64["l0_files"], color=c_t64, linestyle="-", linewidth=2.0, zorder=3)
    ax2.axhline(20, color=c_th, linestyle="--", linewidth=1.0, zorder=2)

    ax2.set_ylim(0, 30)
    ax2.set_yticks([0, 10, 20, 30])
    for lbl in ax2.get_yticklabels():
        lbl.set_fontproperties(times_85)

    set_mixed_title_5(ax2, [("(b) L0", timesbd_105), ("文件数", simsun_105)])
    set_mixed_ylabel_5(ax2, [("L0", times_95), ("文件数", simsun_95), ("(个)", simsun_95)])

    # Legend for (b)
    da_b = DrawingArea(15, 8, 0, 0)
    l_b = Line2D([0, 14], [4, 4], color=c_th, linestyle="--", linewidth=1.0)
    da_b.add_artist(l_b)
    t_b1 = TextArea("Slowdown", textprops=dict(fontproperties=times_85))
    t_b2 = TextArea("阈值", textprops=dict(fontproperties=simsun_85))
    t_b3 = TextArea("(20)", textprops=dict(fontproperties=times_85))
    row_b = HPacker(children=[da_b, t_b1, t_b2, t_b3], align="center", pad=0, sep=1.5)

    anchored_box2 = AnchoredOffsetbox(loc="upper left", child=row_b, pad=0.15, frameon=True,
                                      bbox_to_anchor=(0.02, 0.94), bbox_transform=ax2.transAxes, borderpad=0.15)
    anchored_box2.patch.set_boxstyle("round,pad=0.15,rounding_size=0.1")
    anchored_box2.patch.set_facecolor("white")
    anchored_box2.patch.set_edgecolor("#D0D0D0")
    anchored_box2.patch.set_linewidth(0.6)
    anchored_box2.patch.set_alpha(0.92)
    ax2.add_artist(anchored_box2)

    # --- Subplot (c): Put P99延迟 ---
    ax3.plot(df_t0["progress_pct"], df_t0["put_p99_us"], color=c_t0, linestyle="--", linewidth=1.8, zorder=3)
    ax3.plot(df_t64["progress_pct"], df_t64["put_p99_us"], color=c_t64, linestyle="-", linewidth=2.0, zorder=3)

    ax3.set_yscale("log")
    ax3.set_ylim(8, 25000)
    ax3.set_yticks([10, 100, 1000, 10000])
    ax3.set_yticklabels(["10", "100", "1000", "10000"])
    for lbl in ax3.get_yticklabels():
        lbl.set_fontproperties(times_85)

    set_mixed_title_5(ax3, [("(c) Put P99", timesbd_105), ("延迟", simsun_105)])
    set_mixed_ylabel_5(ax3, [("Put P99", times_95), ("延迟", simsun_95), ("(μs", times_95), ("，对数坐标)", simsun_95)])

    # X-ticks on ax3
    ax3.set_xticks([0, 20, 40, 60, 80, 100])
    ax3.set_xticklabels(["0%", "20%", "40%", "60%", "80%", "100%"])
    for lbl in ax3.get_xticklabels():
        lbl.set_fontproperties(times_85)

    set_mixed_xlabel_5(ax3, [("前台执行进度", simsun_95), ("(%)", times_95)])

    # Legend for (c)
    da_c = DrawingArea(10, 8, 0, 0)
    r_c = Rectangle((0, 1), 9, 6, facecolor=c_sd, edgecolor="#A0A0A0", lw=0.6)
    da_c.add_artist(r_c)
    t_c1 = TextArea("Write Slowdown", textprops=dict(fontproperties=times_85))
    t_c2 = TextArea("窗口", textprops=dict(fontproperties=simsun_85))
    row_c = HPacker(children=[da_c, t_c1, t_c2], align="center", pad=0, sep=1.0)

    anchored_box3 = AnchoredOffsetbox(loc="upper right", child=row_c, pad=0.15, frameon=True,
                                      bbox_to_anchor=(0.98, 0.94), bbox_transform=ax3.transAxes, borderpad=0.15)
    anchored_box3.patch.set_boxstyle("round,pad=0.15,rounding_size=0.1")
    anchored_box3.patch.set_facecolor("white")
    anchored_box3.patch.set_edgecolor("#D0D0D0")
    anchored_box3.patch.set_linewidth(0.6)
    anchored_box3.patch.set_alpha(0.92)
    ax3.add_artist(anchored_box3)

    plt.subplots_adjust(top=0.86, bottom=0.10, left=0.14, right=0.96, hspace=0.20)

    fig_base = os.path.join(BASE_DIR, "fig4-5-longcycle-lifecycle")
    plt.savefig(f"{fig_base}.svg", format="svg", bbox_inches="tight", facecolor="white")
    plt.savefig(f"{fig_base}.pdf", format="pdf", bbox_inches="tight", facecolor="white")
    plt.savefig(f"{fig_base}.png", format="png", dpi=600, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("✓ Figure 4-5 generated.")


# ==============================================================================
# Grayscale Preview Verification
# ==============================================================================
def generate_grayscale_previews():
    """Convert PNGs to grayscale to verify contrast for printing."""
    from PIL import Image
    for fig_name in [
        "fig4-1-range-tombstone-mechanism",
        "fig4-2-delete-ratio",
        "fig4-3-threshold-tradeoff",
        "fig4-4-scale-validation",
        "fig4-5-longcycle-lifecycle"
    ]:
        png_path = os.path.join(BASE_DIR, f"{fig_name}.png")
        if os.path.exists(png_path):
            img = Image.open(png_path).convert("L")
            gray_path = os.path.join(BASE_DIR, f"{fig_name}-gray.png")
            img.save(gray_path)
    print("✓ Grayscale preview images generated.")


# ==============================================================================
# Manifest & Audit Generator
# ==============================================================================
def generate_manifest_and_captions():
    """Generate plots-manifest.json and captions.md."""
    script_path = os.path.abspath(__file__)
    script_sha = compute_sha256(script_path)

    manifest = {
        "metadata": {
            "generator": "plot_opening_figures.py",
            "generator_sha256": script_sha,
            "generated_at": "2026-08-24T14:00:00+08:00",
            "dpi": 600,
            "color_system": "Okabe-Ito (colorblind safe)",
            "font_config": {
                "chinese": "SimSun (宋体)",
                "english": "Times New Roman"
            }
        },
        "figures": [
            {
                "figure_id": "fig4-1",
                "title": "高密度RangeDelete下范围墓碑压力形成及其读写维护权衡",
                "type": "qualitative_mechanism_flowchart",
                "inputs": [],
                "outputs": [
                    "results/plots/opening/fig4-1-range-tombstone-mechanism.svg",
                    "results/plots/opening/fig4-1-range-tombstone-mechanism.pdf",
                    "results/plots/opening/fig4-1-range-tombstone-mechanism.png"
                ],
                "sample_size_N": "N/A (定性图)",
                "error_bar_metric": "N/A",
                "log_scale": False
            },
            {
                "figure_id": "fig4-2",
                "title": "不同DeleteRange比例下的前台吞吐与读取开销",
                "type": "3_subplot_bar_chart",
                "inputs": [
                    {
                        "path": "results/plots/opening/source-data/fig4-2-delete-ratio.csv",
                        "sha256": compute_sha256(os.path.join(SOURCE_DIR, "fig4-2-delete-ratio.csv"))
                    }
                ],
                "outputs": [
                    "results/plots/opening/fig4-2-delete-ratio.svg",
                    "results/plots/opening/fig4-2-delete-ratio.pdf",
                    "results/plots/opening/fig4-2-delete-ratio.png"
                ],
                "sample_size_N": 3,
                "error_bar_metric": "样本标准差 (Sample Standard Deviation, ddof=1)",
                "log_scale": True,
                "log_scale_axes": ["(a) 总体吞吐", "(b) Scan单位有效键开销", "(c) Get(Control)P99延迟"]
            },
            {
                "figure_id": "fig4-3",
                "title": "不同固定RangeDelete数量阈值下的读取尾延迟、写入尾延迟与前台阶段引擎输出写放大",
                "type": "3_subplot_bar_chart",
                "inputs": [
                    {
                        "path": "results/plots/opening/source-data/fig4-3-threshold-tradeoff.csv",
                        "sha256": compute_sha256(os.path.join(SOURCE_DIR, "fig4-3-threshold-tradeoff.csv"))
                    }
                ],
                "outputs": [
                    "results/plots/opening/fig4-3-threshold-tradeoff.svg",
                    "results/plots/opening/fig4-3-threshold-tradeoff.pdf",
                    "results/plots/opening/fig4-3-threshold-tradeoff.png"
                ],
                "sample_size_N": 5,
                "error_bar_metric": "样本标准差 (Sample Standard Deviation, ddof=1)",
                "log_scale": True,
                "log_scale_axes": ["(a) Scan P99延迟", "(b) Put P99延迟"],
                "linear_scale_axes": ["(c) 前台阶段引擎输出写放大"]
            },
            {
                "figure_id": "fig4-4",
                "title": "不同数据规模下T0与T512的前台性能对比",
                "type": "3_subplot_grouped_bar_chart",
                "inputs": [
                    {
                        "path": "results/plots/opening/source-data/fig4-4-scale-validation.csv",
                        "sha256": compute_sha256(os.path.join(SOURCE_DIR, "fig4-4-scale-validation.csv"))
                    }
                ],
                "outputs": [
                    "results/plots/opening/fig4-4-scale-validation.svg",
                    "results/plots/opening/fig4-4-scale-validation.pdf",
                    "results/plots/opening/fig4-4-scale-validation.png"
                ],
                "sample_size_N": 5,
                "error_bar_metric": "样本标准差 (Sample Standard Deviation, ddof=1)",
                "log_scale": True,
                "log_scale_axes": ["(a) Trace IOPS", "(b) Scan P99延迟", "(c) Get(Deleted) P99延迟"]
            },
            {
                "figure_id": "fig4-5",
                "title": "20GiB动态混合负载下一次探索性全生命周期运行中范围墓碑、L0文件及Put P99延迟的演化",
                "type": "3_subplot_shared_x_timeline",
                "inputs": [
                    {
                        "path": "results/plots/opening/source-data/fig4-5-longcycle-lifecycle.csv",
                        "sha256": compute_sha256(os.path.join(SOURCE_DIR, "fig4-5-longcycle-lifecycle.csv"))
                    }
                ],
                "outputs": [
                    "results/plots/opening/fig4-5-longcycle-lifecycle.svg",
                    "results/plots/opening/fig4-5-longcycle-lifecycle.pdf",
                    "results/plots/opening/fig4-5-longcycle-lifecycle.png"
                ],
                "sample_size_N": 1,
                "error_bar_metric": "N/A (单次全生命周期连续采样)",
                "log_scale": True,
                "log_scale_axes": ["(a) 活跃MemTable范围墓碑数", "(c) Put P99延迟"],
                "linear_scale_axes": ["(b) L0文件数"]
            }
        ]
    }

    manifest_path = os.path.join(BASE_DIR, "plots-manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print("✓ plots-manifest.json written.")

    captions_content = """# 开题报告科研图注与答辩要点归档 (captions.md)

本文件收录 5 幅正式开题报告插图的完整图注（Word 报告排版用）与答辩 PPT 一句话结论。

---

## 图4-1 高密度RangeDelete下范围墓碑压力形成及其读写维护权衡

### Word 开题报告完整图注
> **图4-1 高密度RangeDelete下范围墓碑压力形成及其读写维护权衡**。图中展示了小 Value 负载下高密度范围删除引发读退化的完整因果链路（主路径，浅橙色区域），以及提前触发 Flush 进行物理回收时的读写维护权衡代价（主动维护路径与维护代价，浅蓝及红灰区域）。底部给出了该问题的适用边界说明。

### PPT 答辩一句话结论
> **范围墓碑在活跃 MemTable 中的长期滞留是导致读退化的根源，而无节制的提前 Flush 会将读压力转化为剧烈的写放大与写停顿，必须寻求读写权衡的自适应调控机制。**

---

## 图4-2 不同DeleteRange比例下的前台吞吐与读取开销

### Word 开题报告完整图注
> **图4-2 不同DeleteRange比例下的前台吞吐与读取开销**。实验采用 500,000 个 Key、256B Value 和 200,000 个逻辑操作，每组独立重复 3 次，误差棒表示样本标准差（$N=3$）。（a）总体吞吐、（b）Scan 单位有效键开销与（c）Get(Control) P99 尾延迟的纵轴均使用对数坐标标定。该实验对应高密度 RangeDelete 且普通写入不足以触发自然 Flush 的压力场景。

### PPT 答辩一句话结论
> **随着 DeleteRange 比例从 0% 上升至 10%，前台吞吐暴跌约 233.7 倍，Scan 单位有效键开销恶化约 370.8 倍，点查尾延迟从 19.01 μs 攀升至 14.73 ms，证实了高密度范围删除带来的灾难性读退化。**

---

## 图4-3 不同固定RangeDelete数量阈值下的读取尾延迟、写入尾延迟与前台阶段引擎输出写放大

### Word 开题报告完整图注
> **图4-3 不同固定RangeDelete数量阈值下的读取尾延迟、写入尾延迟与前台阶段引擎输出写放大**。数据来自 Formal V2 的 500K 主基线矩阵，每组独立重复 5 次，误差棒表示样本标准差（$N=5$）。子图（a）Scan P99 延迟与（b）Put P99 延迟纵轴采用对数坐标，（c）前台阶段引擎输出写放大纵轴采用线性坐标。注：T0 的 0 仅表示前台基线窗口内未产生 Flush/Compaction 引擎输出，不代表不存在 WAL 或设备层物理写入。

### PPT 答辩一句话结论
> **激进阈值（T64）虽然能降低读延迟，但会引发 10.01 ms 的严重写停顿与高达 50.7x 的写放大；适中阈值（T512）在读延迟（Scan P99: 304 μs, Put P99: 99 μs）与写放大（12.9x）间取得了较好的阶段性平衡，但固定阈值在多变负载下仍缺乏弹性。**

---

## 图4-4 不同数据规模下T0与T512的前台性能对比

### Word 开题报告完整图注
> **图4-4 不同数据规模下T0与T512的前台性能对比**。500K 和 24GiB 实验均采用确定性 Trace 及全库终态摘要校验，每组独立重复 5 次，误差棒表示样本标准差（$N=5$）。子图（a）Trace IOPS、（b）Scan P99 延迟与（c）Get(Deleted) P99 延迟纵轴均采用对数坐标。

### PPT 答辩一句话结论
> **在 500K 与 24GiB 两种量级的严谨实测中，固定阈值下刷策略（T512）相较默认基准（T0）在前台吞吐上分别提升了 11.0 倍和 9.3 倍，Scan P99 分别改善 20.8 倍和 5.3 倍，证明了及时下刷墓碑在不同数据规模下的通用有效性。**

---

## 图4-5 20GiB动态混合负载下一次探索性全生命周期运行中范围墓碑、L0文件及Put P99延迟的演化

### Word 开题报告完整图注
> **图4-5 20GiB动态混合负载下一次探索性全生命周期运行中范围墓碑、L0文件及Put P99延迟的演化**。该实验基于 LongCycle-20GiB 单次探索性运行（$N=1$）的 5 秒连续采样原始数据。横轴按前台已完成操作数除以前台总操作数归一化为执行进度（0%～100%），Phase A、B、C、D 分界线位置严格对应负载阶段转换点。子图（a）展示活跃 MemTable 范围墓碑数（零值仅用于对数坐标绘图时映射为 1），（b）展示 L0 文件数及 Slowdown 阈值线（L0=20），（c）展示 Put P99 延迟，浅灰色竖向阴影标示了 T64 触发 Write Slowdown 的观察窗口。

### PPT 答辩一句话结论
> **在 20GiB 全生命周期运行中，原生默认（T0）活跃 MemTable 范围墓碑大量堆积，而固定阈值下刷（T64）虽然将墓碑数控制在低位，但因触发密集 Flush 导致 L0 文件数突破 Slowdown 阈值（20），造成阶段性写停顿与 Put P99 尾延迟剧增（达 6.57 ms），揭示了全生命周期自适应调控的必要性。**
"""
    captions_path = os.path.join(BASE_DIR, "captions.md")
    with open(captions_path, "w", encoding="utf-8") as f:
        f.write(captions_content)
    print("✓ captions.md written.")


def main():
    print("==================================================")
    print("Generating Master Thesis Opening Report Figures...")
    print("==================================================")
    plot_figure_4_1()
    plot_figure_4_2()
    plot_figure_4_3()
    plot_figure_4_4()
    plot_figure_4_5()
    generate_grayscale_previews()
    generate_manifest_and_captions()
    print("==================================================")
    print("All figures and metadata generated successfully.")
    print("==================================================")


if __name__ == "__main__":
    main()

