#!/usr/bin/env python3
"""
Formal V2 - E8-FG Aggregated and Differential FlameGraph Generator
1. Aggregates N=3 replicates per condition (CLEAN, T0-MEM, POSTFLUSH-SST)
2. Normalizes sample counts to 1,000,000
3. Generates Normalized Differential FlameGraphs:
   - CLEAN -> T0-MEM
   - POSTFLUSH-SST -> T0-MEM
4. Parses Self% and Children% Top-20 hotspots and outputs CSV:
   - results/summary/formal-v2-e8-flamegraph-hotspots.csv
"""

import os
import sys
import json
import subprocess
from collections import defaultdict
import csv

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
RESULTS_BASE = os.path.join(BASE_DIR, "results/formal_v2/e8_flamegraph")
DIFF_DIR = os.path.join(RESULTS_BASE, "differential")
SUMMARY_DIR = os.path.join(BASE_DIR, "results/summary")
FLAMEGRAPH_DIR = "/home/wam/grad/tools/FlameGraph"

CONDITIONS = ["clean", "t0-mem", "postflush-sst"]

def aggregate_condition_folds(cond_id, reps=[1, 2, 3], is_smoke=False):
    stacks = defaultdict(int)
    total_samples = 0

    if is_smoke:
        f_path = os.path.join(RESULTS_BASE, "smoke", cond_id, "perf.folded")
        if os.path.exists(f_path):
            with open(f_path) as f:
                for line in f:
                    line = line.strip()
                    if not line: continue
                    parts = line.rsplit(" ", 1)
                    if len(parts) != 2: continue
                    stk, cnt = parts[0], int(parts[1])
                    stacks[stk] += cnt
                    total_samples += cnt
    else:
        for r in reps:
            f_path = os.path.join(RESULTS_BASE, f"rep{r}", cond_id, "perf.folded")
            if os.path.exists(f_path):
                with open(f_path) as f:
                    for line in f:
                        line = line.strip()
                        if not line: continue
                        parts = line.rsplit(" ", 1)
                        if len(parts) != 2: continue
                        stk, cnt = parts[0], int(parts[1])
                        stacks[stk] += cnt
                        total_samples += cnt

    return stacks, total_samples

def write_folded(stacks, out_path):
    with open(out_path, "w") as f:
        for stk in sorted(stacks.keys()):
            f.write(f"{stk} {stacks[stk]}\n")

def write_normalized_folded(stacks, total_samples, out_path, target_total=1000000):
    factor = target_total / total_samples if total_samples > 0 else 1.0
    with open(out_path, "w") as f:
        for stk in sorted(stacks.keys()):
            norm_cnt = max(1, int(round(stacks[stk] * factor)))
            f.write(f"{stk} {norm_cnt}\n")

def parse_perf_report(report_path, is_children=False):
    # Returns list of (percent, dso, symbol)
    items = []
    if not os.path.exists(report_path):
        return items
    with open(report_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("|") or line.startswith("-"):
                continue
            if is_children:
                parts = line.split(None, 3)
                if len(parts) >= 4 and parts[0].endswith("%") and parts[1].endswith("%"):
                    try:
                        pct = float(parts[0].rstrip("%"))
                        dso = parts[2]
                        sym = parts[3].rsplit("-", 2)[0].strip()
                        if sym.startswith("[.] "):
                            sym = sym[4:]
                        items.append((pct, dso, sym))
                    except ValueError:
                        continue
            else:
                parts = line.split(None, 2)
                if len(parts) >= 3 and parts[0].endswith("%"):
                    try:
                        pct = float(parts[0].rstrip("%"))
                        dso = parts[1]
                        sym = parts[2].rsplit("-", 2)[0].strip()
                        if sym.startswith("[.] "):
                            sym = sym[4:]
                        items.append((pct, dso, sym))
                    except ValueError:
                        continue
    return items

def main():
    is_smoke = "--smoke" in sys.argv
    os.makedirs(DIFF_DIR, exist_ok=True)
    os.makedirs(SUMMARY_DIR, exist_ok=True)

    print("=" * 64)
    print(f"  FormalV2-E8 FlameGraph Analysis Generator (Smoke: {is_smoke})")
    print("=" * 64)

    agg_totals = {}
    flamegraph_pl = os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl")
    difffolded_pl = os.path.join(FLAMEGRAPH_DIR, "difffolded.pl")

    # 1. Aggregate and render SVG for each condition
    for cond in CONDITIONS:
        stacks, total_samples = aggregate_condition_folds(cond, is_smoke=is_smoke)
        agg_totals[cond] = total_samples
        print(f"Condition: {cond.upper()} | Aggregated Samples: {total_samples}")

        raw_folded = os.path.join(DIFF_DIR, f"{cond}_aggregate.folded")
        norm_folded = os.path.join(DIFF_DIR, f"{cond}_normalized.folded")
        svg_out = os.path.join(DIFF_DIR, f"{cond}_aggregate.svg")

        write_folded(stacks, raw_folded)
        write_normalized_folded(stacks, total_samples, norm_folded)

        title = f"{cond.upper()} ScanIntersect Aggregated CPU Flame Graph ({'Smoke' if is_smoke else 'N=3'})"
        with open(svg_out, "w") as f_out:
            subprocess.run([
                flamegraph_pl,
                "--width", "1800",
                "--hash",
                "--title", title,
                "--countname", "samples",
                raw_folded
            ], stdout=f_out, check=True)

    # 2. Generate Normalized Differential FlameGraphs
    diff_pairs = [
        ("clean", "t0-mem", "CLEAN to T0-MEM ScanIntersect Differential Flame Graph"),
        ("postflush-sst", "t0-mem", "POSTFLUSH-SST to T0-MEM ScanIntersect Differential Flame Graph")
    ]

    for base_cond, target_cond, diff_title in diff_pairs:
        base_norm = os.path.join(DIFF_DIR, f"{base_cond}_normalized.folded")
        target_norm = os.path.join(DIFF_DIR, f"{target_cond}_normalized.folded")
        diff_folded = os.path.join(DIFF_DIR, f"{base_cond}_to_{target_cond}_diff.folded")
        diff_svg = os.path.join(DIFF_DIR, f"{base_cond}_to_{target_cond}_diff.svg")

        # difffolded.pl base target > diff.folded (using pre-normalized inputs)
        with open(diff_folded, "w") as f_out:
            subprocess.run([difffolded_pl, "-s", base_norm, target_norm], stdout=f_out, check=True)

        with open(diff_svg, "w") as f_out:
            subprocess.run([
                flamegraph_pl,
                "--width", "1800",
                "--hash",
                "--title", diff_title,
                "--countname", "samples",
                diff_folded
            ], stdout=f_out, check=True)
        print(f"[DIFF FLAMEGRAPH] Generated: {diff_svg}")

    # 3. Build Summary CSV
    csv_path = os.path.join(SUMMARY_DIR, "formal-v2-e8-flamegraph-hotspots.csv")
    csv_rows = []
    
    # We collect for all conditions and reps
    reps = [0] if is_smoke else [1, 2, 3]
    for r in reps:
        rep_label = "smoke" if is_smoke else f"rep{r}"
        for cond in CONDITIONS:
            sum_file = os.path.join(RESULTS_BASE, rep_label, cond, "summary.json")
            if not os.path.exists(sum_file):
                continue
            with open(sum_file) as f:
                meta = json.load(f)

            # Self report
            self_rep = os.path.join(RESULTS_BASE, rep_label, cond, "perf-report-self.txt")
            self_items = parse_perf_report(self_rep)[:20]
            for pct, dso, sym in self_items:
                csv_rows.append({
                    "condition": cond.upper(),
                    "replicate": rep_label,
                    "metric_type": "self",
                    "symbol": sym,
                    "dso": dso,
                    "percent": pct,
                    "total_samples": meta.get("total_samples", 0),
                    "unknown_leaf_percent": meta.get("unknown_leaf_pct", 0.0),
                    "unknown_stack_percent": meta.get("unknown_stack_pct", 0.0),
                    "lost_samples": 0,
                    "binary_sha256": "17ea7c1b28d2559ff494c7ea08cb49be7c8cb21490106a00552010f54afa3a1a",
                    "state_sha256": meta.get("state_sha256", "")
                })

            # Children report
            child_rep = os.path.join(RESULTS_BASE, rep_label, cond, "perf-report-children.txt")
            child_items = parse_perf_report(child_rep, is_children=True)[:20]
            for pct, dso, sym in child_items:
                csv_rows.append({
                    "condition": cond.upper(),
                    "replicate": rep_label,
                    "metric_type": "children",
                    "symbol": sym,
                    "dso": dso,
                    "percent": pct,
                    "total_samples": meta.get("total_samples", 0),
                    "unknown_leaf_percent": meta.get("unknown_leaf_pct", 0.0),
                    "unknown_stack_percent": meta.get("unknown_stack_pct", 0.0),
                    "lost_samples": 0,
                    "binary_sha256": "17ea7c1b28d2559ff494c7ea08cb49be7c8cb21490106a00552010f54afa3a1a",
                    "state_sha256": meta.get("state_sha256", "")
                })

    if csv_rows:
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(csv_rows[0].keys()))
            writer.writeheader()
            writer.writerows(csv_rows)
        print(f"[SUMMARY CSV] Written {len(csv_rows)} rows to {csv_path}")

if __name__ == "__main__":
    main()
