#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for E2 Value Size - MemTable Mismatch validation suite.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "e2")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "e2_all_runs.csv")

def generate_e2_summary_svg(df):
    df['group'] = df['exp_id'].str.replace(r'_r\d+$', '', regex=True)
    grouped = df.groupby('group').mean(numeric_only=True)

    val_sizes = ['0064', '0256', '1024', '4096']
    labels = ['64 B', '256 B', '1 KiB', '4 KiB']

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 400" width="100%" height="400" style="background:#0f172a; font-family:sans-serif;">
      <text x="480" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">E2: Value Size - MemTable Pressure Mismatch Validation</text>
      
      <!-- Panel 1: Scan Cost (μs/key) Comparison -->
      <g transform="translate(50, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Scan Cost (μs/key): Default vs T=256</text>
        <line x1="30" y1="250" x2="380" y2="250" stroke="#64748b"/>
        
        <!-- Legend -->
        <rect x="110" y="45" width="15" height="10" fill="#ef4444" rx="2"/>
        <text x="130" y="54" fill="#e2e8f0" font-size="10">Default (T=0)</text>
        <rect x="240" y="45" width="15" height="10" fill="#10b981" rx="2"/>
        <text x="260" y="54" fill="#e2e8f0" font-size="10">Fixed (T=256)</text>
"""
    for i, (v_code, lbl) in enumerate(zip(val_sizes, labels)):
        def_k = f"e2_val_{v_code}_t000"
        fl_k = f"e2_val_{v_code}_t256"

        def_cost = grouped.loc[def_k, 'scan_us_per_key'] if def_k in grouped.index else 0
        fl_cost = grouped.loc[fl_k, 'scan_us_per_key'] if fl_k in grouped.index else 0

        x_base = 50 + i * 85
        # scale max 260
        h_def = min(170, (def_cost / 260.0) * 170)
        h_fl = min(170, (fl_cost / 260.0) * 170)

        svg += f"""
        <!-- Pair {lbl} -->
        <rect x="{x_base}" y="{250 - h_def:.1f}" width="30" height="{h_def:.1f}" fill="#ef4444" rx="3"/>
        <text x="{x_base + 15}" y="{242 - h_def:.1f}" fill="#f8fafc" font-size="8" text-anchor="middle">{def_cost:.1f}</text>

        <rect x="{x_base + 35}" y="{250 - h_fl:.1f}" width="30" height="{h_fl:.1f}" fill="#10b981" rx="3"/>
        <text x="{x_base + 50}" y="{242 - h_fl:.1f}" fill="#f8fafc" font-size="8" text-anchor="middle">{fl_cost:.1f}</text>

        <text x="{x_base + 32}" y="270" fill="#94a3b8" font-size="10" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += f"""
      </g>

      <!-- Panel 2: Write Amplification vs Put P99 Tradeoff -->
      <g transform="translate(500, 60)">
        <rect width="410" height="300" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="205" y="25" fill="#38bdf8" font-size="14" font-weight="bold" text-anchor="middle">Write Amplification (WA) for T=256</text>
        <line x1="30" y1="250" x2="380" y2="250" stroke="#64748b"/>
"""
    for i, (v_code, lbl) in enumerate(zip(val_sizes, labels)):
        fl_k = f"e2_val_{v_code}_t256"
        wa = grouped.loc[fl_k, 'write_amplification'] if fl_k in grouped.index else 0

        x_base = 50 + i * 85
        # scale max 120
        h_wa = min(170, (wa / 120.0) * 170)

        svg += f"""
        <!-- WA Bar {lbl} -->
        <rect x="{x_base + 15}" y="{250 - h_wa:.1f}" width="40" height="{h_wa:.1f}" fill="#f59e0b" rx="3"/>
        <text x="{x_base + 35}" y="{242 - h_wa:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{wa:.1f}x</text>
        <text x="{x_base + 35}" y="270" fill="#94a3b8" font-size="10" font-weight="bold" text-anchor="middle">{lbl}</text>
        """

    svg += """
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "e2_value_mismatch.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved E2 SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_e2_summary_svg(df)
