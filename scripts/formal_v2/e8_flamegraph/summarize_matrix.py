#!/usr/bin/env python3
"""
Formal V2 - E8-FG Matrix Statistical and Hotspot Analysis Summarizer
"""

import os
import json
import csv
from collections import defaultdict

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
RESULTS_BASE = os.path.join(BASE_DIR, "results/formal_v2/e8_flamegraph")
SUMMARY_CSV = os.path.join(BASE_DIR, "results/summary/formal-v2-e8-flamegraph-hotspots.csv")
DIFF_FILE = os.path.join(RESULTS_BASE, "differential/postflush-sst_to_t0-mem_diff.folded")

def main():
    print("=" * 80)
    print("  FormalV2-E8-FG: 9-Run Matrix Verification & IOPS Summary")
    print("=" * 80)

    stats = defaultdict(lambda: defaultdict(list))

    for r in [1, 2, 3]:
        for cond in ["clean", "t0-mem", "postflush-sst"]:
            sum_path = os.path.join(RESULTS_BASE, f"rep{r}", cond, "summary.json")
            if not os.path.exists(sum_path): continue
            with open(sum_path) as f:
                m = json.load(f)
            iops = m["completed_ops"] / m["elapsed_sec"]
            samples = m["total_samples"]
            leaf_unk = m["unknown_leaf_pct"]
            stk_unk = m["unknown_stack_pct"]
            keys = m["visible_keys_count"]
            sha = m["state_sha256"][:12]
            stats[cond]["iops"].append(iops)
            stats[cond]["samples"].append(samples)
            stats[cond]["unk_leaf"].append(leaf_unk)
            stats[cond]["unk_stk"].append(stk_unk)
            stats[cond]["keys"].append(keys)
            stats[cond]["sha"].append(sha)
            print(f"Rep {r} | {cond.upper():14s} | IOPS: {iops:8.1f} | Samples: {samples:12d} | Leaf Unk: {leaf_unk:5.2f}% | Stk Unk: {stk_unk:5.2f}% | Keys: {keys} | SHA: {sha}...")

    print("\n" + "=" * 80)
    print("  Aggregated Means (N=3):")
    print("=" * 80)
    for cond in ["clean", "t0-mem", "postflush-sst"]:
        mean_iops = sum(stats[cond]["iops"])/3.0
        mean_leaf = sum(stats[cond]["unk_leaf"])/3.0
        mean_stk = sum(stats[cond]["unk_stk"])/3.0
        keys_val = stats[cond]["keys"][0]
        print(f"{cond.upper():14s} | Mean IOPS: {mean_iops:8.1f} | Mean Leaf Unk: {mean_leaf:5.2f}% | Mean Stk Unk: {mean_stk:5.2f}% | Keys: {keys_val}")

    # Parse Summary CSV
    agg = defaultdict(lambda: defaultdict(list))
    with open(SUMMARY_CSV) as f:
        reader = csv.DictReader(f)
        for row in reader:
            cond = row["condition"]
            mtype = row["metric_type"]
            sym = row["symbol"].strip()
            pct = float(row["percent"])
            agg[(cond, mtype)][sym].append(pct)

    print("\n" + "=" * 90)
    print("  TOP 15 SELF% SYMBOLS COMPARISON (N=3 Means)")
    print("=" * 90)
    print(f"{'Symbol':60s} | {'CLEAN':8s} | {'T0-MEM':8s} | {'POSTFLUSH':10s} | {'Delta (T0-POST)':14s}")
    print("-" * 90)

    all_self_syms = set(list(agg[("CLEAN", "self")].keys()) + list(agg[("T0-MEM", "self")].keys()) + list(agg[("POSTFLUSH-SST", "self")].keys()))
    self_table = []
    for sym in all_self_syms:
        c_m = sum(agg[("CLEAN", "self")][sym])/3.0 if sym in agg[("CLEAN", "self")] else 0.0
        t_m = sum(agg[("T0-MEM", "self")][sym])/3.0 if sym in agg[("T0-MEM", "self")] else 0.0
        p_m = sum(agg[("POSTFLUSH-SST", "self")][sym])/3.0 if sym in agg[("POSTFLUSH-SST", "self")] else 0.0
        delta = t_m - p_m
        self_table.append((sym, c_m, t_m, p_m, delta))

    self_table.sort(key=lambda x: max(x[2], x[3]), reverse=True)
    for sym, c_m, t_m, p_m, delta in self_table[:15]:
        short_sym = sym.replace("[.] ", "").replace("rocksdb::", "rdb::")
        print(f"{short_sym[:60]:60s} | {c_m:7.2f}% | {t_m:7.2f}% | {p_m:9.2f}% | {delta:+13.2f}%")

    print("\n" + "=" * 90)
    print("  TOP 15 CHILDREN% SYMBOLS COMPARISON (N=3 Means)")
    print("=" * 90)
    print(f"{'Symbol':60s} | {'CLEAN':8s} | {'T0-MEM':8s} | {'POSTFLUSH':10s} | {'Delta (T0-POST)':14s}")
    print("-" * 90)

    all_child_syms = set(list(agg[("CLEAN", "children")].keys()) + list(agg[("T0-MEM", "children")].keys()) + list(agg[("POSTFLUSH-SST", "children")].keys()))
    child_table = []
    for sym in all_child_syms:
        c_m = sum(agg[("CLEAN", "children")][sym])/3.0 if sym in agg[("CLEAN", "children")] else 0.0
        t_m = sum(agg[("T0-MEM", "children")][sym])/3.0 if sym in agg[("T0-MEM", "children")] else 0.0
        p_m = sum(agg[("POSTFLUSH-SST", "children")][sym])/3.0 if sym in agg[("POSTFLUSH-SST", "children")] else 0.0
        delta = t_m - p_m
        child_table.append((sym, c_m, t_m, p_m, delta))

    child_table.sort(key=lambda x: max(x[2], x[3]), reverse=True)
    for sym, c_m, t_m, p_m, delta in child_table[:15]:
        short_sym = sym.replace("[.] ", "").replace("rocksdb::", "rdb::")
        print(f"{short_sym[:60]:60s} | {c_m:7.2f}% | {t_m:7.2f}% | {p_m:9.2f}% | {delta:+13.2f}%")

    # Differential paths
    print("\n" + "=" * 90)
    print("  Top 15 Calling Paths with Largest Shifts: POSTFLUSH-SST -> T0-MEM (difffolded)")
    print("=" * 90)

    diff_entries = []
    if os.path.exists(DIFF_FILE):
        with open(DIFF_FILE) as f:
            for line in f:
                line = line.strip()
                if not line: continue
                parts = line.rsplit(" ", 2)
                if len(parts) == 3:
                    stack, b_cnt, t_cnt = parts[0], parts[1], parts[2]
                    try:
                        delta = int(t_cnt) - int(b_cnt)
                        diff_entries.append((delta, int(b_cnt), int(t_cnt), stack))
                    except: pass

    diff_entries.sort(key=lambda x: abs(x[0]), reverse=True)
    for delta, b_cnt, t_cnt, stack in diff_entries[:15]:
        leaf = stack.split(";")[-1]
        print(f"Delta: {delta:+8d} (PostFlush: {b_cnt:6d} -> T0: {t_cnt:6d}) | Leaf: {leaf[:65]}")

if __name__ == "__main__":
    main()
