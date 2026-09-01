#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for E3 Native Fixed Threshold Sweep tradeoff curves.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "e3")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "e3_all_runs.csv")

def generate_e3_summary_svg(df):
    df['group'] = df['exp_id'].str.replace(r'_r\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    workloads = ['get_heavy', 'scan_heavy', 'write_heavy']
    wl_titles = {'get_heavy': 'Get-heavy', 'scan_heavy': 'Scan-heavy', 'write_heavy': 'Write-heavy'}
    colors = {'get_heavy': '#38bdf8', 'scan_heavy': '#10b981', 'write_heavy': '#f59e0b'}
    thresholds = [0, 64, 128, 256, 512, 1024, 2048]
    t_labels = ['T=0', '64', '128', '256', '512', '1024', '2048']

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 400" width="100%" height="400" style="background:#0f172a; font-family:sans-serif;">
      <text x="480" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">E3: Native Fixed Threshold Sweep Tradeoff Curves Across Workloads</text>
      
      <!-- Panel 1: Scan Cost (μs/key) vs Threshold -->
      <g transform="translate(50, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Threshold</text>
        <line x1="40" y1="240" x2="380" y2="240" stroke="#64748b"/>
        <line x1="40" y1="40" x2="40" y2="240" stroke="#64748b"/>
"""
    for wl in workloads:
        pts = []
        for i, t in enumerate(thresholds):
            t_str = f"t{t:04d}" if t > 0 else "t0000"
            k = f"e3_{wl}_{t_str}"
            cost = grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0
            x = 50 + i * 48
            # scale max 250
            y = 240 - min(190, (cost / 250.0) * 190)
            pts.append(f"{x:.1f},{y:.1f}")
        svg += f'<polyline fill="none" stroke="{colors[wl]}" stroke-width="2" points="{" ".join(pts)}"/>'
        for pt in pts:
            x, y = pt.split(",")
            svg += f'<circle cx="{x}" cy="{y}" r="3.5" fill="{colors[wl]}"/>'

    for i, lbl in enumerate(t_labels):
        x = 50 + i * 48
        svg += f'<text x="{x}" y="258" fill="#94a3b8" font-size="9" text-anchor="middle">{lbl}</text>'

    svg += f"""
        <!-- Legend -->
        {"".join([f'<circle cx="{60 + i*110}" cy="285" r="4" fill="{colors[workloads[i]]}"/><text x="{70 + i*110}" y="288" fill="#e2e8f0" font-size="10">{wl_titles[workloads[i]]}</text>' for i in range(3)])}
      </g>

      <!-- Panel 2: Write Amplification (WA) vs Threshold -->
      <g transform="translate(500, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Write Amplification (WA) vs Threshold</text>
        <line x1="40" y1="240" x2="380" y2="240" stroke="#64748b"/>
        <line x1="40" y1="40" x2="40" y2="240" stroke="#64748b"/>
"""
    for wl in workloads:
        pts = []
        for i, t in enumerate(thresholds):
            t_str = f"t{t:04d}" if t > 0 else "t0000"
            k = f"e3_{wl}_{t_str}"
            wa = grouped.loc[k, 'write_amplification'] if k in grouped.index else 0
            x = 50 + i * 48
            # scale max 80
            y = 240 - min(190, (wa / 80.0) * 190)
            pts.append(f"{x:.1f},{y:.1f}")
        svg += f'<polyline fill="none" stroke="{colors[wl]}" stroke-width="2" points="{" ".join(pts)}"/>'
        for pt in pts:
            x, y = pt.split(",")
            svg += f'<circle cx="{x}" cy="{y}" r="3.5" fill="{colors[wl]}"/>'

    for i, lbl in enumerate(t_labels):
        x = 50 + i * 48
        svg += f'<text x="{x}" y="258" fill="#94a3b8" font-size="9" text-anchor="middle">{lbl}</text>'

    svg += f"""
        <!-- Legend -->
        {"".join([f'<circle cx="{60 + i*110}" cy="285" r="4" fill="{colors[workloads[i]]}"/><text x="{70 + i*110}" y="288" fill="#e2e8f0" font-size="10">{wl_titles[workloads[i]]}</text>' for i in range(3)])}
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "e3_threshold_tradeoff.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved E3 SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_e3_summary_svg(df)
