#!/usr/bin/env python3
import os
import sys
import json
import csv
import numpy as np
from collections import defaultdict

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
RAW_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3a/raw")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3a")

CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]
REPS = [1, 2, 3]
SEEDS = {1: 510001, 2: 520001, 3: 530001}

def safe_float(v):
    if v is None or v == "":
        return 0.0
    return float(v)

def fmt_stat(vals):
    if not vals:
        return "N/A"
    m = np.mean(vals)
    s = np.std(vals)
    return f"{m:.2f} ± {s:.2f}"

def fmt_int_stat(vals):
    if not vals:
        return "N/A"
    m = np.mean(vals)
    s = np.std(vals)
    return f"{m:.1f} ± {s:.1f}"

def main():
    print("======================================================================")
    print("Aggregating M3a Audit N=3 Results")
    print("======================================================================")

    # Load all raw JSONs
    raw_data = {} # (config, rep) -> dict
    for cfg in CONFIGS:
        cfg_tag = cfg.lower().replace("-", "_")
        for rep in REPS:
            exp_id = f"m3a_audit_{cfg_tag}_rep{rep}"
            json_path = os.path.join(RAW_DIR, f"{exp_id}.json")
            if not os.path.exists(json_path):
                print(f"[WARN] Missing {json_path}")
                continue
            with open(json_path, "r") as f:
                raw_data[(cfg, rep)] = json.load(f)

    if len(raw_data) < 12:
        print(f"[WARN] Only loaded {len(raw_data)}/12 results. Proceeding with available.")

    # 1. Overview & Throughput Summary CSV
    summary_rows = []
    for cfg in CONFIGS:
        pA_iops = []
        pB_iops = []
        pC_iops = []
        pB_sec = []
        flushes = []
        flushed_tb = []
        active_tb = []

        for rep in REPS:
            if (cfg, rep) not in raw_data:
                continue
            d = raw_data[(cfg, rep)]
            flushes.append(d.get("flush_count", 0))
            flushed_tb.append(d.get("flushed_tombstones_total", 0))
            active_tb.append(d.get("active_mem_tombstones", 0))

            phases = d.get("phases", [])
            for p in phases:
                pid = p.get("phase_id", 0)
                if pid == 0:
                    pA_iops.append(p.get("iops", 0))
                elif pid == 1:
                    pB_iops.append(p.get("iops", 0))
                    pB_sec.append(p.get("elapsed_seconds", 0))
                elif pid == 2:
                    pC_iops.append(p.get("iops", 0))

        summary_rows.append({
            "config": cfg,
            "phase_a_iops_mean": np.mean(pA_iops) if pA_iops else 0,
            "phase_a_iops_std": np.std(pA_iops) if pA_iops else 0,
            "phase_b_iops_mean": np.mean(pB_iops) if pB_iops else 0,
            "phase_b_iops_std": np.std(pB_iops) if pB_iops else 0,
            "phase_b_sec_mean": np.mean(pB_sec) if pB_sec else 0,
            "phase_b_sec_std": np.std(pB_sec) if pB_sec else 0,
            "phase_c_iops_mean": np.mean(pC_iops) if pC_iops else 0,
            "phase_c_iops_std": np.std(pC_iops) if pC_iops else 0,
            "flushes_mean": np.mean(flushes) if flushes else 0,
            "flushed_tb_mean": np.mean(flushed_tb) if flushed_tb else 0,
            "active_tb_mean": np.mean(active_tb) if active_tb else 0,
        })

    summary_csv = os.path.join(RESULTS_DIR, "m3a_audit_n3_summary.csv")
    with open(summary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"Wrote {summary_csv}")

    # 2. Detailed per-phase, per-operation breakdown CSV
    # (cfg, phase_id, op_name) -> metrics lists
    op_metrics = defaultdict(lambda: defaultdict(list))
    for (cfg, rep), d in raw_data.items():
        phases = d.get("phases", [])
        for p in phases:
            pid = p.get("phase_id", 0)
            pname = p.get("phase_name", "")
            for op in p.get("operations", []):
                op_name = op.get("op_name", "")
                key = (cfg, pid, pname, op_name)
                op_metrics[key]["count"].append(op.get("count", 0))
                op_metrics[key]["p50_us"].append(op.get("p50_us", 0))
                op_metrics[key]["p95_us"].append(op.get("p95_us", 0))
                op_metrics[key]["p99_us"].append(op.get("p99_us", 0))
                op_metrics[key]["p999_us"].append(op.get("p999_us", 0))
                op_metrics[key]["max_us"].append(op.get("max_us", 0))
                op_metrics[key]["avg_us"].append(op.get("avg_us", 0))
                op_metrics[key]["returned_keys"].append(op.get("total_returned_keys", 0))
                op_metrics[key]["avg_ret_keys"].append(op.get("avg_returned_keys", 0))
                op_metrics[key]["scan_cost_per_key_us"].append(op.get("scan_cost_per_key_us", 0))
                op_metrics[key]["mat_count"].append(op.get("cumulative_thread_side_materialization_count", 0))
                op_metrics[key]["mat_time_ns"].append(op.get("cumulative_thread_side_materialization_nanos", 0))
                op_metrics[key]["lock_contended"].append(op.get("reader_mutex_contended_count", 0))
                op_metrics[key]["lock_wait_ns"].append(op.get("cumulative_thread_side_reader_mutex_wait_nanos", 0))
                op_metrics[key]["reseeks"].append(op.get("scan_reseek_count", 0))
                op_metrics[key]["advances"].append(op.get("scan_boundary_advance_count", 0))
                op_metrics[key]["skips"].append(op.get("scan_covered_skip_count", 0))

    per_phase_ops_rows = []
    for (cfg, pid, pname, op_name), m in sorted(op_metrics.items()):
        per_phase_ops_rows.append({
            "config": cfg,
            "phase_id": pid,
            "phase_name": pname,
            "operation": op_name,
            "count": int(np.mean(m["count"])),
            "p50_us_mean": np.mean(m["p50_us"]),
            "p50_us_std": np.std(m["p50_us"]),
            "p95_us_mean": np.mean(m["p95_us"]),
            "p95_us_std": np.std(m["p95_us"]),
            "p99_us_mean": np.mean(m["p99_us"]),
            "p99_us_std": np.std(m["p99_us"]),
            "p999_us_mean": np.mean(m["p999_us"]),
            "p999_us_std": np.std(m["p999_us"]),
            "max_us_mean": np.mean(m["max_us"]),
            "max_us_std": np.std(m["max_us"]),
            "avg_us_mean": np.mean(m["avg_us"]),
            "avg_us_std": np.std(m["avg_us"]),
            "avg_returned_keys": np.mean(m["avg_ret_keys"]),
            "scan_cost_per_key_us": np.mean(m["scan_cost_per_key_us"]),
            "mat_count_mean": np.mean(m["mat_count"]),
            "mat_time_ms_mean": np.mean(m["mat_time_ns"]) / 1e6, # Cumulative thread-side materialization time in ms
            "lock_contended_mean": np.mean(m["lock_contended"]),
            "lock_wait_ms_mean": np.mean(m["lock_wait_ns"]) / 1e6, # Cumulative thread-side lock wait time in ms
            "reseeks_mean": np.mean(m["reseeks"]),
        })

    ops_csv = os.path.join(RESULTS_DIR, "m3a_audit_n3_per_phase_ops.csv")
    with open(ops_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(per_phase_ops_rows[0].keys()))
        writer.writeheader()
        writer.writerows(per_phase_ops_rows)
    print(f"Wrote {ops_csv}")

    # 3. T512 Generation Details CSV
    gen_rows = []
    for cfg in ["Native-T512", "AMTV-T512"]:
        for rep in REPS:
            if (cfg, rep) not in raw_data:
                continue
            d = raw_data[(cfg, rep)]
            gens = d.get("t512_generations", [])
            cum_tb = 0
            for g in gens:
                rds = g.get("range_deletions", 0)
                cum_tb += rds
                gen_rows.append({
                    "config": cfg,
                    "rep": rep,
                    "generation": g.get("generation", 0),
                    "range_deletions": rds,
                    "cumulative_flushed_tombstones": cum_tb,
                    "flush_reason": g.get("flush_reason", ""),
                    "is_active_generation": False
                })
            active_tb = d.get("active_mem_tombstones", 0)
            gen_rows.append({
                "config": cfg,
                "rep": rep,
                "generation": len(gens) + 1,
                "range_deletions": active_tb,
                "cumulative_flushed_tombstones": cum_tb + active_tb,
                "flush_reason": "Active MemTable Residual",
                "is_active_generation": True
            })

    gen_csv = os.path.join(RESULTS_DIR, "m3a_audit_n3_t512_generations.csv")
    if gen_rows:
        with open(gen_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(gen_rows[0].keys()))
            writer.writeheader()
            writer.writerows(gen_rows)
        print(f"Wrote {gen_csv}")

    # 4. Generate Markdown Summary Tables for Report
    print("\n" + "=" * 70)
    print("M3a AUDIT N=3 SUMMARY REPORT TABLES")
    print("=" * 70)

    print("\n### 1. 吞吐率与阶段执行时间（Mean ± Std across N=3）\n")
    print("| 配置 | Phase A IOPS | Phase B IOPS | Phase B 耗时 (s) | Phase C IOPS | Flush 次数 | 已刷墓碑数 | 活跃代墓碑数 |")
    print("|:---|---:|---:|---:|---:|---:|---:|---:|")
    for r in summary_rows:
        print(f"| {r['config']} | {r['phase_a_iops_mean']:.0f} ± {r['phase_a_iops_std']:.0f} | "
              f"{r['phase_b_iops_mean']:.0f} ± {r['phase_b_iops_std']:.0f} | "
              f"{r['phase_b_sec_mean']:.2f} ± {r['phase_b_sec_std']:.2f} | "
              f"{r['phase_c_iops_mean']:.0f} ± {r['phase_c_iops_std']:.0f} | "
              f"{r['flushes_mean']:.1f} | {r['flushed_tb_mean']:.0f} | {r['active_tb_mean']:.0f} |")

    print("\n### 2. Phase B (动态写入期) 逐操作延迟与底层开销（Mean across N=3）\n")
    print("| 配置 | 操作类型 | 样本数 | P50 (us) | P99 (us) | Max (us) | 物化次数 | 累计线程侧物化计时 (ms) | 争用次数 | 累计线程侧等待 (ms) | 墓碑重寻次数 |")
    print("|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in per_phase_ops_rows:
        if r["phase_id"] == 1:
            print(f"| {r['config']} | `{r['operation']}` | {r['count']:,} | {r['p50_us_mean']:.2f} | {r['p99_us_mean']:.2f} | {r['max_us_mean']:.2f} | "
                  f"{r['mat_count_mean']:.0f} | {r['mat_time_ms_mean']:.2f} | {r['lock_contended_mean']:.0f} | {r['lock_wait_ms_mean']:.2f} | {r['reseeks_mean']:.0f} |")

    print("\n### 3. Phase C (静态存活期) 逐操作延迟与底层开销（Mean across N=3）\n")
    print("| 配置 | 操作类型 | 样本数 | P50 (us) | P99 (us) | Max (us) | 物化次数 | 累计线程侧物化计时 (ms) | 争用次数 | 累计线程侧等待 (ms) | 墓碑重寻次数 |")
    print("|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in per_phase_ops_rows:
        if r["phase_id"] == 2:
            print(f"| {r['config']} | `{r['operation']}` | {r['count']:,} | {r['p50_us_mean']:.2f} | {r['p99_us_mean']:.2f} | {r['max_us_mean']:.2f} | "
                  f"{r['mat_count_mean']:.0f} | {r['mat_time_ms_mean']:.2f} | {r['lock_contended_mean']:.0f} | {r['lock_wait_ms_mean']:.2f} | {r['reseeks_mean']:.0f} |")

if __name__ == "__main__":
    main()
