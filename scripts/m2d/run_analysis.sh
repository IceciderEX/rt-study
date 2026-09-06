#!/usr/bin/env bash
cat << 'EOF' > /home/wam/grad/s14-range-delete-study/scripts/m2d/analyze_m2d1a.py
#!/usr/bin/env python3
import os
import json
import glob
import numpy as np

def load_json(path):
    with open(path, 'r') as f:
        return json.load(f)

results_dir = "/home/wam/grad/s14-range-delete-study/results/amtv_m2d/raw/m2d1a_paired"

# Pairs for Rep 1, 2, 3
pairs = [
    (1, f"{results_dir}/m2d1a_rep1_b64_h32.json", f"{results_dir}/m2d1a_rep1_b128_h16.json"),
    (2, f"{results_dir}/m2d1a_rep2_b64_h32.json", f"{results_dir}/m2d1a_rep2_b128_h16.json"),
    (3, f"{results_dir}/m2d1a_rep3_b64_h32.json", f"{results_dir}/m2d1a_rep3_b128_h16.json"),
]

data_a = []
data_b = []

for rep, path_a, path_b in pairs:
    data_a.append(load_json(path_a))
    data_b.append(load_json(path_b))

metrics = [
    ("fg_iops", "Foreground IOPS (ops/s)", False),
    ("fg_elapsed_sec", "Foreground Elapsed (s)", True),
    ("phase_b_sec", "Phase B Elapsed (s)", True),
    ("get_live_p50_us", "GetLive P50 (us)", True),
    ("get_live_p95_us", "GetLive P95 (us)", True),
    ("get_live_p99_us", "GetLive P99 (us)", True),
    ("get_live_p999_us", "GetLive P99.9 (us)", True),
    ("get_live_max_us", "GetLive Max (us)", True),
    ("put_p99_us", "Put P99 (us)", True),
    ("put_max_us", "Put Max (us)", True),
    ("delete_range_p99_us", "DeleteRange P99 (us)", True),
    ("delete_range_max_us", "DeleteRange Max (us)", True),
    ("peak_backlog_excess", "Peak Backlog Excess (runs)", True),
    ("backlog_excess_max_duration_us", "Backlog Excess Max Duration (us)", True),
    ("peak_claimed_input_runs", "Peak Claimed Input Runs", True),
    ("peak_scheduling_backlog", "Peak Scheduling Backlog", True),
    ("phase_b_end_to_merge_stable_us", "Phase B End to Stable (us)", True),
    ("foreground_end_to_merge_stable_us", "Foreground End to Stable (us)", True),
    ("phase_b_end_merges_computed_after", "Merges Computed After Phase B", True),
    ("phase_b_end_merges_published_after", "Merges Published After Phase B", True),
    ("amtv_merge_completed", "Total Merges Completed", True),
    ("max_computed_merge_wall_us", "Max Computed Merge Wall (us)", True),
    ("max_published_merge_wall_us", "Max Published Merge Wall (us)", True),
    ("total_computed_merge_wall_us", "Total Computed Merge Wall (us)", True),
    ("total_published_merge_wall_us", "Total Published Merge Wall (us)", True),
    ("phase_b_del_range_chunk_arrival_rate_chunks_per_sec", "Phase B Chunk Arrival Rate (chunks/s)", False),
    ("raw_entry_payload_bytes_peak", "Raw Entry Payload Peak (bytes)", True),
    ("raw_entry_capacity_proxy_bytes_peak", "Raw Entry Capacity Proxy Peak (bytes)", True),
    ("fragment_payload_proxy_bytes_peak", "Fragment Payload Proxy Peak (bytes)", True),
    ("inflight_payload_proxy_bytes_peak", "In-flight Payload Proxy Peak (bytes)", True),
    ("peak_rss_kb", "Peak RSS (KB)", True),
    ("amtv_reconstruction_amplification", "AMTV Reconstruction Amplification", True)
]

print("=" * 110)
print(f"{'Metric':<36} | {'Candidate A (B64-H32)':<25} | {'Candidate B (B128-H16)':<25} | {'Diff (A - B) [Mean ± SD]':<20}")
print("=" * 110)

summary_data = {}

for key, label, lower_better in metrics:
    vals_a = [d[key] for d in data_a]
    vals_b = [d[key] for d in data_b]
    diffs = [a - b for a, b in zip(vals_a, vals_b)]
    
    mean_a, std_a = np.mean(vals_a), np.std(vals_a, ddof=1)
    mean_b, std_b = np.mean(vals_b), np.std(vals_b, ddof=1)
    mean_diff, std_diff = np.mean(diffs), np.std(diffs, ddof=1)
    med_diff = np.median(diffs)
    
    summary_data[key] = {
        'label': label,
        'vals_a': vals_a,
        'vals_b': vals_b,
        'diffs': diffs,
        'mean_a': mean_a, 'std_a': std_a,
        'mean_b': mean_b, 'std_b': std_b,
        'mean_diff': mean_diff, 'std_diff': std_diff,
        'med_diff': med_diff,
        'lower_better': lower_better
    }
    
    str_a = f"{mean_a:.2f} ± {std_a:.2f}" if abs(mean_a) < 10000 else f"{mean_a:.0f} ± {std_a:.0f}"
    str_b = f"{mean_b:.2f} ± {std_b:.2f}" if abs(mean_b) < 10000 else f"{mean_b:.0f} ± {std_b:.0f}"
    str_diff = f"{mean_diff:+.2f} ± {std_diff:.2f}" if abs(mean_diff) < 10000 else f"{mean_diff:+.0f} ± {std_diff:.0f}"
    
    print(f"{label:<36} | {str_a:<25} | {str_b:<25} | {str_diff:<20}")

print("=" * 110)

# Save JSON summary
with open(f"{results_dir}/m2d1a_summary.json", 'w') as f:
    json.dump(summary_data, f, indent=2)

print("Summary saved to m2d1a_summary.json")
EOF
python3 /home/wam/grad/s14-range-delete-study/scripts/m2d/analyze_m2d1a.py
