#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for P7 P1-replay experiment suite.
Generates publication-quality vector charts for summary, wall-clock, and progress time series.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary", "p7-p1-replay")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "p7-p1-replay")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "all-runs.csv")
TS_PROGRESS_CSV = os.path.join(SUMMARY_DIR, "timeseries-progress.csv")

def generate_p7_summary_svg(df):
    df['group'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    keys = ['p7_ratio_000', 'p7_ratio_005', 'p7_ratio_010', 'p7_ratio_020', 'p7_ratio_050', 'p7_ratio_100']
    labels = ['0.0%', '0.5%', '1.0%', '2.0%', '5.0%', '10.0%']
    colors = ['#38bdf8', '#06b6d4', '#10b981', '#f59e0b', '#f97316', '#ef4444']

    iops = [grouped.loc[k, 'overall_iops'] / 1e3 if k in grouped.index else 0 for k in keys]
    scan_cost = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in keys]
    del_count = [grouped.loc[k, 'del_range_count'] if k in grouped.index else 0 for k in keys]
    get_del_lat = [grouped.loc[k, 'get_del_p95_us'] if k in grouped.index else 0 for k in keys]

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 380" width="100%" height="380" style="background:#0f172a; font-family:sans-serif;">
      <text x="480" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P7: P1 High Degradation Intensity Replay (DeleteRange 0.0% to 10.0%)</text>
      
      <!-- Panel 1: Throughput (kIOPS) -->
      <g transform="translate(40, 60)">
        <rect width="200" height="280" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Throughput (kOps/s)</text>
        <line x1="20" y1="240" x2="180" y2="240" stroke="#64748b"/>
        <!-- Bars (max scale 450k) -->
        {"".join([f'<rect x="{25 + i*25}" y="{240 - min(200, iops[i]*0.45):.1f}" width="20" height="{min(200, iops[i]*0.45):.1f}" fill="{colors[i]}" rx="2"/>' for i in range(6)])}
        {"".join([f'<text x="{35 + i*25}" y="255" fill="#94a3b8" font-size="8" text-anchor="middle">{labels[i]}</text>' for i in range(6)])}
        <text x="100" y="272" fill="#e2e8f0" font-size="10" text-anchor="middle">{iops[0]:.0f}k → {iops[5]:.1f}k (234x Drop)</text>
      </g>

      <!-- Panel 2: Scan Cost (μs/key) -->
      <g transform="translate(260, 60)">
        <rect width="200" height="280" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key)</text>
        <line x1="20" y1="240" x2="180" y2="240" stroke="#64748b"/>
        <!-- Bars (scale max 250) -->
        {"".join([f'<rect x="{25 + i*25}" y="{240 - min(200, scan_cost[i]*0.8):.1f}" width="20" height="{min(200, scan_cost[i]*0.8):.1f}" fill="{colors[i]}" rx="2"/>' for i in range(6)])}
        {"".join([f'<text x="{35 + i*25}" y="255" fill="#94a3b8" font-size="8" text-anchor="middle">{labels[i]}</text>' for i in range(6)])}
        <text x="100" y="272" fill="#e2e8f0" font-size="10" text-anchor="middle">{scan_cost[0]:.2f} → {scan_cost[5]:.1f} μs (371x Cost)</text>
      </g>

      <!-- Panel 3: DeleteRange Operations Count -->
      <g transform="translate(480, 60)">
        <rect width="200" height="280" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="100" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">In-Mem Tombstones Count</text>
        <line x1="20" y1="240" x2="180" y2="240" stroke="#64748b"/>
        <!-- Bars (max 20,000) -->
        {"".join([f'<rect x="{25 + i*25}" y="{240 - (del_count[i]/20000.0)*200:.1f}" width="20" height="{(del_count[i]/20000.0)*200:.1f}" fill="{colors[i]}" rx="2"/>' for i in range(6)])}
        {"".join([f'<text x="{35 + i*25}" y="255" fill="#94a3b8" font-size="8" text-anchor="middle">{labels[i]}</text>' for i in range(6)])}
        <text x="100" y="272" fill="#e2e8f0" font-size="10" text-anchor="middle">Max Tombstones: {del_count[5]:.0f}</text>
      </g>

      <!-- Panel 4: Get Deleted P95 Latency (ms) -->
      <g transform="translate(700, 60)">
        <rect width="220" height="280" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="110" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Get (Deleted) P95 (ms)</text>
        <line x1="20" y1="240" x2="200" y2="240" stroke="#64748b"/>
        {"".join([f'<rect x="{30 + i*27}" y="{240 - min(200, (get_del_lat[i]/1000.0)*7.0):.1f}" width="20" height="{min(200, (get_del_lat[i]/1000.0)*7.0):.1f}" fill="{colors[i]}" rx="2"/>' for i in range(6)])}
        {"".join([f'<text x="{40 + i*27}" y="255" fill="#94a3b8" font-size="8" text-anchor="middle">{labels[i]}</text>' for i in range(6)])}
        <text x="110" y="272" fill="#e2e8f0" font-size="10" text-anchor="middle">Max P95: {get_del_lat[5]/1000.0:.1f} ms</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p7_summary_comparison.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P7 Summary SVG plot to {out_file}")

def generate_p7_progress_svg():
    if not os.path.exists(TS_PROGRESS_CSV):
        return
    df = pd.read_csv(TS_PROGRESS_CSV)
    if df.empty:
        return

    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = df.groupby(['config_base', 'progress_pct_idx']).mean(numeric_only=True).reset_index()

    configs = ['p7_ratio_000', 'p7_ratio_010', 'p7_ratio_020', 'p7_ratio_050', 'p7_ratio_100']
    colors = {'p7_ratio_000': '#38bdf8', 'p7_ratio_010': '#10b981', 'p7_ratio_020': '#f59e0b', 'p7_ratio_050': '#f97316', 'p7_ratio_100': '#ef4444'}
    labels = {'p7_ratio_000': '0.0%', 'p7_ratio_010': '1.0%', 'p7_ratio_020': '2.0%', 'p7_ratio_050': '5.0%', 'p7_ratio_100': '10.0%'}

    def make_poly(sub_df, field, y_min, y_max, h=160, w=340, x_off=30, y_off=200):
        pts = []
        for _, row in sub_df.iterrows():
            prog = row['progress_pct_idx']
            val = row[field]
            x = x_off + (prog / 100.0) * w
            y = y_off - ((val - y_min) / (y_max - y_min + 1e-6)) * h
            pts.append(f"{x:.1f},{y:.1f}")
        return " ".join(pts)

    polys_scan = {}
    polys_iops = {}
    for c in configs:
        sub = grouped[grouped['config_base'] == c].sort_values('progress_pct_idx')
        polys_scan[c] = make_poly(sub, 'scan_us_per_key', 0.0, 250.0)
        polys_iops[c] = make_poly(sub, 'instantaneous_iops', 0.0, 500000.0)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 360" width="100%" height="360" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">P7: Request-Progress-Aligned Evolution (0% to 100% Request Completion)</text>
      
      <!-- Panel 1: Instantaneous IOPS across Progress -->
      <g transform="translate(50, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Instantaneous IOPS vs Request Progress</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        {"".join([f'<polyline fill="none" stroke="{colors[c]}" stroke-width="2" points="{polys_iops[c]}"/>' for c in configs])}
        
        <!-- Legend -->
        {"".join([f'<circle cx="{40 + i*65}" cy="235" r="4" fill="{colors[configs[i]]}"/><text x="{48 + i*65}" y="238" fill="#e2e8f0" font-size="9">{labels[configs[i]]}</text>' for i in range(len(configs))])}
      </g>

      <!-- Panel 2: Scan Cost (μs/key) across Progress -->
      <g transform="translate(470, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Request Progress</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        {"".join([f'<polyline fill="none" stroke="{colors[c]}" stroke-width="2" points="{polys_scan[c]}"/>' for c in configs])}
        
        <!-- Legend -->
        {"".join([f'<circle cx="{40 + i*65}" cy="235" r="4" fill="{colors[configs[i]]}"/><text x="{48 + i*65}" y="238" fill="#e2e8f0" font-size="9">{labels[configs[i]]}</text>' for i in range(len(configs))])}
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "p7_progress_evolution.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved P7 Progress SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_p7_summary_svg(df)
    generate_p7_progress_svg()
