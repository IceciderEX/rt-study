#!/usr/bin/env python3
import csv
import os
import numpy as np

def main():
    src_csv = "/home/wam/grad/s14-range-delete-study/results/summary/b1_all_runs.csv"
    out_csv = "/home/wam/grad/s14-range-delete-study/results/summary/b1-natural-flush-boundary.csv"

    if not os.path.exists(src_csv):
        print(f"Error: {src_csv} not found")
        return

    with open(src_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    configs = ["b1_put_025x", "b1_put_050x", "b1_put_075x", "b1_put_125x"]
    grouped = {c: [] for c in configs}

    for r in rows:
        exp_id = r.get('exp_id', '')
        for c in configs:
            if exp_id.startswith(c):
                grouped[c].append(r)
                break

    metrics = [
        'elapsed_sec', 'total_ops', 'overall_iops',
        'put_count', 'put_p50_us', 'put_p95_us', 'put_p99_us', 'put_mean_us',
        'get_aff_count', 'get_aff_p50_us', 'get_aff_p95_us', 'get_aff_p99_us', 'get_aff_mean_us',
        'get_del_count', 'get_del_p50_us', 'get_del_p95_us', 'get_del_p99_us', 'get_del_mean_us',
        'get_ctrl_count', 'get_ctrl_p50_us', 'get_ctrl_p95_us', 'get_ctrl_p99_us', 'get_ctrl_mean_us',
        'scan_count', 'scan_ops_sec', 'scan_keys_sec', 'scan_avg_keys_returned',
        'scan_p50_us', 'scan_p95_us', 'scan_p99_us', 'scan_mean_us', 'scan_us_per_key',
        'del_range_count', 'del_range_p50_us', 'del_range_p95_us', 'del_range_p99_us', 'del_range_mean_us',
        'tombstones_count', 'flush_count_total', 'flush_write_mb', 'compaction_read_mb', 'compaction_write_mb',
        'stall_micros', 'compaction_drop_keys', 'l0_files_final', 'total_sst_mb'
    ]

    out_rows = []
    for c in configs:
        item_list = grouped[c]
        if not item_list:
            continue
        row_dict = {'config_base': c, 'runs_count': len(item_list)}
        
        # Calculate CWA and PWA for each run
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

        row_dict['cwa_mean'] = float(np.mean(cwa_list))
        row_dict['cwa_std'] = float(np.std(cwa_list))
        row_dict['pwa_mean'] = float(np.mean(pwa_list))
        row_dict['pwa_std'] = float(np.std(pwa_list))

        for m in metrics:
            vals = [float(x.get(m, 0)) for x in item_list]
            row_dict[f'{m}_mean'] = float(np.mean(vals))
            row_dict[f'{m}_std'] = float(np.std(vals))
            row_dict[f'{m}_min'] = float(np.min(vals))
            row_dict[f'{m}_max'] = float(np.max(vals))

        out_rows.append(row_dict)

    if out_rows:
        os.makedirs(os.path.dirname(out_csv), exist_ok=True)
        fieldnames = list(out_rows[0].keys())
        with open(out_csv, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(out_rows)
        print(f"Saved aggregated B1 CSV to {out_csv}")

if __name__ == "__main__":
    main()
