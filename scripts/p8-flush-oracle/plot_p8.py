#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for P8 Flush Oracle experiment suite.
Generates vector charts for Phase A/B/C comparisons and progress recovery curves.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary", "p8-flush-oracle")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "p8-flush-oracle")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "all-runs.csv")
TS_PROGRESS_CSV = os.path.join(SUMMARY_DIR, "timeseries-progress.csv")

def generate_p8_summary_svg(df):
    df['group'] = df['exp_id'].str.replace(r'-r\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    pairs = [
        ('p8-default-ratio-020', 'p8-flush-ratio-020', '2.0%'),
        ('p8-default-ratio-050', 'p8-flush-ratio-050', '5.0%'),
        ('p8-default-ratio-100', 'p8-flush-ratio-100', '10.0%'),
    ]

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 400" width="100%" height="400" style="background:#0f172a; font-family:sans-serif;">
      <text x="480" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P8: Range Tombstone Pressure Flush Oracle Validation</text>
      
      <!-- Panel 1: Phase C Scan Cost (μs/key) Comparison -->
      <g transform="translate(50, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Phase C Scan Cost (μs/key) [Post-Flush Phase]</text>
        <line x1="30" y1="250" x2="380" y2="250" stroke="#64748b"/>
        
        <!-- Legend -->
        <rect x="110" y="45" width="15" height="10" fill="#ef4444" rx="2"/>
        <text x="130" y="54" fill="#e2e8f0" font-size="10">Default (No Flush)</text>
        <rect x="240" y="45" width="15" height="10" fill="#10b981" rx="2"/>
        <text x="260" y="54" fill="#e2e8f0" font-size="10">Oracle Flush</text>
"""
    for i, (def_k, fl_k, lbl) in enumerate(pairs):
        def_cost = grouped.loc[def_k, 'phase_c_scan_us_per_key'] if def_k in grouped.index else 0
        fl_cost = grouped.loc[fl_k, 'phase_c_scan_us_per_key'] if fl_k in grouped.index else 0

        x_base = 60 + i * 110
        # scale max 350
        h_def = min(170, (def_cost / 350.0) * 170)
        h_fl = min(170, (fl_cost / 350.0) * 170)

        svg += f"""
        <!-- Pair {lbl} -->
        <rect x="{x_base}" y="{250 - h_def:.1f}" width="35" height="{h_def:.1f}" fill="#ef4444" rx="3"/>
        <text x="{x_base + 17}" y="{240 - h_def:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{def_cost:.1f}</text>

        <rect x="{x_base + 40}" y="{250 - h_fl:.1f}" width="35" height="{h_fl:.1f}" fill="#10b981" rx="3"/>
        <text x="{x_base + 57}" y="{240 - h_fl:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{fl_cost:.1f}</text>

        <text x="{x_base + 37}" y="270" fill="#94a3b8" font-size="11" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += f"""
      </g>

      <!-- Panel 2: Phase C IOPS Comparison -->
      <g transform="translate(500, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Phase C IOPS (kOps/s) [Post-Flush Recovery]</text>
        <line x1="30" y1="250" x2="380" y2="250" stroke="#64748b"/>
        
        <!-- Legend -->
        <rect x="110" y="45" width="15" height="10" fill="#ef4444" rx="2"/>
        <text x="130" y="54" fill="#e2e8f0" font-size="10">Default (No Flush)</text>
        <rect x="240" y="45" width="15" height="10" fill="#10b981" rx="2"/>
        <text x="260" y="54" fill="#e2e8f0" font-size="10">Oracle Flush</text>
"""
    for i, (def_k, fl_k, lbl) in enumerate(pairs):
        def_iops = (grouped.loc[def_k, 'phase_c_iops'] / 1e3) if def_k in grouped.index else 0
        fl_iops = (grouped.loc[fl_k, 'phase_c_iops'] / 1e3) if fl_k in grouped.index else 0

        x_base = 60 + i * 110
        # scale max 50k
        h_def = min(170, (def_iops / 50.0) * 170)
        h_fl = min(170, (fl_iops / 50.0) * 170)

        svg += f"""
        <!-- Pair {lbl} -->
        <rect x="{x_base}" y="{250 - h_def:.1f}" width="35" height="{h_def:.1f}" fill="#ef4444" rx="3"/>
        <text x="{x_base + 17}" y="{240 - h_def:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{def_iops:.1f}k</text>

        <rect x="{x_base + 40}" y="{250 - h_fl:.1f}" width="35" height="{h_fl:.1f}" fill="#10b981" rx="3"/>
        <text x="{x_base + 57}" y="{240 - h_fl:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{fl_iops:.1f}k</text>

        <text x="{x_base + 37}" y="270" fill="#94a3b8" font-size="11" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += """
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p8_phase_comparison.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P8 Phase Comparison SVG plot to {out_file}")

def generate_p8_progress_svg():
    if not os.path.exists(TS_PROGRESS_CSV):
        return
    df = pd.read_csv(TS_PROGRESS_CSV)
    if df.empty:
        return

    df['config_base'] = df['exp_id'].str.replace(r'-r\d+$', '', regex=True)
    grouped = df.groupby(['config_base', 'progress_pct_idx']).mean(numeric_only=True).reset_index()

    targets = [
        ('p8-default-ratio-100', 'p8-flush-ratio-100', '10.0% Group'),
        ('p8-default-ratio-050', 'p8-flush-ratio-050', '5.0% Group')
    ]

    def make_poly(sub_df, field, y_min, y_max, h=160, w=340, x_off=30, y_off=200):
        pts = []
        for _, row in sub_df.iterrows():
            prog = row['progress_pct_idx']
            val = row[field]
            x = x_off + (prog / 100.0) * w
            y = y_off - ((val - y_min) / (y_max - y_min + 1e-6)) * h
            pts.append(f"{x:.1f},{y:.1f}")
        return " ".join(pts)

    sub_def_10 = grouped[grouped['config_base'] == 'p8-default-ratio-100'].sort_values('progress_pct_idx')
    sub_fl_10 = grouped[grouped['config_base'] == 'p8-flush-ratio-100'].sort_values('progress_pct_idx')

    poly_def_10_scan = make_poly(sub_def_10, 'scan_us_per_key', 0.0, 400.0)
    poly_fl_10_scan = make_poly(sub_fl_10, 'scan_us_per_key', 0.0, 400.0)

    poly_def_10_iops = make_poly(sub_def_10, 'instantaneous_iops', 0.0, 10000.0)
    poly_fl_10_iops = make_poly(sub_fl_10, 'instantaneous_iops', 0.0, 10000.0)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 360" width="100%" height="360" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P8: 10% DeleteRange Progress Trajectory (Default vs Oracle Flush at 50% Progress)</text>
      
      <!-- Panel 1: Scan Cost (μs/key) vs Progress -->
      <g transform="translate(50, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Progress</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <!-- Flush Marker at 50% (x = 30 + 50/100*340 = 200) -->
        <line x1="200" y1="40" x2="200" y2="200" stroke="#10b981" stroke-dasharray="3"/>
        <text x="200" y="35" fill="#10b981" font-size="9" text-anchor="middle">Flush Trigger</text>

        <polyline fill="none" stroke="#ef4444" stroke-width="2" points="{poly_def_10_scan}"/>
        <polyline fill="none" stroke="#10b981" stroke-width="2" points="{poly_fl_10_scan}"/>

        <!-- Legend -->
        <circle cx="60" cy="235" r="4" fill="#ef4444"/>
        <text x="70" y="238" fill="#e2e8f0" font-size="9">Default (No Flush)</text>
        <circle cx="200" cy="235" r="4" fill="#10b981"/>
        <text x="210" y="238" fill="#e2e8f0" font-size="9">Oracle Flush</text>
      </g>

      <!-- Panel 2: Instantaneous IOPS vs Progress -->
      <g transform="translate(470, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Instantaneous IOPS vs Progress</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <line x1="200" y1="40" x2="200" y2="200" stroke="#10b981" stroke-dasharray="3"/>
        <text x="200" y="35" fill="#10b981" font-size="9" text-anchor="middle">Flush Trigger</text>

        <polyline fill="none" stroke="#ef4444" stroke-width="2" points="{poly_def_10_iops}"/>
        <polyline fill="none" stroke="#10b981" stroke-width="2" points="{poly_fl_10_iops}"/>

        <!-- Legend -->
        <circle cx="60" cy="235" r="4" fill="#ef4444"/>
        <text x="70" y="238" fill="#e2e8f0" font-size="9">Default (No Flush)</text>
        <circle cx="200" cy="235" r="4" fill="#10b981"/>
        <text x="210" y="238" fill="#e2e8f0" font-size="9">Oracle Flush</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p8_progress_trajectory.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P8 Progress Trajectory SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_p8_summary_svg(df)
    generate_p8_progress_svg()
