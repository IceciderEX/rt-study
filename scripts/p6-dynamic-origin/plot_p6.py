#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for P6 dynamic origin experiment.
Zero external dependencies, renders crisp vector SVG charts.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary", "p6-dynamic-origin")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "p6-dynamic-origin")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "all-runs.csv")
TIMESERIES_CSV = os.path.join(SUMMARY_DIR, "timeseries.csv")

def generate_p6_summary_svg(df):
    df['group'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    keys = ['p6_d0_clean', 'p6_d1_static', 'p6_d2_dynamic']
    labels = ['D0: Clean', 'D1: Static Tomb', 'D2: Dynamic Del']
    colors = ['#38bdf8', '#a855f7', '#f43f5e']

    iops = [grouped.loc[k, 'overall_iops'] / 1e3 if k in grouped.index else 0 for k in keys]
    scan_cost = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in keys]
    get_aff_p99 = [grouped.loc[k, 'get_aff_p99_us'] if k in grouped.index else 0 for k in keys]
    l0_files = [grouped.loc[k, 'l0_files_final'] if k in grouped.index else 0 for k in keys]

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 360" width="100%" height="360" style="background:#0f172a; font-family:sans-serif;">
      <text x="480" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P6: Dynamic Range Deletion Origin Localization (D0 vs D1 vs D2)</text>
      
      <!-- Panel 1: Throughput (kIOPS) -->
      <g transform="translate(40, 60)">
        <rect width="200" height="250" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Main IOPS (kOps/s)</text>
        <line x1="20" y1="210" x2="180" y2="210" stroke="#64748b"/>
        <!-- Bars (scale max 250k) -->
        <rect x="30" y="{210 - iops[0]*0.75:.1f}" width="35" height="{iops[0]*0.75:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="47" y="{200 - iops[0]*0.75:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{iops[0]:.1f}k</text>
        <text x="47" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D0</text>

        <rect x="82" y="{210 - iops[1]*0.75:.1f}" width="35" height="{iops[1]*0.75:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="99" y="{200 - iops[1]*0.75:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{iops[1]:.1f}k</text>
        <text x="99" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D1</text>

        <rect x="135" y="{210 - iops[2]*0.75:.1f}" width="35" height="{iops[2]*0.75:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="152" y="{200 - iops[2]*0.75:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{iops[2]:.1f}k</text>
        <text x="152" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D2</text>
      </g>

      <!-- Panel 2: Scan Cost (μs/key) -->
      <g transform="translate(260, 60)">
        <rect width="200" height="250" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key)</text>
        <line x1="20" y1="210" x2="180" y2="210" stroke="#64748b"/>
        <!-- Bars (scale max 5.0) -->
        <rect x="30" y="{210 - scan_cost[0]*35:.1f}" width="35" height="{scan_cost[0]*35:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="47" y="{200 - scan_cost[0]*35:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[0]:.2f}</text>
        <text x="47" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D0</text>

        <rect x="82" y="{210 - scan_cost[1]*35:.1f}" width="35" height="{scan_cost[1]*35:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="99" y="{200 - scan_cost[1]*35:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[1]:.2f}</text>
        <text x="99" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D1</text>

        <rect x="135" y="{210 - scan_cost[2]*35:.1f}" width="35" height="{scan_cost[2]*35:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="152" y="{200 - scan_cost[2]*35:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[2]:.2f}</text>
        <text x="152" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D2</text>
      </g>

      <!-- Panel 3: Get Affected P99 (μs) -->
      <g transform="translate(480, 60)">
        <rect width="200" height="250" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Get (Aff) P99 (μs)</text>
        <line x1="20" y1="210" x2="180" y2="210" stroke="#64748b"/>
        <!-- Bars (scale max 40.0) -->
        <rect x="30" y="{210 - get_aff_p99[0]*4.5:.1f}" width="35" height="{get_aff_p99[0]*4.5:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="47" y="{200 - get_aff_p99[0]*4.5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_aff_p99[0]:.1f}</text>
        <text x="47" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D0</text>

        <rect x="82" y="{210 - get_aff_p99[1]*4.5:.1f}" width="35" height="{get_aff_p99[1]*4.5:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="99" y="{200 - get_aff_p99[1]*4.5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_aff_p99[1]:.1f}</text>
        <text x="99" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D1</text>

        <rect x="135" y="{210 - get_aff_p99[2]*4.5:.1f}" width="35" height="{get_aff_p99[2]*4.5:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="152" y="{200 - get_aff_p99[2]*4.5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_aff_p99[2]:.1f}</text>
        <text x="152" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D2</text>
      </g>

      <!-- Panel 4: Final L0 Files -->
      <g transform="translate(700, 60)">
        <rect width="220" height="250" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="110" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Final L0 File Count</text>
        <line x1="20" y1="210" x2="200" y2="210" stroke="#64748b"/>
        <!-- Bars (scale max 10) -->
        <rect x="35" y="{210 - l0_files[0]*18:.1f}" width="35" height="{l0_files[0]*18:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="52" y="{200 - l0_files[0]*18:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{l0_files[0]:.0f}</text>
        <text x="52" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D0</text>

        <rect x="92" y="{210 - l0_files[1]*18:.1f}" width="35" height="{l0_files[1]*18:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="109" y="{200 - l0_files[1]*18:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{l0_files[1]:.0f}</text>
        <text x="109" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D1</text>

        <rect x="150" y="{210 - l0_files[2]*18:.1f}" width="35" height="{l0_files[2]*18:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="167" y="{200 - l0_files[2]*18:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{l0_files[2]:.0f}</text>
        <text x="167" y="225" fill="#94a3b8" font-size="9" text-anchor="middle">D2</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p6_summary_comparison.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P6 Summary SVG plot to {out_file}")

def generate_p6_timeseries_svg():
    if not os.path.exists(TIMESERIES_CSV):
        return
    ts_df = pd.read_csv(TIMESERIES_CSV)
    if ts_df.empty:
        return

    ts_df['config_base'] = ts_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = ts_df.groupby(['config_base', 'second_idx']).mean(numeric_only=True).reset_index()

    d0 = grouped[grouped['config_base'] == 'p6_d0_clean'].sort_values('second_idx')
    d1 = grouped[grouped['config_base'] == 'p6_d1_static'].sort_values('second_idx')
    d2 = grouped[grouped['config_base'] == 'p6_d2_dynamic'].sort_values('second_idx')

    def make_poly(sub_df, field, y_min, y_max, h=160, w=340, x_off=30, y_off=200):
        pts = []
        for _, row in sub_df.iterrows():
            sec = row['second_idx']
            val = row[field]
            x = x_off + (sec / 80.0) * w
            y = y_off - ((val - y_min) / (y_max - y_min + 1e-6)) * h
            pts.append(f"{x:.1f},{y:.1f}")
        return " ".join(pts)

    poly_d0_iops = make_poly(d0, 'overall_iops', 50000, 250000)
    poly_d1_iops = make_poly(d1, 'overall_iops', 50000, 250000)
    poly_d2_iops = make_poly(d2, 'overall_iops', 50000, 250000)

    poly_d0_scan = make_poly(d0, 'scan_us_per_key', 1.0, 4.5)
    poly_d1_scan = make_poly(d1, 'scan_us_per_key', 1.0, 4.5)
    poly_d2_scan = make_poly(d2, 'scan_us_per_key', 1.0, 4.5)

    poly_d0_mem = make_poly(d0, 'active_memtable_mb', 0, 70)
    poly_d1_mem = make_poly(d1, 'active_memtable_mb', 0, 70)
    poly_d2_mem = make_poly(d2, 'active_memtable_mb', 0, 70)

    poly_d0_l0 = make_poly(d0, 'l0_files', 0, 8)
    poly_d1_l0 = make_poly(d1, 'l0_files', 0, 8)
    poly_d2_l0 = make_poly(d2, 'l0_files', 0, 8)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 680" width="100%" height="680" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P6: 1-Second Time Series Dynamics (80s Total: 10s Warmup + 60s Main + 10s Cooldown)</text>
      
      <!-- Panel 1: Throughput (IOPS) -->
      <g transform="translate(50, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Frontend Throughput (IOPS)</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <!-- Markers: t=10s Main start (x = 30 + 10/80*340 = 72.5), t=50s D2 Del end (x = 30 + 50/80*340 = 242.5), t=70s Cooldown (x = 30 + 70/80*340 = 327.5) -->
        <line x1="72.5" y1="40" x2="72.5" y2="200" stroke="#64748b" stroke-dasharray="3"/>
        <line x1="242.5" y1="40" x2="242.5" y2="200" stroke="#f43f5e" stroke-dasharray="3"/>
        <line x1="327.5" y1="40" x2="327.5" y2="200" stroke="#64748b" stroke-dasharray="3"/>
        <text x="72.5" y="35" fill="#94a3b8" font-size="8" text-anchor="middle">Main</text>
        <text x="242.5" y="35" fill="#f43f5e" font-size="8" text-anchor="middle">Del End</text>

        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_d0_iops}"/>
        <polyline fill="none" stroke="#a855f7" stroke-width="2" points="{poly_d1_iops}"/>
        <polyline fill="none" stroke="#f43f5e" stroke-width="2" points="{poly_d2_iops}"/>

        <!-- Legend -->
        <circle cx="50" cy="235" r="4" fill="#38bdf8"/>
        <text x="60" y="238" fill="#e2e8f0" font-size="9">D0: Clean</text>
        <circle cx="150" cy="235" r="4" fill="#a855f7"/>
        <text x="160" y="238" fill="#e2e8f0" font-size="9">D1: Static Tomb</text>
        <circle cx="270" cy="235" r="4" fill="#f43f5e"/>
        <text x="280" y="238" fill="#e2e8f0" font-size="9">D2: Dynamic Del</text>
      </g>

      <!-- Panel 2: Scan Cost (μs/key) -->
      <g transform="translate(470, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">RangeScan Cost (μs/key)</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <line x1="72.5" y1="40" x2="72.5" y2="200" stroke="#64748b" stroke-dasharray="3"/>
        <line x1="242.5" y1="40" x2="242.5" y2="200" stroke="#f43f5e" stroke-dasharray="3"/>

        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_d0_scan}"/>
        <polyline fill="none" stroke="#a855f7" stroke-width="2" points="{poly_d1_scan}"/>
        <polyline fill="none" stroke="#f43f5e" stroke-width="2" points="{poly_d2_scan}"/>

        <!-- Legend -->
        <circle cx="50" cy="235" r="4" fill="#38bdf8"/>
        <text x="60" y="238" fill="#e2e8f0" font-size="9">D0: Clean</text>
        <circle cx="150" cy="235" r="4" fill="#a855f7"/>
        <text x="160" y="238" fill="#e2e8f0" font-size="9">D1: Static Tomb</text>
        <circle cx="270" cy="235" r="4" fill="#f43f5e"/>
        <text x="280" y="238" fill="#e2e8f0" font-size="9">D2: Dynamic Del</text>
      </g>

      <!-- Panel 3: Active MemTable Size (MB) -->
      <g transform="translate(50, 350)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Active MemTable Size (MB)</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <line x1="72.5" y1="40" x2="72.5" y2="200" stroke="#64748b" stroke-dasharray="3"/>
        <line x1="242.5" y1="40" x2="242.5" y2="200" stroke="#f43f5e" stroke-dasharray="3"/>

        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_d0_mem}"/>
        <polyline fill="none" stroke="#a855f7" stroke-width="2" points="{poly_d1_mem}"/>
        <polyline fill="none" stroke="#f43f5e" stroke-width="2" points="{poly_d2_mem}"/>

        <!-- Legend -->
        <circle cx="50" cy="235" r="4" fill="#38bdf8"/>
        <text x="60" y="238" fill="#e2e8f0" font-size="9">D0: Clean</text>
        <circle cx="150" cy="235" r="4" fill="#a855f7"/>
        <text x="160" y="238" fill="#e2e8f0" font-size="9">D1: Static Tomb</text>
        <circle cx="270" cy="235" r="4" fill="#f43f5e"/>
        <text x="280" y="238" fill="#e2e8f0" font-size="9">D2: Dynamic Del</text>
      </g>

      <!-- Panel 4: Level 0 Files Count -->
      <g transform="translate(470, 350)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Level 0 SST Files Count</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <line x1="72.5" y1="40" x2="72.5" y2="200" stroke="#64748b" stroke-dasharray="3"/>
        <line x1="242.5" y1="40" x2="242.5" y2="200" stroke="#f43f5e" stroke-dasharray="3"/>

        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_d0_l0}"/>
        <polyline fill="none" stroke="#a855f7" stroke-width="2" points="{poly_d1_l0}"/>
        <polyline fill="none" stroke="#f43f5e" stroke-width="2" points="{poly_d2_l0}"/>

        <!-- Legend -->
        <circle cx="50" cy="235" r="4" fill="#38bdf8"/>
        <text x="60" y="238" fill="#e2e8f0" font-size="9">D0: Clean</text>
        <circle cx="150" cy="235" r="4" fill="#a855f7"/>
        <text x="160" y="238" fill="#e2e8f0" font-size="9">D1: Static Tomb</text>
        <circle cx="270" cy="235" r="4" fill="#f43f5e"/>
        <text x="280" y="238" fill="#e2e8f0" font-size="9">D2: Dynamic Del</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p6_timeseries_dynamics.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P6 TimeSeries SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_p6_summary_svg(df)
    generate_p6_timeseries_svg()
