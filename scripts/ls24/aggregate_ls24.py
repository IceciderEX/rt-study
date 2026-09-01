#!/usr/bin/env python3
import csv
import os
import numpy as np

def main():
    src_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24_all_runs.csv"
    prog_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24_timeseries_progress.csv"
    main_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24-main.csv"
    phases_csv = "/home/wam/grad/s14-range-delete-study/results/summary/ls24-phases-breakdown.csv"

    if not os.path.exists(src_csv):
        print(f"Error: {src_csv} not found")
        return

    with open(src_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    thresholds = [0, 256, 2048]
    grouped = {t: [] for t in thresholds}

    for r in rows:
        exp_id = r.get('exp_id', '')
        for t in thresholds:
            t_str = f"t{t:04d}"
            if t_str in exp_id:
                grouped[t].append(r)
                break

    out_rows = []
    for t in thresholds:
        item_list = grouped[t]
        if not item_list:
            continue
        
        iops_list = [float(x.get('overall_iops', 0)) for x in item_list]
        scan_cost_list = [float(x.get('scan_us_per_key', 0)) for x in item_list]
        scan_p99_list = [float(x.get('scan_p99_us', 0)) for x in item_list]
        put_p99_list = [float(x.get('put_p99_us', 0)) for x in item_list]
        get_p99_list = [float(x.get('get_del_p99_us', 0)) for x in item_list]
        flush_list = [float(x.get('flush_count_total', 0)) for x in item_list]
        drop_list = [float(x.get('compaction_drop_keys', 0)) for x in item_list]
        sst_list = [float(x.get('total_sst_mb', 0)) for x in item_list]

        cwa_list = []
        pwa_list = []
        for x in item_list:
            put_cnt = float(x.get('put_count', 0))
            val_sz = float(x.get('value_size', 256))
            put_logical_mb = (put_cnt * val_sz) / (1024.0 * 1024.0)
            flush_mb = float(x.get('flush_write_mb', 0))
            comp_mb = float(x.get('compaction_write_mb', 0))
            cwa_list.append(comp_mb / put_logical_mb if put_logical_mb > 0 else 0.0)
            pwa_list.append((flush_mb + comp_mb) / put_logical_mb if put_logical_mb > 0 else 0.0)

        out_rows.append({
            'threshold': t,
            'runs_count': len(item_list),
            'iops_mean': float(np.mean(iops_list)),
            'iops_std': float(np.std(iops_list)),
            'scan_us_per_key_mean': float(np.mean(scan_cost_list)),
            'scan_us_per_key_std': float(np.std(scan_cost_list)),
            'scan_p99_us_mean': float(np.mean(scan_p99_list)),
            'scan_p99_us_std': float(np.std(scan_p99_list)),
            'put_p99_us_mean': float(np.mean(put_p99_list)),
            'put_p99_us_std': float(np.std(put_p99_list)),
            'get_del_p99_us_mean': float(np.mean(get_p99_list)),
            'get_del_p99_us_std': float(np.std(get_p99_list)),
            'flush_count_mean': float(np.mean(flush_list)),
            'flush_count_std': float(np.std(flush_list)),
            'cwa_mean': float(np.mean(cwa_list)),
            'cwa_std': float(np.std(cwa_list)),
            'pwa_mean': float(np.mean(pwa_list)),
            'pwa_std': float(np.std(pwa_list)),
            'compaction_drop_keys_mean': float(np.mean(drop_list)),
            'compaction_drop_keys_std': float(np.std(drop_list)),
            'total_sst_mb_mean': float(np.mean(sst_list)),
            'total_sst_mb_std': float(np.std(sst_list))
        })

    if out_rows:
        os.makedirs(os.path.dirname(main_csv), exist_ok=True)
        fieldnames = list(out_rows[0].keys())
        with open(main_csv, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(out_rows)
        print(f"Saved aggregated LS24 main CSV to {main_csv}")

    # Phase A / B / C Breakdown
    if os.path.exists(prog_csv):
        with open(prog_csv, 'r', encoding='utf-8') as f:
            p_rows = list(csv.DictReader(f))

        phases = [
            ('Phase A (Read Sensitive)', 1, 33),
            ('Phase B (Write Burst)', 34, 66),
            ('Phase C (Read Recovery)', 67, 100)
        ]
        
        breakdown_data = []
        for t in thresholds:
            t_str = f"t{t:04d}"
            t_rows = [r for r in p_rows if t_str in r['exp_id']]
            run_ids = sorted(list(set(r['exp_id'] for r in t_rows)))
            
            for p_name, p_start, p_end in phases:
                scan_costs, scan_p99s, put_p99s, get_p99s, iops_list = [], [], [], [], []
                for rid in run_ids:
                    pr = [r for r in t_rows if r['exp_id'] == rid and p_start <= int(r['progress_pct_idx']) <= p_end]
                    if not pr: continue
                    scan_costs.append(np.mean([float(x['scan_us_per_key']) for x in pr]))
                    scan_p99s.append(np.percentile([float(x['scan_p99_us']) for x in pr], 95))
                    put_p99s.append(np.percentile([float(x['put_p99_us']) for x in pr], 95))
                    get_p99s.append(np.percentile([float(x['get_del_p99_us']) for x in pr], 95))
                    iops_list.append(np.mean([float(x['instantaneous_iops']) for x in pr]))

                breakdown_data.append({
                    'threshold': t,
                    'phase': p_name,
                    'iops_mean': float(np.mean(iops_list)) if iops_list else 0.0,
                    'scan_us_per_key_mean': float(np.mean(scan_costs)) if scan_costs else 0.0,
                    'scan_us_per_key_std': float(np.std(scan_costs)) if scan_costs else 0.0,
                    'scan_p99_us_mean': float(np.mean(scan_p99s)) if scan_p99s else 0.0,
                    'scan_p99_us_std': float(np.std(scan_p99s)) if scan_p99s else 0.0,
                    'put_p99_us_mean': float(np.mean(put_p99s)) if put_p99s else 0.0,
                    'put_p99_us_std': float(np.std(put_p99s)) if put_p99s else 0.0,
                    'get_del_p99_us_mean': float(np.mean(get_p99s)) if get_p99s else 0.0,
                    'get_del_p99_us_std': float(np.std(get_p99s)) if get_p99s else 0.0
                })

        if breakdown_data:
            with open(phases_csv, 'w', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=list(breakdown_data[0].keys()))
                writer.writeheader()
                writer.writerows(breakdown_data)
            print(f"Saved aggregated LS24 phase breakdown CSV to {phases_csv}")

if __name__ == "__main__":
    main()
