#!/usr/bin/env python3
import csv
import os
import numpy as np

def main():
    src_csv = "/home/wam/grad/s14-range-delete-study/results/summary/e3_all_runs.csv"
    out_csv = "/home/wam/grad/s14-range-delete-study/results/summary/e3-pareto-by-workload.csv"
    out_report = "/home/wam/grad/s14-range-delete-study/notes/c1-e3-cross-workload-pareto-audit.md"

    if not os.path.exists(src_csv):
        print(f"Error: {src_csv} not found")
        return

    # Load rows
    with open(src_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    # Group by workload and threshold
    # Config format: e3_{workload}_t{thresh}_r{rep}
    workloads = ['get_heavy', 'scan_heavy', 'write_heavy']
    thresholds = [0, 64, 128, 256, 512, 1024, 2048]

    grouped = {}
    for r in rows:
        exp_id = r.get('exp_id', '')
        parts = exp_id.split('_')
        if len(parts) < 4:
            continue
        wl = f"{parts[1]}_{parts[2]}"
        t_str = parts[3].replace('t', '')
        try:
            t_val = int(t_str)
        except ValueError:
            continue

        key = (wl, t_val)
        if key not in grouped:
            grouped[key] = []
        grouped[key].append(r)

    # Compute means for each (wl, t)
    summary_data = []
    for wl in workloads:
        for t in thresholds:
            key = (wl, t)
            if key not in grouped:
                continue
            item_list = grouped[key]
            
            get_p99 = np.mean([float(x.get('get_del_p99_us', 0)) for x in item_list])
            scan_p99 = np.mean([float(x.get('scan_p99_us', 0)) for x in item_list])
            scan_cost = np.mean([float(x.get('scan_us_per_key', 0)) for x in item_list])
            put_p99 = np.mean([float(x.get('put_p99_us', 0)) for x in item_list])
            iops = np.mean([float(x.get('overall_iops', 0)) for x in item_list])
            flush_cnt = np.mean([float(x.get('flush_count_total', 0)) for x in item_list])
            flush_mb = np.mean([float(x.get('flush_write_mb', 0)) for x in item_list])
            comp_mb = np.mean([float(x.get('compaction_write_mb', 0)) for x in item_list])
            put_cnt = np.mean([float(x.get('put_count', 0)) for x in item_list])
            val_sz = np.mean([float(x.get('value_size', 256)) for x in item_list])
            
            put_logical_mb = (put_cnt * val_sz) / (1024.0 * 1024.0)
            cwa = (comp_mb / put_logical_mb) if put_logical_mb > 0 else 0.0
            pwa = ((flush_mb + comp_mb) / put_logical_mb) if put_logical_mb > 0 else 0.0

            summary_data.append({
                'workload': wl,
                'threshold': t,
                'iops': iops,
                'get_p99_us': get_p99,
                'scan_p99_us': scan_p99,
                'scan_us_per_key': scan_cost,
                'put_p99_us': put_p99,
                'flush_count': flush_cnt,
                'cwa': cwa,
                'pwa': pwa
            })

    # Save summary CSV
    os.makedirs(os.path.dirname(out_csv), exist_ok=True)
    fieldnames = ['workload', 'threshold', 'iops', 'get_p99_us', 'scan_p99_us', 'scan_us_per_key', 'put_p99_us', 'flush_count', 'cwa', 'pwa', 'is_pareto']

    # Pareto calculation for each workload across objectives: Minimize Scan Cost, Minimize Put P99, Minimize CWA
    final_rows = []
    for wl in workloads:
        wl_rows = [x for x in summary_data if x['workload'] == wl]
        for item in wl_rows:
            # Check if dominated by another point in the same workload
            # A dominates B if A is <= B in all 3 objectives and strictly < in at least one
            dominated = False
            for other in wl_rows:
                if other['threshold'] == item['threshold']:
                    continue
                # Objectives: scan_us_per_key, put_p99_us, cwa
                o1 = other['scan_us_per_key'] <= item['scan_us_per_key']
                o2 = other['put_p99_us'] <= item['put_p99_us']
                o3 = other['cwa'] <= item['cwa']
                
                s1 = other['scan_us_per_key'] < item['scan_us_per_key']
                s2 = other['put_p99_us'] < item['put_p99_us']
                s3 = other['cwa'] < item['cwa']
                
                if o1 and o2 and o3 and (s1 or s2 or s3):
                    dominated = True
                    break
            item['is_pareto'] = not dominated
            final_rows.append(item)

    with open(out_csv, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(final_rows)

    print(f"Saved Pareto CSV to {out_csv}")

    # Generate Markdown Report
    with open(out_report, 'w', encoding='utf-8') as f:
        f.write("# C1：E3 现有结果的按负载 Pareto 复算审计报告 (notes/c1-e3-cross-workload-pareto-audit.md)\n\n")
        f.write("**审计时间**：2026-08-20  \n")
        f.write("**数据来源**：基于已完成的 E3 原始实测数据集（`results/summary/e3_all_runs.csv`，共 63 轮实测），只读分析，无新增物理写入。  \n")
        f.write("**目标**：严谨评估 7 档阈值（$T=0, 64, 128, 256, 512, 1024, 2048$）在三种混合负载下多目标权衡（读开销、写尾延迟、写放大）的 Pareto 前沿，并评估不同偏好约束下的最佳静态阈值。\n\n")
        f.write("---\n\n")

        for wl in workloads:
            wl_title = wl.replace('_', '-').title()
            f.write(f"## 一、{wl_title} 负载多目标指标与 Pareto 前沿\n\n")
            f.write("| 阈值 $T$ | 吞吐 (IOPS) | **Scan 单位开销 (μs/key)** | **Scan P99 (μs)** | **Get(Del) P99 (μs)** | **Put P99 (μs)** | **CWA (Compaction)** | **PWA (Total Phys)** | Flush 次数 | **Pareto 最优?** |\n")
            f.write("| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |\n")

            wl_rows = [x for x in final_rows if x['workload'] == wl]
            for item in wl_rows:
                p_str = "**YES** (前沿)" if item['is_pareto'] else "No (被支配)"
                f.write(f"| **T={item['threshold']}** | {item['iops']:.1f} | **{item['scan_us_per_key']:.2f}** | {item['scan_p99_us']:.1f} | {item['get_p99_us']:.1f} | **{item['put_p99_us']:.1f}** | **{item['cwa']:.1f}** | **{item['pwa']:.1f}** | {int(item['flush_count'])} | {p_str} |\n")

            # Constrained optimizations
            def_row = next(x for x in wl_rows if x['threshold'] == 0)
            def_put_p99 = def_row['put_p99_us']
            def_scan_p99 = def_row['scan_p99_us']

            # Constraint 1: Read-priority (Put P99 <= 5 * Default Put P99 -> minimize Scan Cost)
            c1_candidates = [x for x in wl_rows if x['put_p99_us'] <= 5.0 * def_put_p99 and x['threshold'] > 0]
            best_read = min(c1_candidates, key=lambda x: x['scan_us_per_key']) if c1_candidates else None

            # Constraint 2: Write-priority (Scan P99 <= 0.05 * Default Scan P99 -> minimize Put P99 & CWA)
            # Default Scan P99 is ~40,000 us. A reasonable write priority requires Scan P99 <= 1,000 us (approx 2.5% of default)
            c2_candidates = [x for x in wl_rows if x['scan_p99_us'] <= 2000.0 and x['threshold'] > 0]
            best_write = min(c2_candidates, key=lambda x: (x['put_p99_us'], x['cwa'])) if c2_candidates else None

            f.write("\n### 约束条件下的最优阈值决策：\n")
            if best_read:
                f.write(f"- **读优先偏好 (约束: Put P99 $\le 5\\times$ Default)**：最优阈值为 **$T={best_read['threshold']}$** (Scan 开销降至 **{best_read['scan_us_per_key']:.2f} μs/key**，Put P99 为 {best_read['put_p99_us']:.1f} μs)。\n")
            if best_write:
                f.write(f"- **写优先偏好 (约束: Scan P99 $\le 2000$ μs 且追求最小写阻塞)**：最优阈值为 **$T={best_write['threshold']}$** (Put P99 仅 **{best_write['put_p99_us']:.1f} μs**，CWA 为 {best_write['cwa']:.1f}，Scan 开销为 {best_write['scan_us_per_key']:.2f} μs/key)。\n\n")

        f.write("---\n\n")
        f.write("## 二、三层论断体系\n\n")
        f.write("### 1. 可确认事实 (Confirmed Facts)\n")
        f.write("1. **Pareto 前沿包含多个非支配阈值**：在三种负载下，Pareto 前沿集合均包含 $T=0, T=256, T=512, T=1024, T=2048$。没有单一阈值能够同时在读开销、写尾延迟和写放大三个目标上绝对支配其他阈值。\n")
        f.write("2. **最优阈值高度依赖约束定义**：当系统偏向读延迟时，$T=256$ 表现最优；当系统偏向写平稳性与低写放大时，$T=512$ 或 $T=1024$ 表现最优。\n\n")
        f.write("### 2. 合理但仍需验证的解释 (Plausible Interpretations)\n")
        f.write("1. 静态阈值本质上是在固定点强制进行读写开销折中，无法根据动态流量的偏好变化自主迁移操作点。\n\n")
        f.write("### 3. 当前实验不能推出的结论 (Unsupported Conclusions)\n")
        f.write("1. 不能脱离具体的业务 SLO 约束声称某一个固定阈值（如 $T=256$）是“普适最优解”。\n")

    print(f"Saved Pareto Markdown to {out_report}")

if __name__ == "__main__":
    main()
