#!/usr/bin/env python3
import csv
import os
import numpy as np

def main():
    src_csv = "/home/wam/grad/s14-range-delete-study/results/summary/wb_cleanwrite_all_runs.csv"
    out_csv = "/home/wam/grad/s14-range-delete-study/results/summary/wb-cleanwrite.csv"

    if not os.path.exists(src_csv):
        print(f"Error: {src_csv} not found")
        return

    with open(src_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    configs = ["16mb", "64mb", "128mb"]
    grouped = {c: [] for c in configs}

    for r in rows:
        exp_id = r.get('exp_id', '')
        for c in configs:
            if c in exp_id:
                grouped[c].append(r)
                break

    out_rows = []
    for c in configs:
        item_list = grouped[c]
        if not item_list:
            continue
        
        iops_list = [float(x.get('overall_iops', 0)) for x in item_list]
        put_p99_list = [float(x.get('put_p99_us', 0)) for x in item_list]
        get_p99_list = [float(x.get('get_ctrl_p99_us', 0)) for x in item_list]
        scan_cost_list = [float(x.get('scan_us_per_key', 0)) for x in item_list]
        scan_p99_list = [float(x.get('scan_p99_us', 0)) for x in item_list]
        flush_list = [float(x.get('flush_count_total', 0)) for x in item_list]
        flush_mb_list = [float(x.get('flush_write_mb', 0)) for x in item_list]
        comp_read_mb_list = [float(x.get('compaction_read_mb', 0)) for x in item_list]
        comp_write_mb_list = [float(x.get('compaction_write_mb', 0)) for x in item_list]
        sst_list = [float(x.get('total_sst_mb', 0)) for x in item_list]
        stall_list = [float(x.get('stall_micros', 0)) for x in item_list]

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
            'write_buffer_size': c,
            'runs_count': len(item_list),
            'iops_mean': float(np.mean(iops_list)),
            'iops_std': float(np.std(iops_list)),
            'put_p99_us_mean': float(np.mean(put_p99_list)),
            'put_p99_us_std': float(np.std(put_p99_list)),
            'get_ctrl_p99_us_mean': float(np.mean(get_p99_list)),
            'get_ctrl_p99_us_std': float(np.std(get_p99_list)),
            'scan_us_per_key_mean': float(np.mean(scan_cost_list)),
            'scan_us_per_key_std': float(np.std(scan_cost_list)),
            'scan_p99_us_mean': float(np.mean(scan_p99_list)),
            'scan_p99_us_std': float(np.std(scan_p99_list)),
            'flush_count_mean': float(np.mean(flush_list)),
            'flush_count_std': float(np.std(flush_list)),
            'flush_write_mb_mean': float(np.mean(flush_mb_list)),
            'flush_write_mb_std': float(np.std(flush_mb_list)),
            'compaction_read_mb_mean': float(np.mean(comp_read_mb_list)),
            'compaction_read_mb_std': float(np.std(comp_read_mb_list)),
            'compaction_write_mb_mean': float(np.mean(comp_write_mb_list)),
            'compaction_write_mb_std': float(np.std(comp_write_mb_list)),
            'cwa_mean': float(np.mean(cwa_list)),
            'cwa_std': float(np.std(cwa_list)),
            'pwa_mean': float(np.mean(pwa_list)),
            'pwa_std': float(np.std(pwa_list)),
            'total_sst_mb_mean': float(np.mean(sst_list)),
            'total_sst_mb_std': float(np.std(sst_list)),
            'write_stall_us_mean': float(np.mean(stall_list)),
            'write_stall_us_std': float(np.std(stall_list))
        })

    if out_rows:
        os.makedirs(os.path.dirname(out_csv), exist_ok=True)
        fieldnames = list(out_rows[0].keys())
        with open(out_csv, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(out_rows)
        print(f"Saved aggregated WB-CleanWrite CSV to {out_csv}")

if __name__ == "__main__":
    main()
