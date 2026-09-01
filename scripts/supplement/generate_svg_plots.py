#!/usr/bin/env python3
"""
Pure-Python SVG chart generator for S1, S2, S3 supplement experiments.
Zero external dependencies, renders crisp vector SVG charts.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary", "supplement")
PLOTS_DIR = os.path.join(BASE_DIR, "results", "plots", "supplement")
os.makedirs(PLOTS_DIR, exist_ok=True)

ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "all-runs.csv")
TIMESERIES_CSV = os.path.join(SUMMARY_DIR, "timeseries.csv")

def generate_s1_svg(df):
    s1_df = df[df['exp_id'].str.startswith('s1_')].copy()
    s1_df['group'] = s1_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = s1_df.groupby('group').mean(numeric_only=True)

    # Values
    keys = ['s1_clean_point_heavy', 's1_tombstone_point_heavy', 's1_clean_scan_heavy', 's1_tombstone_scan_heavy']
    labels = ['Clean (Pt)', 'Tomb (Pt)', 'Clean (Sc)', 'Tomb (Sc)']
    colors = ['#38bdf8', '#f43f5e', '#38bdf8', '#f43f5e']

    scan_cost = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in keys]
    get_p99 = [grouped.loc[k, 'get_ctrl_p99_us'] if k in grouped.index else 0 for k in keys]
    cache_miss = [grouped.loc[k, 'block_cache_misses'] / 1e6 if k in grouped.index else 0 for k in keys]

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 350" width="100%" height="350" style="background:#0f172a; font-family:sans-serif;">
      <text x="450" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">S1: Static Tombstone Read Cost Isolation (Clean vs Tombstone)</text>
      
      <!-- Panel 1: Scan Normalized Cost -->
      <g transform="translate(50, 60)">
        <rect width="240" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="120" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key)</text>
        <line x1="30" y1="200" x2="220" y2="200" stroke="#64748b"/>
        <!-- Bars (scale max 3.0) -->
        <rect x="40" y="{200 - scan_cost[0]*60:.1f}" width="35" height="{scan_cost[0]*60:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="57" y="{190 - scan_cost[0]*60:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[0]:.2f}</text>
        <text x="57" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Pt</text>

        <rect x="85" y="{200 - scan_cost[1]*60:.1f}" width="35" height="{scan_cost[1]*60:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="102" y="{190 - scan_cost[1]*60:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[1]:.2f}</text>
        <text x="102" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Pt</text>

        <rect x="130" y="{200 - scan_cost[2]*60:.1f}" width="35" height="{scan_cost[2]*60:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="147" y="{190 - scan_cost[2]*60:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[2]:.2f}</text>
        <text x="147" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Sc</text>

        <rect x="175" y="{200 - scan_cost[3]*60:.1f}" width="35" height="{scan_cost[3]*60:.1f}" fill="{colors[3]}" rx="3"/>
        <text x="192" y="{190 - scan_cost[3]*60:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{scan_cost[3]:.2f}</text>
        <text x="192" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Sc</text>
      </g>

      <!-- Panel 2: Get Control P99 -->
      <g transform="translate(330, 60)">
        <rect width="240" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="120" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Get (Control) P99 (μs)</text>
        <line x1="30" y1="200" x2="220" y2="200" stroke="#64748b"/>
        <!-- Bars (scale max 35.0) -->
        <rect x="40" y="{200 - get_p99[0]*5:.1f}" width="35" height="{get_p99[0]*5:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="57" y="{190 - get_p99[0]*5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_p99[0]:.1f}</text>
        <text x="57" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Pt</text>

        <rect x="85" y="{200 - get_p99[1]*5:.1f}" width="35" height="{get_p99[1]*5:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="102" y="{190 - get_p99[1]*5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_p99[1]:.1f}</text>
        <text x="102" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Pt</text>

        <rect x="130" y="{200 - get_p99[2]*5:.1f}" width="35" height="{get_p99[2]*5:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="147" y="{190 - get_p99[2]*5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_p99[2]:.1f}</text>
        <text x="147" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Sc</text>

        <rect x="175" y="{200 - get_p99[3]*5:.1f}" width="35" height="{get_p99[3]*5:.1f}" fill="{colors[3]}" rx="3"/>
        <text x="192" y="{190 - get_p99[3]*5:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{get_p99[3]:.1f}</text>
        <text x="192" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Sc</text>
      </g>

      <!-- Panel 3: Cache Misses -->
      <g transform="translate(610, 60)">
        <rect width="240" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="120" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Cache Misses (Million)</text>
        <line x1="30" y1="200" x2="220" y2="200" stroke="#64748b"/>
        <!-- Bars (scale max 3.5M) -->
        <rect x="40" y="{200 - cache_miss[0]*50:.1f}" width="35" height="{cache_miss[0]*50:.1f}" fill="{colors[0]}" rx="3"/>
        <text x="57" y="{190 - cache_miss[0]*50:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{cache_miss[0]:.2f}M</text>
        <text x="57" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Pt</text>

        <rect x="85" y="{200 - cache_miss[1]*50:.1f}" width="35" height="{cache_miss[1]*50:.1f}" fill="{colors[1]}" rx="3"/>
        <text x="102" y="{190 - cache_miss[1]*50:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{cache_miss[1]:.2f}M</text>
        <text x="102" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Pt</text>

        <rect x="130" y="{200 - cache_miss[2]*50:.1f}" width="35" height="{cache_miss[2]*50:.1f}" fill="{colors[2]}" rx="3"/>
        <text x="147" y="{190 - cache_miss[2]*50:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{cache_miss[2]:.2f}M</text>
        <text x="147" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Clean-Sc</text>

        <rect x="175" y="{200 - cache_miss[3]*50:.1f}" width="35" height="{cache_miss[3]*50:.1f}" fill="{colors[3]}" rx="3"/>
        <text x="192" y="{190 - cache_miss[3]*50:.1f}" fill="#f8fafc" font-size="10" text-anchor="middle">{cache_miss[3]:.2f}M</text>
        <text x="192" y="215" fill="#94a3b8" font-size="9" text-anchor="middle">Tomb-Sc</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "s1_static_tombstone_cost.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved S1 SVG plot to {out_file}")

def generate_s2_svg(df):
    s2_df = df[df['exp_id'].str.startswith('s2_')].copy()
    s2_df['group'] = s2_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = s2_df.groupby('group').mean(numeric_only=True)

    pt_keys = ['s2_seg20_point_heavy', 's2_seg200_point_heavy', 's2_seg2000_point_heavy']
    sc_keys = ['s2_seg20_scan_heavy', 's2_seg200_scan_heavy', 's2_seg2000_scan_heavy']

    pt_scan = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in pt_keys]
    sc_scan = [grouped.loc[k, 'scan_us_per_key'] if k in grouped.index else 0 for k in sc_keys]

    pt_get = [grouped.loc[k, 'get_aff_p99_us'] if k in grouped.index else 0 for k in pt_keys]
    sc_get = [grouped.loc[k, 'get_aff_p99_us'] if k in grouped.index else 0 for k in sc_keys]

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 350" width="100%" height="350" style="background:#0f172a; font-family:sans-serif;">
      <text x="400" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">S2: Tombstone Fragmentation Fairness (40% Fixed Coverage)</text>
      
      <!-- Panel 1: Scan Cost -->
      <g transform="translate(60, 60)">
        <rect width="320" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="160" y="25" fill="#a855f7" font-size="13" font-weight="bold" text-anchor="middle">Scan Cost (μs/key) vs Segments</text>
        <line x1="30" y1="200" x2="290" y2="200" stroke="#64748b"/>

        <!-- seg-20 -->
        <rect x="50" y="{200 - pt_scan[0]*75:.1f}" width="28" height="{pt_scan[0]*75:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="80" y="{200 - sc_scan[0]*75:.1f}" width="28" height="{sc_scan[0]*75:.1f}" fill="#ec4899" rx="2"/>
        <text x="79" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-20</text>
        <text x="64" y="{190 - pt_scan[0]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_scan[0]:.2f}</text>
        <text x="94" y="{190 - sc_scan[0]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_scan[0]:.2f}</text>

        <!-- seg-200 -->
        <rect x="130" y="{200 - pt_scan[1]*75:.1f}" width="28" height="{pt_scan[1]*75:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="160" y="{200 - sc_scan[1]*75:.1f}" width="28" height="{sc_scan[1]*75:.1f}" fill="#ec4899" rx="2"/>
        <text x="159" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-200</text>
        <text x="144" y="{190 - pt_scan[1]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_scan[1]:.2f}</text>
        <text x="174" y="{190 - sc_scan[1]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_scan[1]:.2f}</text>

        <!-- seg-2000 -->
        <rect x="210" y="{200 - pt_scan[2]*75:.1f}" width="28" height="{pt_scan[2]*75:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="240" y="{200 - sc_scan[2]*75:.1f}" width="28" height="{sc_scan[2]*75:.1f}" fill="#ec4899" rx="2"/>
        <text x="239" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-2000</text>
        <text x="224" y="{190 - pt_scan[2]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_scan[2]:.2f}</text>
        <text x="254" y="{190 - sc_scan[2]*75:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_scan[2]:.2f}</text>
      </g>

      <!-- Panel 2: Get Affected P99 -->
      <g transform="translate(420, 60)">
        <rect width="320" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="160" y="25" fill="#a855f7" font-size="13" font-weight="bold" text-anchor="middle">Get (Affected) P99 (μs) vs Segments</text>
        <line x1="30" y1="200" x2="290" y2="200" stroke="#64748b"/>

        <!-- seg-20 -->
        <rect x="50" y="{200 - pt_get[0]*4:.1f}" width="28" height="{pt_get[0]*4:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="80" y="{200 - sc_get[0]*4:.1f}" width="28" height="{sc_get[0]*4:.1f}" fill="#ec4899" rx="2"/>
        <text x="79" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-20</text>
        <text x="64" y="{190 - pt_get[0]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_get[0]:.1f}</text>
        <text x="94" y="{190 - sc_get[0]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_get[0]:.1f}</text>

        <!-- seg-200 -->
        <rect x="130" y="{200 - pt_get[1]*4:.1f}" width="28" height="{pt_get[1]*4:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="160" y="{200 - sc_get[1]*4:.1f}" width="28" height="{sc_get[1]*4:.1f}" fill="#ec4899" rx="2"/>
        <text x="159" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-200</text>
        <text x="144" y="{190 - pt_get[1]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_get[1]:.1f}</text>
        <text x="174" y="{190 - sc_get[1]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_get[1]:.1f}</text>

        <!-- seg-2000 -->
        <rect x="210" y="{200 - pt_get[2]*4:.1f}" width="28" height="{pt_get[2]*4:.1f}" fill="#38bdf8" rx="2"/>
        <rect x="240" y="{200 - sc_get[2]*4:.1f}" width="28" height="{sc_get[2]*4:.1f}" fill="#ec4899" rx="2"/>
        <text x="239" y="215" fill="#94a3b8" font-size="10" text-anchor="middle">seg-2000</text>
        <text x="224" y="{190 - pt_get[2]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{pt_get[2]:.1f}</text>
        <text x="254" y="{190 - sc_get[2]*4:.1f}" fill="#f8fafc" font-size="9" text-anchor="middle">{sc_get[2]:.1f}</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "s2_fragmentation_comparison.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved S2 SVG plot to {out_file}")

def generate_s3_svg():
    if not os.path.exists(TIMESERIES_CSV):
        return
    ts_df = pd.read_csv(TIMESERIES_CSV)
    if ts_df.empty:
        return

    ts_df['config_base'] = ts_df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)
    grouped = ts_df.groupby(['config_base', 'second_idx']).mean(numeric_only=True).reset_index()

    ctrl = grouped[grouped['config_base'] == 's3_control'].sort_values('second_idx')
    hot = grouped[grouped['config_base'] == 's3_hot_reclaim'].sort_values('second_idx')
    cold = grouped[grouped['config_base'] == 's3_cold_reclaim'].sort_values('second_idx')

    # Build polyline points for Throughput
    def make_poly(sub_df, field, y_min, y_max, h=160, w=320):
        pts = []
        for _, row in sub_df.iterrows():
            sec = row['second_idx']
            val = row[field]
            x = 30 + (sec / 60.0) * w
            y = 200 - ((val - y_min) / (y_max - y_min + 1e-6)) * h
            pts.append(f"{x:.1f},{y:.1f}")
        return " ".join(pts)

    # Total IOPS
    ctrl['total_iops'] = ctrl['get_ctrl_ops'] + ctrl['get_aff_ops'] + ctrl['scan_ops'] + ctrl['put_ops']
    hot['total_iops'] = hot['get_ctrl_ops'] + hot['get_aff_ops'] + hot['scan_ops'] + hot['put_ops']
    cold['total_iops'] = cold['get_ctrl_ops'] + cold['get_aff_ops'] + cold['scan_ops'] + cold['put_ops']

    poly_ctrl_iops = make_poly(ctrl, 'total_iops', 140000, 200000)
    poly_hot_iops = make_poly(hot, 'total_iops', 140000, 200000)
    poly_cold_iops = make_poly(cold, 'total_iops', 140000, 200000)

    # Scan P99
    poly_ctrl_scan = make_poly(ctrl, 'scan_p99_us', 200, 450)
    poly_hot_scan = make_poly(hot, 'scan_p99_us', 200, 450)
    poly_cold_scan = make_poly(cold, 'scan_p99_us', 200, 450)

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 850 350" width="100%" height="350" style="background:#0f172a; font-family:sans-serif;">
      <text x="425" y="30" fill="#f8fafc" font-size="18" font-weight="bold" text-anchor="middle">S3: Controlled Reclamation 1-Second Time Series Dynamics</text>
      
      <!-- Panel 1: Throughput (IOPS) -->
      <g transform="translate(50, 60)">
        <rect width="360" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="180" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">Frontend Throughput (IOPS)</text>
        <line x1="30" y1="200" x2="350" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <!-- CompactRange Trigger marker at t=20s (x = 30 + 20/60*320 = 136.6) -->
        <line x1="136.7" y1="40" x2="136.7" y2="200" stroke="#f43f5e" stroke-dasharray="4" stroke-width="1.5"/>
        <text x="136.7" y="35" fill="#f43f5e" font-size="9" text-anchor="middle">t=20s Compact</text>

        <!-- Curves -->
        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_ctrl_iops}"/>
        <polyline fill="none" stroke="#22c55e" stroke-width="2" points="{poly_hot_iops}"/>
        <polyline fill="none" stroke="#f59e0b" stroke-width="2" points="{poly_cold_iops}"/>

        <!-- Legend -->
        <circle cx="45" cy="225" r="4" fill="#38bdf8"/>
        <text x="55" y="228" fill="#e2e8f0" font-size="9">Control</text>
        <circle cx="120" cy="225" r="4" fill="#22c55e"/>
        <text x="130" y="228" fill="#e2e8f0" font-size="9">Hot Reclaim</text>
        <circle cx="210" cy="225" r="4" fill="#f59e0b"/>
        <text x="220" y="228" fill="#e2e8f0" font-size="9">Cold Reclaim</text>
      </g>

      <!-- Panel 2: Scan P99 (μs) -->
      <g transform="translate(440, 60)">
        <rect width="360" height="240" rx="8" fill="#1e293b" stroke="#334155"/>
        <text x="180" y="25" fill="#38bdf8" font-size="13" font-weight="bold" text-anchor="middle">RangeScan P99 Latency (μs)</text>
        <line x1="30" y1="200" x2="350" y2="200" stroke="#64748b"/>
        <line x1="30" y1="40" x2="30" y2="200" stroke="#64748b"/>
        <line x1="136.7" y1="40" x2="136.7" y2="200" stroke="#f43f5e" stroke-dasharray="4" stroke-width="1.5"/>
        <text x="136.7" y="35" fill="#f43f5e" font-size="9" text-anchor="middle">t=20s Compact</text>

        <!-- Curves -->
        <polyline fill="none" stroke="#38bdf8" stroke-width="2" points="{poly_ctrl_scan}"/>
        <polyline fill="none" stroke="#22c55e" stroke-width="2" points="{poly_hot_scan}"/>
        <polyline fill="none" stroke="#f59e0b" stroke-width="2" points="{poly_cold_scan}"/>

        <!-- Legend -->
        <circle cx="45" cy="225" r="4" fill="#38bdf8"/>
        <text x="55" y="228" fill="#e2e8f0" font-size="9">Control</text>
        <circle cx="120" cy="225" r="4" fill="#22c55e"/>
        <text x="130" y="228" fill="#e2e8f0" font-size="9">Hot Reclaim</text>
        <circle cx="210" cy="225" r="4" fill="#f59e0b"/>
        <text x="220" y="228" fill="#e2e8f0" font-size="9">Cold Reclaim</text>
      </g>
    </svg>"""

    out_file = os.path.join(PLOTS_DIR, "s3_timeseries_comparison.svg")
    with open(out_file, "w") as f:
        f.write(svg)
    print(f"Saved S3 SVG plot to {out_file}")

if __name__ == "__main__":
    if os.path.exists(ALL_RUNS_CSV):
        df = pd.read_csv(ALL_RUNS_CSV)
        generate_s1_svg(df)
        generate_s2_svg(df)
    generate_s3_svg()
