#!/usr/bin/env python3
import csv
import os

def main():
    csv_path = "/home/wam/grad/s14-range-delete-study/results/summary/b1-natural-flush-boundary.csv"
    out_svg = "/home/wam/grad/s14-range-delete-study/results/plots/b1/b1_natural_flush_boundary.svg"

    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found")
        return

    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    # Categories: 0.25x (16MB), 0.50x (32MB), 0.75x (48MB), 1.25x (80MB)
    labels = ["0.25x (16MB)", "0.50x (32MB)", "0.75x (48MB)", "1.25x (80MB)"]
    
    # Extract values
    scan_costs = [float(r['scan_us_per_key_mean']) for r in rows]
    scan_p99s = [float(r['scan_p99_us_mean']) for r in rows]
    put_p99s = [float(r['put_p99_us_mean']) for r in rows]
    flush_cnts = [float(r['flush_count_total_mean']) for r in rows]
    pwa_vals = [float(r['pwa_mean']) for r in rows]

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
    svg.append('  .legend-text { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: 11px; fill: #475569; }')
    svg.append('  .grid { stroke: #e2e8f0; stroke-dasharray: 4,4; }')
    svg.append('</style>')

    # Background
    svg.append(f'<rect width="{width}" height="{height}" fill="#f8fafc" rx="8" />')

    # Titles
    svg.append('<text x="50" y="40" class="title">B1: Natural MemTable Flush Boundary Verification</text>')
    svg.append('<text x="50" y="62" class="subtitle">Demonstrating read latency recovery when foreground Put writes cross the 64MB MemTable threshold (256B Value, Native memtable_max_range_deletions=0)</text>')

    # Panel 1: Scan Cost per Key (Top Left)
    p1_x, p1_y, p1_w, p1_h = 60, 110, 460, 260
    svg.append(f'<rect x="{p1_x}" y="{p1_y}" width="{p1_w}" height="{p1_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p1_x+16}" y="{p1_y+24}" class="panel-title">Scan Unit Cost (μs / valid key)</text>')

    max_cost = max(scan_costs) * 1.15 if scan_costs else 250
    for i, (lab, val) in enumerate(zip(labels, scan_costs)):
        bx = p1_x + 50 + i * 95
        bh = (val / max_cost) * 160
        by = p1_y + 210 - bh
        color = "#10b981" if val < 20 else "#ef4444"
        svg.append(f'<rect x="{bx}" y="{by}" width="60" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+30}" y="{by-6}" class="bar-label" text-anchor="middle">{val:.2f}</text>')
        svg.append(f'<text x="{bx+30}" y="{p1_y+230}" class="axis-label" text-anchor="middle">{lab.split()[0]}</text>')
        svg.append(f'<text x="{bx+30}" y="{p1_y+244}" class="axis-label" text-anchor="middle" font-size="9">{lab.split()[1]}</text>')

    # Panel 2: Scan P99 & Put P99 (Top Right)
    p2_x, p2_y, p2_w, p2_h = 580, 110, 460, 260
    svg.append(f'<rect x="{p2_x}" y="{p2_y}" width="{p2_w}" height="{p2_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p2_x+16}" y="{p2_y+24}" class="panel-title">Scan P99 vs Put P99 (μs)</text>')

    max_p99 = max(scan_p99s) * 1.15 if scan_p99s else 50000
    for i, (lab, s_p99, p_p99) in enumerate(zip(labels, scan_p99s, put_p99s)):
        bx = p2_x + 45 + i * 95
        sbh = (s_p99 / max_p99) * 160
        sby = p2_y + 210 - sbh
        pbh = max(3, (p_p99 / max_p99) * 160)
        pby = p2_y + 210 - pbh

        svg.append(f'<rect x="{bx}" y="{sby}" width="32" height="{sbh}" fill="#f59e0b" rx="3" />')
        svg.append(f'<rect x="{bx+36}" y="{pby}" width="32" height="{pbh}" fill="#6366f1" rx="3" />')
        svg.append(f'<text x="{bx+16}" y="{sby-5}" class="bar-label" text-anchor="middle" font-size="9">{int(s_p99)}</text>')
        svg.append(f'<text x="{bx+52}" y="{pby-5}" class="bar-label" text-anchor="middle" font-size="9">{int(p_p99)}</text>')
        svg.append(f'<text x="{bx+34}" y="{p2_y+230}" class="axis-label" text-anchor="middle">{lab.split()[0]}</text>')

    # Legend for Panel 2
    svg.append(f'<rect x="{p2_x+280}" y="{p2_y+15}" width="12" height="12" fill="#f59e0b" rx="2" />')
    svg.append(f'<text x="{p2_x+298}" y="{p2_y+25}" class="legend-text">Scan P99</text>')
    svg.append(f'<rect x="{p2_x+360}" y="{p2_y+15}" width="12" height="12" fill="#6366f1" rx="2" />')
    svg.append(f'<text x="{p2_x+378}" y="{p2_y+25}" class="legend-text">Put P99</text>')

    # Panel 3: Flush Count (Bottom Left)
    p3_x, p3_y, p3_w, p3_h = 60, 410, 460, 260
    svg.append(f'<rect x="{p3_x}" y="{p3_y}" width="{p3_w}" height="{p3_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p3_x+16}" y="{p3_y+24}" class="panel-title">Natural Flush Count (kWriteBufferFull)</text>')

    for i, (lab, cnt) in enumerate(zip(labels, flush_cnts)):
        bx = p3_x + 50 + i * 95
        bh = max(4, cnt * 80)
        by = p3_y + 210 - bh
        color = "#0284c7" if cnt > 0 else "#94a3b8"
        svg.append(f'<rect x="{bx}" y="{by}" width="60" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+30}" y="{by-6}" class="bar-label" text-anchor="middle">{int(cnt)} 次</text>')
        svg.append(f'<text x="{bx+30}" y="{p3_y+230}" class="axis-label" text-anchor="middle">{lab.split()[0]}</text>')
        svg.append(f'<text x="{bx+30}" y="{p3_y+244}" class="axis-label" text-anchor="middle" font-size="9">{lab.split()[1]}</text>')

    # Panel 4: Physical Write Amplification (Bottom Right)
    p4_x, p4_y, p4_w, p4_h = 580, 410, 460, 260
    svg.append(f'<rect x="{p4_x}" y="{p4_y}" width="{p4_w}" height="{p4_h}" fill="#ffffff" stroke="#cbd5e1" rx="6" />')
    svg.append(f'<text x="{p4_x+16}" y="{p4_y+24}" class="panel-title">Total Physical Write Amplification (PWA)</text>')

    max_pwa = max(pwa_vals) * 1.3 if any(pwa_vals) else 10
    for i, (lab, pwa) in enumerate(zip(labels, pwa_vals)):
        bx = p4_x + 50 + i * 95
        bh = max(4, (pwa / max_pwa) * 160) if max_pwa > 0 else 4
        by = p4_y + 210 - bh
        color = "#8b5cf6" if pwa > 0 else "#94a3b8"
        svg.append(f'<rect x="{bx}" y="{by}" width="60" height="{bh}" fill="{color}" rx="4" />')
        svg.append(f'<text x="{bx+30}" y="{by-6}" class="bar-label" text-anchor="middle">{pwa:.1f}x</text>')
        svg.append(f'<text x="{bx+30}" y="{p4_y+230}" class="axis-label" text-anchor="middle">{lab.split()[0]}</text>')
        svg.append(f'<text x="{bx+30}" y="{p4_y+244}" class="axis-label" text-anchor="middle" font-size="9">{lab.split()[1]}</text>')

    svg.append('</svg>')

    with open(out_svg, 'w', encoding='utf-8') as f:
        f.write('\n'.join(svg))
    print(f"Saved B1 SVG plot to {out_svg}")

if __name__ == "__main__":
    main()
