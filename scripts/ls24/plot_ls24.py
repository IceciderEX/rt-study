#!/usr/bin/env python3
import csv
import os

def main():
    csv_path = "/home/wam/grad/s14-range-delete-study/results/summary/ls24-main.csv"
    out_svg = "/home/wam/grad/s14-range-delete-study/results/plots/ls24/ls24_large_scale_tradeoff.svg"

    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found")
        return

    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    labels = [f"T={r['threshold']}" if r['threshold'] != '0' else 'T=0 (Disabled)' for r in rows]
    scan_costs = [float(r['scan_us_per_key_mean']) for r in rows]
    put_p99s = [float(r['put_p99_us_mean']) for r in rows]
    pwa_vals = [float(r['pwa_mean']) for r in rows]
    iops_vals = [float(r['iops_mean']) for r in rows]

    width = 1100
    height = 750
    os.makedirs(os.path.dirname(out_svg), exist_ok=True)

    svg = []
    svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">')
    svg.append('<style>')
    svg.append('  .title { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 20px; font-weight: 700; fill: #1e293b; }')
    svg.append('  .subtitle { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 13px; fill: #64748b; }')
    svg.append('  .panel-title { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 14px; font-weight: 600; fill: #334155; }')
    svg.append('  .axis-label { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 11px; fill: #64748b; }')
    svg.append('  .bar-label { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 11px; font-weight: 600; fill: #1e293b; }')
    svg.append('</style>')

    # Background
    svg.append(f'<rect width="{width}" height="{height}" fill="#f8fafc" rx="8" />')

    # Titles
    svg.append('<text x="50" y="40" class="title">LS24: 24GiB (100M Keys) Large-Scale RangeDelete Tradeoff</text>')
    svg.append('<text x="50" y="62" class="subtitle">Validation under 24GiB Payload on NVMe: T=0 vs T=256 vs T=2048</text>')

    # Panel 1: Scan Unit Cost (Top Left)
    p1_x, p1_y, p1_w, p1_h = 60, 110, 460, 260
    svg.append(f'<rect x="{p1_x}" y="{p1_y}" width="{p1_w}" height="{p1_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p1_x+16}" y="{p1_y+24}" class="panel-title">Scan Unit Cost (μs / valid key)</text>')

    max_cost = max(scan_costs) * 1.15 if scan_costs else 250
    for i, (lab, val) in enumerate(zip(labels, scan_costs)):
        bx = p1_x + 70 + i * 120
        bh = (val / max_cost) * 160 if max_cost > 0 else 4
        by = p1_y + 210 - bh
        color = "#10b981" if val < 20 else "#ef4444"
        svg.append(f'<rect x="{bx}" y="{by}" width="70" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+35}" y="{by-6}" class="bar-label" text-anchor="middle">{val:.2f}</text>')
        svg.append(f'<text x="{bx+35}" y="{p1_y+230}" class="axis-label" text-anchor="middle">{lab}</text>')

    # Panel 2: Put P99 (Top Right)
    p2_x, p2_y, p2_w, p2_h = 580, 110, 460, 260
    svg.append(f'<rect x="{p2_x}" y="{p2_y}" width="{p2_w}" height="{p2_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p2_x+16}" y="{p2_y+24}" class="panel-title">Put P99 Write Tail Latency (μs)</text>')

    max_put = max(put_p99s) * 1.15 if put_p99s else 4000
    for i, (lab, val) in enumerate(zip(labels, put_p99s)):
        bx = p2_x + 70 + i * 120
        bh = (val / max_put) * 160 if max_put > 0 else 4
        by = p2_y + 210 - bh
        color = "#ef4444" if val > 2000 else "#6366f1"
        svg.append(f'<rect x="{bx}" y="{by}" width="70" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+35}" y="{by-6}" class="bar-label" text-anchor="middle">{int(val)}</text>')
        svg.append(f'<text x="{bx+35}" y="{p2_y+230}" class="axis-label" text-anchor="middle">{lab}</text>')

    # Panel 3: Overall Throughput (Bottom Left)
    p3_x, p3_y, p3_w, p3_h = 60, 410, 460, 260
    svg.append(f'<rect x="{p3_x}" y="{p3_y}" width="{p3_w}" height="{p3_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p3_x+16}" y="{p3_y+24}" class="panel-title">Overall IOPS (kOps/s)</text>')

    max_iops = max(iops_vals) * 1.15 if iops_vals else 100000
    for i, (lab, val) in enumerate(zip(labels, iops_vals)):
        bx = p3_x + 70 + i * 120
        bh = (val / max_iops) * 160 if max_iops > 0 else 4
        by = p3_y + 210 - bh
        color = "#0284c7" if val > 10000 else "#94a3b8"
        svg.append(f'<rect x="{bx}" y="{by}" width="70" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+35}" y="{by-6}" class="bar-label" text-anchor="middle">{val/1000.0:.1f}k</text>')
        svg.append(f'<text x="{bx+35}" y="{p3_y+230}" class="axis-label" text-anchor="middle">{lab}</text>')

    # Panel 4: Physical Write Amplification (Bottom Right)
    p4_x, p4_y, p4_w, p4_h = 580, 410, 460, 260
    svg.append(f'<rect x="{p4_x}" y="{p4_y}" width="{p4_w}" height="{p4_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p4_x+16}" y="{p4_y+24}" class="panel-title">Total Physical Write Amplification (PWA)</text>')

    max_pwa = max(pwa_vals) * 1.2 if any(pwa_vals) else 100
    for i, (lab, val) in enumerate(zip(labels, pwa_vals)):
        bx = p4_x + 70 + i * 120
        bh = max(4, (val / max_pwa) * 160) if max_pwa > 0 else 4
        by = p4_y + 210 - bh
        color = "#8b5cf6" if val > 0 else "#94a3b8"
        svg.append(f'<rect x="{bx}" y="{by}" width="70" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+35}" y="{by-6}" class="bar-label" text-anchor="middle">{val:.1f}x</text>')
        svg.append(f'<text x="{bx+35}" y="{p4_y+230}" class="axis-label" text-anchor="middle">{lab}</text>')

    svg.append('</svg>')

    with open(out_svg, 'w', encoding='utf-8') as f:
        f.write('\n'.join(svg))
    print(f"Saved LS24 SVG plot to {out_svg}")

if __name__ == "__main__":
    main()
