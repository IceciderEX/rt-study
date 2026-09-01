#!/usr/bin/env python3
import csv
import os
import numpy as np

def main():
    src_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24_density_clean_all_runs.csv"
    prog_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24_density_clean_timeseries_progress.csv"
    out_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24-density-preserved-clean.csv"
    phases_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24-density-preserved-clean-phases.csv"

    if not os.path.exists(src_csv):
        print(f"Error: {src_csv} not found")
        return

    with open(src_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    if not rows:
        return

    scan_costs = [float(x.get('scan_us_per_key', 0)) for x in rows]
    scan_p99s = [float(x.get('scan_p99_us', 0)) for x in rows]
    get_ctrl_p99s = [float(x.get('get_ctrl_p99_us', 0)) for x in rows]
    put_p99s = [float(x.get('put_p99_us', 0)) for x in rows]
    flush_cnts = [float(x.get('flush_count_total', 0)) for x in rows]
    sst_mbs = [float(x.get('total_sst_mb', 0)) for x in rows]

    out_row = {
        'benchmark': 'LS24-DensityPreserved-Clean (Same Trajectory, No DeleteRange)',
        'runs_count': len(rows),
        'scan_us_per_key_mean': float(np.mean(scan_costs)),
        'scan_us_per_key_std': float(np.std(scan_costs)),
        'scan_p99_us_mean': float(np.mean(scan_p99s)),
        'scan_p99_us_std': float(np.std(scan_p99s)),
        'get_ctrl_p99_us_mean': float(np.mean(get_ctrl_p99s)),
        'get_ctrl_p99_us_std': float(np.std(get_ctrl_p99s)),
        'put_p99_us_mean': float(np.mean(put_p99s)),
        'put_p99_us_std': float(np.std(put_p99s)),
        'flush_count_mean': float(np.mean(flush_cnts)),
        'flush_count_std': float(np.std(flush_cnts)),
        'total_sst_mb_mean': float(np.mean(sst_mbs)),
        'total_sst_mb_std': float(np.std(sst_mbs))
    }

    os.makedirs(os.path.dirname(out_csv), exist_ok=True)
    with open(out_csv, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(out_row.keys()))
        writer.writeheader()
        writer.writerow(out_row)
    print(f"Saved LS24-DensityPreserved-Clean summary to {out_csv}")

    # Phase Breakdown
    if os.path.exists(prog_csv):
        with open(prog_csv, 'r', encoding='utf-8') as f:
            p_rows = list(csv.DictReader(f))

        phases = [
            ('Phase A (Read Sensitive)', 1, 33),
            ('Phase B (Write Burst)', 34, 66),
            ('Phase C (Read Recovery)', 67, 100)
        ]
        
        breakdown_data = []
        run_ids = sorted(list(set(r['exp_id'] for r in p_rows)))
        
        for p_name, p_start, p_end in phases:
            scan_costs, scan_p99s, put_p99s, get_p99s, iops_list = [], [], [], [], []
            for rid in run_ids:
                pr = [r for r in p_rows if r['exp_id'] == rid]
                t_start = 0.0
                ops_start = 0
                if p_start > 1:
                    r_prev = [r for r in pr if int(r['progress_pct_idx']) == p_start - 1]
                    if r_prev:
                        t_start = float(r_prev[0]['elapsed_sec'])
                        ops_start = int(r_prev[0]['completed_ops'])
                r_end = [r for r in pr if int(r['progress_pct_idx']) == p_end]
                if r_end:
                    t_end = float(r_end[0]['elapsed_sec'])
                    ops_end = int(r_end[0]['completed_ops'])
                    dt = t_end - t_start
                    dops = ops_end - ops_start
                    if dt > 0:
                        iops_list.append(dops / dt)

                pr_window = [r for r in pr if p_start <= int(r['progress_pct_idx']) <= p_end]
                if pr_window:
                    scan_costs.append(np.mean([float(x['scan_us_per_key']) for x in pr_window]))
                    scan_p99s.append(np.percentile([float(x['scan_p99_us']) for x in pr_window], 95))
                    put_p99s.append(np.percentile([float(x['put_p99_us']) for x in pr_window], 95))
                    get_p99s.append(np.percentile([float(x['get_ctrl_p99_us']) for x in pr_window], 95))

            breakdown_data.append({
                'benchmark': 'LS24-DensityPreserved-Clean',
                'phase': p_name,
                'phase_true_iops_mean': float(np.mean(iops_list)) if iops_list else 0.0,
                'phase_true_iops_std': float(np.std(iops_list)) if iops_list else 0.0,
                'scan_us_per_key_mean': float(np.mean(scan_costs)) if scan_costs else 0.0,
                'scan_us_per_key_std': float(np.std(scan_costs)) if scan_costs else 0.0,
                'scan_p99_us_mean': float(np.mean(scan_p99s)) if scan_p99s else 0.0,
                'scan_p99_us_std': float(np.std(scan_p99s)) if scan_p99s else 0.0,
                'get_ctrl_p99_us_mean': float(np.mean(get_p99s)) if get_p99s else 0.0,
                'get_ctrl_p99_us_std': float(np.std(get_p99s)) if get_p99s else 0.0,
                'put_p99_us_mean': float(np.mean(put_p99s)) if put_p99s else 0.0,
                'put_p99_us_std': float(np.std(put_p99s)) if put_p99s else 0.0
            })

        with open(phases_csv, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=list(breakdown_data[0].keys()))
            writer.writeheader()
            writer.writerows(breakdown_data)
        print(f"Saved LS24-DensityPreserved-Clean phase breakdown to {phases_csv}")

if __name__ == "__main__":
    main()
