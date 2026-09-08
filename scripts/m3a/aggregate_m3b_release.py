#!/usr/bin/env python3
"""
M3b Release N=5 Aggregator
Computes:
1. Overall & Phase-specific throughput, latency, and duration metrics (Mean, Std, Median, Q1, Q3, IQR).
2. Per-phase, per-operation breakdown (GetLive, Scan-PlannedIntersect, Scan-Intersect, Scan-NonIntersect, Put, DeleteRange).
3. Exact visible scan keys validation across all seeds.
4. T512 generation flush causes, tombstone conservation, and active generation leftovers.
5. Three-window I/O (Flush, Compaction Write/Read) and Engine Write Amplification Factor (normalized by 60,000 Puts * 256 B = 15.36 MB).
6. Paired differences and speedup ratios per Rep (Native-T0 vs AMTV-T0, Native-T512 vs AMTV-T512).
7. Produces CSV tables and markdown-ready summary blocks.
"""

import os
import sys
import json
import csv
import numpy as np
from collections import defaultdict

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
RAW_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3b/raw")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3b")

CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]
REPS = [1, 2, 3, 4, 5]
SEEDS = {1: 610001, 2: 620001, 3: 630001, 4: 640001, 5: 650001}
NORMALIZED_PUT_BYTES = 60000 * 256  # 15,360,000 bytes (14.65 MiB)

def compute_iqr_stats(data):
    if not data:
        return {"mean": 0.0, "std": 0.0, "median": 0.0, "q1": 0.0, "q3": 0.0, "iqr": 0.0}
    arr = np.array(data, dtype=float)
    q1 = np.percentile(arr, 25)
    q3 = np.percentile(arr, 75)
    return {
        "mean": float(np.mean(arr)),
        "std": float(np.std(arr)),
        "median": float(np.median(arr)),
        "q1": float(q1),
        "q3": float(q3),
        "iqr": float(q3 - q1)
    }

def main():
    print("======================================================================")
    print("Aggregating M3b Release N=5 Results")
    print("======================================================================")

    # 1. Load Raw JSONs
    raw_data = {}  # (config, rep) -> dict
    for cfg in CONFIGS:
        cfg_tag = cfg.lower().replace("-", "_")
        for rep in REPS:
            exp_id = f"m3b_release_{cfg_tag}_rep{rep}"
            json_path = os.path.join(RAW_DIR, f"{exp_id}.json")
            if not os.path.exists(json_path):
                print(f"[WARN] Missing {json_path}")
                continue
            with open(json_path, "r") as f:
                raw_data[(cfg, rep)] = json.load(f)

    loaded_count = len(raw_data)
    print(f"Loaded {loaded_count}/20 raw JSON results.")
    if loaded_count < 20:
        print(f"[ERROR] Incomplete run set ({loaded_count} < 20). Cannot aggregate partial matrix.")
        sys.exit(1)

    os.makedirs(RESULTS_DIR, exist_ok=True)

    # 2. State & Invariant Verification Matrix
    print("\n--- Verifying Invariants across all 20 runs ---")
    rep_shas = {}
    for (cfg, rep), d in sorted(raw_data.items(), key=lambda x: (x[0][1], x[0][0])):
        seed = SEEDS[rep]
        sha = d["db_state_sha256"]
        if rep not in rep_shas:
            rep_shas[rep] = sha
        else:
            assert sha == rep_shas[rep], f"SHA mismatch in Rep {rep} ({cfg}): {sha} != {rep_shas[rep]}"
        assert d["verified_live_keys"] == 300000
        assert d["verified_deleted_keys"] == 200000
        assert d["iterator_scan_visible_keys"] == 300000
        if "AMTV" in cfg:
            assert d["fallback_events"] == 0
        if "T0" in cfg:
            assert d["flush_count"] == 0
            assert d["active_mem_tombstones"] == 20000
        elif "T512" in cfg:
            assert d["flushed_tombstones_total"] + d["active_mem_tombstones"] == 20000
    print("[PASS] All 20 runs strictly satisfy 500K State model, zero fallback, and tombstone conservation.")

    # 3. Throughput & Stage Duration Summary CSV with IQR
    summary_rows = []
    for cfg in CONFIGS:
        pA_iops, pB_iops, pC_iops, overall_iops = [], [], [], []
        pA_sec, pB_sec, pC_sec, total_sec = [], [], [], []
        flushes, flushed_tb, active_tb = [], [], []
        flush_bytes, comp_write_bytes, comp_read_bytes, wafs = [], [], [], []

        for rep in REPS:
            d = raw_data[(cfg, rep)]
            flushes.append(d["flush_count"])
            flushed_tb.append(d["flushed_tombstones_total"])
            active_tb.append(d["active_mem_tombstones"])

            phases = d["phases"]
            pa, pb, pc = phases[0], phases[1], phases[2]
            pA_iops.append(pa["iops"])
            pB_iops.append(pb["iops"])
            pC_iops.append(pc["iops"])

            pA_sec.append(pa["elapsed_seconds"])
            pB_sec.append(pb["elapsed_seconds"])
            pC_sec.append(pc["elapsed_seconds"])

            tot_s = pa["elapsed_seconds"] + pb["elapsed_seconds"] + pc["elapsed_seconds"]
            total_sec.append(tot_s)
            overall_iops.append(300000.0 / tot_s)

            tw = d["three_window_io"]
            fb = tw["three_window_total_flush_bytes"]
            cwb = tw["three_window_total_compaction_write_bytes"]
            crb = tw.get("three_window_total_compaction_read_bytes", 0)
            flush_bytes.append(fb)
            comp_write_bytes.append(cwb)
            comp_read_bytes.append(crb)
            engine_write_bytes = fb + cwb
            wafs.append(engine_write_bytes / NORMALIZED_PUT_BYTES)

        row = {
            "config": cfg,
            "phase_a_iops_mean": np.mean(pA_iops),
            "phase_a_iops_std": np.std(pA_iops),
            "phase_a_iops_median": np.median(pA_iops),
            "phase_a_iops_iqr": np.percentile(pA_iops, 75) - np.percentile(pA_iops, 25),
            "phase_b_iops_mean": np.mean(pB_iops),
            "phase_b_iops_std": np.std(pB_iops),
            "phase_b_iops_median": np.median(pB_iops),
            "phase_b_iops_iqr": np.percentile(pB_iops, 75) - np.percentile(pB_iops, 25),
            "phase_b_sec_mean": np.mean(pB_sec),
            "phase_b_sec_std": np.std(pB_sec),
            "phase_b_sec_median": np.median(pB_sec),
            "phase_b_sec_iqr": np.percentile(pB_sec, 75) - np.percentile(pB_sec, 25),
            "phase_c_iops_mean": np.mean(pC_iops),
            "phase_c_iops_std": np.std(pC_iops),
            "phase_c_iops_median": np.median(pC_iops),
            "phase_c_iops_iqr": np.percentile(pC_iops, 75) - np.percentile(pC_iops, 25),
            "overall_iops_mean": np.mean(overall_iops),
            "overall_iops_std": np.std(overall_iops),
            "overall_iops_median": np.median(overall_iops),
            "overall_iops_iqr": np.percentile(overall_iops, 75) - np.percentile(overall_iops, 25),
            "flushes_mean": np.mean(flushes),
            "flushed_tb_mean": np.mean(flushed_tb),
            "active_tb_mean": np.mean(active_tb),
            "three_win_flush_mb_mean": np.mean(flush_bytes) / (1024 * 1024),
            "three_win_comp_write_mb_mean": np.mean(comp_write_bytes) / (1024 * 1024),
            "waf_mean": np.mean(wafs),
            "waf_std": np.std(wafs),
        }
        summary_rows.append(row)

    summary_csv = os.path.join(RESULTS_DIR, "m3b_release_n5_throughput_summary.csv")
    with open(summary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"Wrote {summary_csv}")

    # 4. Detailed Per-Phase, Per-Operation Latencies (P50, P95, P99, P99.9, Max)
    # Group: (config, phase_id, op_name) -> list of metrics across 5 reps
    op_data = defaultdict(lambda: defaultdict(list))
    for (cfg, rep), d in raw_data.items():
        for p in d["phases"]:
            pid = p["phase_id"]
            pname = p["phase_name"]
            for op in p["operations"]:
                op_name = op["op_name"]
                key = (cfg, pid, pname, op_name)
                op_data[key]["count"].append(op["count"])
                op_data[key]["p50_us"].append(op["p50_us"])
                op_data[key]["p95_us"].append(op["p95_us"])
                op_data[key]["p99_us"].append(op["p99_us"])
                op_data[key]["p999_us"].append(op["p999_us"])
                op_data[key]["max_us"].append(op["max_us"])
                op_data[key]["avg_us"].append(op["avg_us"])
                op_data[key]["total_returned_keys"].append(op["total_returned_keys"])
                op_data[key]["avg_returned_keys"].append(op["avg_returned_keys"])
                op_data[key]["scan_cost_per_key_us"].append(op["scan_cost_per_key_us"])

    per_phase_ops_rows = []
    for (cfg, pid, pname, op_name), metrics in sorted(op_data.items()):
        per_phase_ops_rows.append({
            "config": cfg,
            "phase_id": pid,
            "phase_name": pname,
            "op_name": op_name,
            "count": int(np.mean(metrics["count"])),
            "p50_us_mean": np.mean(metrics["p50_us"]),
            "p50_us_median": np.median(metrics["p50_us"]),
            "p50_us_iqr": np.percentile(metrics["p50_us"], 75) - np.percentile(metrics["p50_us"], 25),
            "p95_us_mean": np.mean(metrics["p95_us"]),
            "p99_us_mean": np.mean(metrics["p99_us"]),
            "p99_us_median": np.median(metrics["p99_us"]),
            "p99_us_iqr": np.percentile(metrics["p99_us"], 75) - np.percentile(metrics["p99_us"], 25),
            "p999_us_mean": np.mean(metrics["p999_us"]),
            "max_us_mean": np.mean(metrics["max_us"]),
            "avg_us_mean": np.mean(metrics["avg_us"]),
            "ret_keys_total": int(np.mean(metrics["total_returned_keys"])),
            "avg_returned_keys": np.mean(metrics["avg_returned_keys"]),
            "scan_cost_per_key_us": np.mean(metrics["scan_cost_per_key_us"]),
        })

    ops_csv = os.path.join(RESULTS_DIR, "m3b_release_n5_per_phase_ops.csv")
    with open(ops_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(per_phase_ops_rows[0].keys()))
        writer.writeheader()
        writer.writerows(per_phase_ops_rows)
    print(f"Wrote {ops_csv}")

    # 5. Paired Differences & Ratios per Rep
    # Rep by Rep:
    # Pair A: Native-T0 vs AMTV-T0 (Primary mechanism comparison)
    # Pair B: Native-T512 vs AMTV-T512 (End-to-end configuration comparison)
    paired_rows = []
    for rep in REPS:
        nat_t0 = raw_data[("Native-T0", rep)]
        amtv_t0 = raw_data[("AMTV-T0", rep)]
        nat_t512 = raw_data[("Native-T512", rep)]
        amtv_t512 = raw_data[("AMTV-T512", rep)]

        # Phase B IOPS and durations
        pB_nat_t0_iops = nat_t0["phases"][1]["iops"]
        pB_amtv_t0_iops = amtv_t0["phases"][1]["iops"]
        pB_t0_ratio = pB_amtv_t0_iops / pB_nat_t0_iops

        pB_nat_t0_sec = nat_t0["phases"][1]["elapsed_seconds"]
        pB_amtv_t0_sec = amtv_t0["phases"][1]["elapsed_seconds"]
        pB_t0_time_reduction = pB_nat_t0_sec - pB_amtv_t0_sec
        pB_t0_time_ratio = pB_nat_t0_sec / pB_amtv_t0_sec

        # Overall IOPS
        tot_s_nat_t0 = sum(p["elapsed_seconds"] for p in nat_t0["phases"])
        tot_s_amtv_t0 = sum(p["elapsed_seconds"] for p in amtv_t0["phases"])
        overall_t0_ratio = (300000.0 / tot_s_amtv_t0) / (300000.0 / tot_s_nat_t0)

        # T512 comparison
        pB_nat_t512_iops = nat_t512["phases"][1]["iops"]
        pB_amtv_t512_iops = amtv_t512["phases"][1]["iops"]
        pB_t512_ratio = pB_amtv_t512_iops / pB_nat_t512_iops

        tot_s_nat_t512 = sum(p["elapsed_seconds"] for p in nat_t512["phases"])
        tot_s_amtv_t512 = sum(p["elapsed_seconds"] for p in amtv_t512["phases"])
        overall_t512_ratio = (300000.0 / tot_s_amtv_t512) / (300000.0 / tot_s_nat_t512)

        paired_rows.append({
            "rep": rep,
            "seed": SEEDS[rep],
            "t0_phase_b_nat_iops": pB_nat_t0_iops,
            "t0_phase_b_amtv_iops": pB_amtv_t0_iops,
            "t0_phase_b_iops_ratio": pB_t0_ratio,
            "t0_phase_b_nat_sec": pB_nat_t0_sec,
            "t0_phase_b_amtv_sec": pB_amtv_t0_sec,
            "t0_phase_b_sec_diff": pB_t0_time_reduction,
            "t0_phase_b_sec_ratio": pB_t0_time_ratio,
            "t0_overall_iops_ratio": overall_t0_ratio,
            "t512_phase_b_nat_iops": pB_nat_t512_iops,
            "t512_phase_b_amtv_iops": pB_amtv_t512_iops,
            "t512_phase_b_iops_ratio": pB_t512_ratio,
            "t512_overall_iops_ratio": overall_t512_ratio,
        })

    paired_csv = os.path.join(RESULTS_DIR, "m3b_release_n5_paired_diffs.csv")
    with open(paired_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(paired_rows[0].keys()))
        writer.writeheader()
        writer.writerows(paired_rows)
    print(f"Wrote {paired_csv}")

    # 6. T512 Generation Details CSV
    gen_rows = []
    for cfg in ["Native-T512", "AMTV-T512"]:
        for rep in REPS:
            d = raw_data[(cfg, rep)]
            for g in d.get("t512_generations", []):
                gen_rows.append({
                    "config": cfg,
                    "rep": rep,
                    "seed": SEEDS[rep],
                    "generation": g["generation"],
                    "range_deletions": g["range_deletions"],
                    "flush_reason": g["flush_reason"],
                })

    gen_csv = os.path.join(RESULTS_DIR, "m3b_release_n5_t512_generations.csv")
    with open(gen_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(gen_rows[0].keys()))
        writer.writeheader()
        writer.writerows(gen_rows)
    print(f"Wrote {gen_csv}")

    print("\nAll aggregations successfully written to results/amtv_m3b/!")

if __name__ == '__main__':
    main()
