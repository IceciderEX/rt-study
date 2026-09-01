#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for E1 Coverage Pressure validation.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "e1")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "e1_all_runs.csv")

def generate_e1_summary_svg():
    if not os.path.exists(ALL_RUNS_CSV):
        return
    df = pd.read_csv(ALL_RUNS_CSV)
    if df.empty:
        return

    df['group'] = df['exp_id'].str.replace(r'_r\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    spans = ['010', '100', '400']
    labels = ['Span 10 (~1%)', 'Span 100 (~10%)', 'Span 400 (~40%)']

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 360" width="100%" height="360" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">E1: Range Tombstone Coverage Span vs Scan Cost & Keys Dropped</text>
      
      <!-- Panel 1: Scan Cost (μs/key) -->
      <g transform="translate(50, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Span</text>
        <line x1="30" y1="200" x2="350" y2="200" stroke="#64748b"/>
"""
    for i, (sp, lbl) in enumerate(zip(spans, labels)):
        k_dyn = f"e1_span_{sp}_dynamic"
        cost = grouped.loc[k_dyn, 'scan_us_per_key'] if k_dyn in grouped.index else 0
        x = 60 + i * 105
        h = min(140, (cost / 200.0) * 140)
        svg += f"""
        <rect x="{x}" y="{200 - h:.1f}" width="50" height="{h:.1f}" fill="#38bdf8" rx="3"/>
        <text x="{x + 25}" y="{192 - h:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{cost:.1f}</text>
        <text x="{x + 25}" y="220" fill="#94a3b8" font-size="10" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += f"""
      </g>

      <!-- Panel 2: Physically Dropped Keys -->
      <g transform="translate(470, 60)">
        <rect width="380" height="260" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="190" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Compaction Key Drop Count vs Span</text>
        <line x1="30" y1="200" x2="350" y2="200" stroke="#64748b"/>
"""
    for i, (sp, lbl) in enumerate(zip(spans, labels)):
        k_dyn = f"e1_span_{sp}_dynamic"
        drops = grouped.loc[k_dyn, 'compaction_drop_keys'] if k_dyn in grouped.index else 0
        x = 60 + i * 105
        h = min(140, (drops / 500000.0) * 140)
        svg += f"""
        <rect x="{x}" y="{200 - h:.1f}" width="50" height="{h:.1f}" fill="#10b981" rx="3"/>
        <text x="{x + 25}" y="{192 - h:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{drops:.0f}</text>
        <text x="{x + 25}" y="220" fill="#94a3b8" font-size="10" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += """
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "e1_coverage_pressure.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved E1 SVG plot to {out_file}")

if __name__ == "__main__":
    generate_e1_summary_svg()
