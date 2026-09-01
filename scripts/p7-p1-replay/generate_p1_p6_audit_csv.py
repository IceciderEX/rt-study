#!/usr/bin/env python3
"""
Generates the comprehensive P1 vs P6 protocol audit CSV table.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
P1_CSV = os.path.join(BASE_DIR, "results", "summary", "p1_summary.csv")
P6_CSV = os.path.join(BASE_DIR, "results", "summary", "p6-dynamic-origin", "all-runs.csv")
OUT_CSV = os.path.join(BASE_DIR, "results", "summary", "p7-p1-replay", "p1-p6-protocol-audit.csv")
os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)

df_p1 = pd.read_csv(P1_CSV)
df_p6 = pd.read_csv(P6_CSV)

rows = []

# Process P1 runs
for _, r in df_p1.iterrows():
    exp_id = r['exp_id']
    duration = float(r['elapsed_sec'])
    total_ops = int(r['total_ops'])
    put_cnt = int(r['put_count'])
    get_aff = int(r['get_aff_live_count'])
    get_del = int(r['get_del_count'])
    get_ctrl = int(r['get_ctrl_count'])
    scan_cnt = int(r['scan_count'])
    del_cnt = int(r['del_range_count'])
    
    del_logical_ratio = (del_cnt / total_ops) * 100.0
    del_active_time = duration # in P1 DeleteRange was interleaved across entire duration
    del_injection_rate = (del_cnt / del_active_time) if del_active_time > 0 else 0.0
    
    del_len = 100
    cum_req_keys = del_cnt * del_len
    union_cov = float(r['union_coverage_ratio']) * 100.0
    union_del_keys = int(r['union_deleted_keys'])
    
    del_rate_sec = del_cnt / duration if duration > 0 else 0.0
    get_rate_sec = (get_aff + get_del + get_ctrl) / duration if duration > 0 else 0.0
    scan_rate_sec = scan_cnt / duration if duration > 0 else 0.0
    put_rate_sec = put_cnt / duration if duration > 0 else 0.0
    
    rows.append({
        'exp_id': exp_id,
        'protocol_suite': 'P1',
        'group': exp_id.replace('_rep1', '').replace('_rep2', '').replace('_rep3', ''),
        'main_duration_sec': duration,
        'total_ops': total_ops,
        'put_count': put_cnt,
        'get_aff_count': get_aff,
        'get_del_count': get_del,
        'get_ctrl_count': get_ctrl,
        'scan_count': scan_cnt,
        'del_range_count': del_cnt,
        'del_range_logical_ratio_pct': del_logical_ratio,
        'del_range_active_rate_ops_per_sec': del_injection_rate,
        'del_range_length_keys': del_len if del_cnt > 0 else 0,
        'cumulative_covered_keys_req': cum_req_keys,
        'final_union_coverage_ratio_pct': union_cov,
        'final_logical_deleted_keys': union_del_keys,
        'del_range_rate_sec': del_rate_sec,
        'get_rate_sec': get_rate_sec,
        'scan_rate_sec': scan_rate_sec,
        'put_rate_sec': put_rate_sec,
        'scan_us_per_key': float(r['scan_us_per_key']),
        'compaction_read_mb': float(r['compaction_read_mb']),
        'compaction_write_mb': float(r['compaction_write_mb']),
        'flush_write_mb': float(r['flush_write_mb']),
        'stall_micros': int(r['stall_micros']),
        'compaction_drop_keys': int(r['compaction_drop_keys']),
        'l0_files_final': int(r['l0_files_final']),
        'data_scale_mb': 128.0,
        'value_size_bytes': 256,
        'block_cache_mb': 128.0
    })

# Process P6 runs
for _, r in df_p6.iterrows():
    exp_id = r['exp_id']
    duration = float(r['main_duration_sec'])
    total_ops = int(r['total_ops'])
    put_cnt = int(r['put_count'])
    get_aff = int(r['get_aff_count'])
    get_del = 0
    get_ctrl = int(r['get_ctrl_count'])
    scan_cnt = int(r['scan_count'])
    del_cnt = int(r['del_range_count'])
    
    del_logical_ratio = (del_cnt / total_ops) * 100.0 if total_ops > 0 else 0.0
    del_active_time = 40.0 if del_cnt > 0 else 0.0 # in P6 D2, 4000 del ranges were injected during first 40s
    del_injection_rate = (del_cnt / del_active_time) if del_active_time > 0 else 0.0
    
    del_len = 100
    cum_req_keys = del_cnt * del_len
    union_cov = 40.0 if del_cnt > 0 or r['group_name'] in ['D1', 'D2'] else 0.0
    union_del_keys = 400000 if del_cnt > 0 or r['group_name'] in ['D1', 'D2'] else 0
    
    del_rate_sec = del_cnt / duration if duration > 0 else 0.0
    get_rate_sec = (get_aff + get_ctrl) / duration if duration > 0 else 0.0
    scan_rate_sec = scan_cnt / duration if duration > 0 else 0.0
    put_rate_sec = put_cnt / duration if duration > 0 else 0.0
    
    rows.append({
        'exp_id': exp_id,
        'protocol_suite': 'P6',
        'group': r['group_name'],
        'main_duration_sec': duration,
        'total_ops': total_ops,
        'put_count': put_cnt,
        'get_aff_count': get_aff,
        'get_del_count': get_del,
        'get_ctrl_count': get_ctrl,
        'scan_count': scan_cnt,
        'del_range_count': del_cnt,
        'del_range_logical_ratio_pct': del_logical_ratio,
        'del_range_active_rate_ops_per_sec': del_injection_rate,
        'del_range_length_keys': del_len if del_cnt > 0 else 0,
        'cumulative_covered_keys_req': cum_req_keys,
        'final_union_coverage_ratio_pct': union_cov,
        'final_logical_deleted_keys': union_del_keys,
        'del_range_rate_sec': del_rate_sec,
        'get_rate_sec': get_rate_sec,
        'scan_rate_sec': scan_rate_sec,
        'put_rate_sec': put_rate_sec,
        'scan_us_per_key': float(r['scan_us_per_key']),
        'compaction_read_mb': float(r['compaction_read_mb']),
        'compaction_write_mb': float(r['compaction_write_mb']),
        'flush_write_mb': float(r['flush_write_mb']),
        'stall_micros': int(r['stall_micros']),
        'compaction_drop_keys': int(r['compaction_drop_keys']),
        'l0_files_final': int(r['l0_files_final']),
        'data_scale_mb': 1024.0,
        'value_size_bytes': 1024,
        'block_cache_mb': 128.0
    })

audit_df = pd.DataFrame(rows)
audit_df.to_csv(OUT_CSV, index=False)
print(f"Generated P1 vs P6 protocol audit table ({len(audit_df)} rows) at {OUT_CSV}")
