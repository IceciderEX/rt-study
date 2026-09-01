#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for E4 Time Series and Causality Boundary.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "e4")
os.makedirs(PLOTS_DIR, exist_ok=True)

TS_WALLCLOCK_CSV = os.path.join(SUMMARY_DIR, "e4_timeseries_wallclock.csv")

def generate_e4_summary_svg():
    if not os.path.exists(TS_WALLCLOCK_CSV):
        return
    df = pd.read_csv(TS_WALLCLOCK_CSV)
    if df.empty:
        return

    df['config_base'] = df['exp_id'].str.replace(r'_r\d+$', '', regex=True)
    grouped = df.groupby(['config_base', 'second_idx']).mean(numeric_only=True).reset_index()

    configs = ['e4_ts_t0064', 'e4_ts_t0256', 'e4_ts_t1024']
    colors = {'e4_ts_t0064': '#ef4444', 'e4_ts_t0256': '#10b981', 'e4_ts_t1024': '#38bdf8'}
    labels = {'e4_ts_t0064': 'T=64 (Aggressive)', 'e4_ts_t0256': 'T=256 (Medium)', 'e4_ts_t1024': 'T=1024 (Conservative)'}

    def make_poly(sub_df, field, y_min, y_max, h=160, w=340, x_off=30, y_off=200):
        pts = []
        max_sec = 20.0
        for _, row in sub_df.iterrows():
            sec = row['second_idx']
            val = row[field]
            x = x_off + min(w, (sec / max_sec) * w)
            y = y_off - min(h, ((val - y_min) / (y_max - y_min + 1e-6)) * h)
            pts.append(f"{x:.1f},{y:.1f}")
        return " ".join(pts)

    polys_scan = {}
    polys_mem = {}
    for c in configs:
        sub = grouped[grouped['config_base'] == c].sort_values('second_idx')
        polys_scan[c] = make_poly(sub, 'scan_us_per_key', 0.0, 50.0)
        polys_mem[c] = make_poly(sub, 'active_memtable_mb', 0.0, 64.0)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 360" width="100%" height="360" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">E4: Time Series & Causality Boundary (1-Second Resolution)</text>
      
      <!-- Panel 1: Scan Cost (μs/key) vs Time -->
      <g transform="translate(50, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Time (s)</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        {"".join([f'<polyline fill="none" stroke="{colors[c]}" stroke-width="2" points="{polys_scan[c]}"/>' for c in configs if c in polys_scan])}
        
        <!-- Legend -->
        {"".join([f'<circle cx="{40 + i*115}" cy="235" r="4" fill="{colors[configs[i]]}"/><text x="{48 + i*115}" y="238" fill="#e2e8f0" font-size="9">{labels[configs[i]]}</text>' for i in range(len(configs))])}
      </g>

      <!-- Panel 2: Active MemTable Size (MB) vs Time -->
      <g transform="translate(470, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Active MemTable Size (MB) vs Time (s)</text>
        <line x1="30" y1="200" x2="370" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        {"".join([f'<polyline fill="none" stroke="{colors[c]}" stroke-width="2" points="{polys_mem[c]}"/>' for c in configs if c in polys_mem])}
        
        <!-- Legend -->
        {"".join([f'<circle cx="{40 + i*115}" cy="235" r="4" fill="{colors[configs[i]]}"/><text x="{48 + i*115}" y="238" fill="#e2e8f0" font-size="9">{labels[configs[i]]}</text>' for i in range(len(configs))])}
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "e4_timeseries_evolution.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved E4 SVG plot to {out_file}")

if __name__ == "__main__":
    generate_e4_summary_svg()
