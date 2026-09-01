#!/usr/bin/env python3
import csv
import numpy as np

def main():
    prog_csv = "/home/wam/grad/s14-range-delete-study/results/summary/b2_timeseries_progress.csv"
    out_csv = "/home/wam/grad/s14-range-delete-study/results/summary/b2-phases-breakdown.csv"
    
    with open(prog_csv, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    # Thresholds: 0, 64, 256, 2048
    thresholds = [0, 64, 256, 2048]
    
    # Phase ranges:
    # Phase A: 1 <= progress_pct_idx <= 33
    # Phase B: 34 <= progress_pct_idx <= 66
    # Phase C: 67 <= progress_pct_idx <= 100
    phases = [
        ('Phase A (Read Sensitive)', 1, 33),
        ('Phase B (Write Burst)', 34, 66),
        ('Phase C (Read Recovery)', 67, 100)
    ]
    
    breakdown_data = []
    
    for t in thresholds:
        t_str = f"t{t:04d}"
        t_rows = [r for r in rows if t_str in r['exp_id']]
        
        # Group by run: r01, r02, r03
        run_ids = sorted(list(set(r['exp_id'] for r in t_rows)))
        
        for p_name, p_start, p_end in phases:
            scan_costs = []
            scan_p99s = []
            put_p99s = []
            get_p99s = []
            iops_list = []
            flush_deltas = []
            cwa_deltas = []
            
            for rid in run_ids:
                p_run_rows = [r for r in t_rows if r['exp_id'] == rid and p_start <= int(r['progress_pct_idx']) <= p_end]
                if not p_run_rows:
                    continue
                
                # Metrics for this phase in this run
                scan_costs.append(np.mean([float(r['scan_us_per_key']) for r in p_run_rows]))
                scan_p99s.append(np.percentile([float(r['scan_p99_us']) for r in p_run_rows], 95))
                put_p99s.append(np.percentile([float(r['put_p99_us']) for r in p_run_rows], 95))
                get_p99s.append(np.percentile([float(r['get_del_p99_us']) for r in p_run_rows], 95))
                iops_list.append(np.mean([float(r['instantaneous_iops']) for r in p_run_rows]))
                
                # Flush delta in this phase
                f_delta = sum(float(r.get('flush_range_del_reason_delta', 0)) for r in p_run_rows)
                flush_deltas.append(f_delta)
                
            breakdown_data.append({
                'threshold': t,
                'phase': p_name,
                'iops_mean': float(np.mean(iops_list)),
                'scan_us_per_key_mean': float(np.mean(scan_costs)),
                'scan_us_per_key_std': float(np.std(scan_costs)),
                'scan_p99_us_mean': float(np.mean(scan_p99s)),
                'scan_p99_us_std': float(np.std(scan_p99s)),
                'put_p99_us_mean': float(np.mean(put_p99s)),
                'put_p99_us_std': float(np.std(put_p99s)),
                'get_del_p99_us_mean': float(np.mean(get_p99s)),
                'get_del_p99_us_std': float(np.std(get_p99s)),
                'flush_count_mean': float(np.mean(flush_deltas))
            })

    # Save to CSV
    fieldnames = ['threshold', 'phase', 'iops_mean', 'scan_us_per_key_mean', 'scan_us_per_key_std', 'scan_p99_us_mean', 'scan_p99_us_std', 'put_p99_us_mean', 'put_p99_us_std', 'get_del_p99_us_mean', 'get_del_p99_us_std', 'flush_count_mean']
    with open(out_csv, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(breakdown_data)
        
    print(f"Saved B2 phase breakdown CSV to {out_csv}")
    
    # Print formatted table
    print("\n" + "="*80)
    print("B2 PER-PHASE BREAKDOWN SUMMARY")
    print("="*80)
    print(f"{'Threshold':<10} | {'Phase':<28} | {'Scan Cost (us/k)':<16} | {'Scan P99 (us)':<14} | {'Put P99 (us)':<14} | {'Get P99 (us)':<14}")
    print("-"*105)
    for b in breakdown_data:
        print(f"T={b['threshold']:<8} | {b['phase']:<28} | {b['scan_us_per_key_mean']:<16.2f} | {b['scan_p99_us_mean']:<14.1f} | {b['put_p99_us_mean']:<14.1f} | {b['get_del_p99_us_mean']:<14.1f}")
    print("="*80)

if __name__ == "__main__":
    main()
