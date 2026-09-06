#!/usr/bin/env python3
import os
import json
import glob
import numpy as np

def load_json(path):
    with open(path, 'r') as f:
        return json.load(f)

results_dir = "/home/wam/grad/s14-range-delete-study/results/amtv_m2d/raw/m2d1a_r_paired"

pairs = [
    (1, f"{results_dir}/m2d1a_r_rep01_b64_h32.json", f"{results_dir}/m2d1a_r_rep01_b128_h16.json", "A->B"),
    (2, f"{results_dir}/m2d1a_r_rep02_b64_h32.json", f"{results_dir}/m2d1a_r_rep02_b128_h16.json", "B->A"),
    (3, f"{results_dir}/m2d1a_r_rep03_b64_h32.json", f"{results_dir}/m2d1a_r_rep03_b128_h16.json", "B->A"),
    (4, f"{results_dir}/m2d1a_r_rep04_b64_h32.json", f"{results_dir}/m2d1a_r_rep04_b128_h16.json", "A->B"),
]

data_a = []
data_b = []

for rep, path_a, path_b, order in pairs:
    data_a.append(load_json(path_a))
    data_b.append(load_json(path_b))

metrics = [
    ("fg_iops", "Foreground IOPS (ops/s)", False),
    ("fg_elapsed_sec", "Foreground Elapsed (s)", True),
    ("phase_a_sec", "Phase A Elapsed (s)", True),
    ("phase_b_sec", "Phase B Elapsed (s)", True),
    ("phase_c_sec", "Phase C Elapsed (s)", True),
    ("get_live_p50_us", "GetLive P50 (us)", True),
    ("get_live_p95_us", "GetLive P95 (us)", True),
    ("get_live_p99_us", "GetLive P99 (us)", True),
    ("get_live_p999_us", "GetLive P99.9 (us)", True),
    ("get_live_max_us", "GetLive Max (us)", True),
    ("put_p95_us", "Put P95 (us)", True),
    ("put_p99_us", "Put P99 (us)", True),
    ("put_p999_us", "Put P99.9 (us)", True),
    ("put_max_us", "Put Max (us)", True),
    ("delete_range_p95_us", "DeleteRange P95 (us)", True),
    ("delete_range_p99_us", "DeleteRange P99 (us)", True),
    ("delete_range_p999_us", "DeleteRange P99.9 (us)", True),
    ("delete_range_max_us", "DeleteRange Max (us)", True),
    ("peak_actual_sealed_runs", "Peak Actual Sealed Runs", True),
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
    ("amtv_reconstruction_amplification", "AMTV Reconstruction Amplification", True),
    ("amtv_fallback_events", "Fallback Events", True),
]

summary_data = {}

print("=" * 125)
print(f"{'Metric':<38} | {'Candidate A (B64-H32)':<25} | {'Candidate B (B128-H16)':<25} | {'Diff (A - B) [Mean +/- SD]':<22}")
print("=" * 125)

for key, label, lower_better in metrics:
    vals_a = [d.get(key, 0) for d in data_a]
    vals_b = [d.get(key, 0) for d in data_b]
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

    str_a = f"{mean_a:.2f} +/- {std_a:.2f}" if abs(mean_a) < 10000 else f"{mean_a:.0f} +/- {std_a:.0f}"
    str_b = f"{mean_b:.2f} +/- {std_b:.2f}" if abs(mean_b) < 10000 else f"{mean_b:.0f} +/- {std_b:.0f}"
    str_diff = f"{mean_diff:+.2f} +/- {std_diff:.2f}" if abs(mean_diff) < 10000 else f"{mean_diff:+.0f} +/- {std_diff:.0f}"

    print(f"{label:<38} | {str_a:<25} | {str_b:<25} | {str_diff:<22}")

print("=" * 125)

# Save JSON summary
with open(f"{results_dir}/m2d1a_r_summary.json", 'w') as f:
    json.dump(summary_data, f, indent=2)

print("\nDetailed per-round values:")
for k in ["fg_iops", "get_live_p99_us", "get_live_p999_us", "put_p99_us", "delete_range_p99_us", "peak_actual_sealed_runs", "peak_backlog_excess", "phase_b_end_to_merge_stable_us", "foreground_end_to_merge_stable_us", "amtv_fallback_events"]:
    item = summary_data[k]
    lbl = item["label"]
    va = item["vals_a"]
    vb = item["vals_b"]
    df = item["diffs"]
    pct = [(a - b) / b * 100 for a, b in zip(va, vb)] if k == "fg_iops" else []
    print(lbl + ":")
    print("  A: " + str(va))
    print("  B: " + str(vb))
    print("  Diff (A-B): " + str(df))
    if pct:
        print("  Pct diff: " + str([f"{p:+.2f}%" for p in pct]) + f", Mean Pct: {(item['mean_a'] - item['mean_b']) / item['mean_b'] * 100:+.2f}%")

# Evaluate 8 Rules
print("\n" + "=" * 80)
print("Evaluating 8 Formal Parameter Freeze Rules for B64/H32:")
print("=" * 80)

# Rule 1: 4/4 no fallback, no merge failure, no reconcile error
a_fallback = sum(d["amtv_fallback_events"] for d in data_a)
b_fallback = sum(d["amtv_fallback_events"] for d in data_b)
a_reconcile = all(d["db_sha256"] == d["expected_model_sha"] for d in data_a)
b_reconcile = all(d["db_sha256"] == d["expected_model_sha"] for d in data_b)
r1 = (a_fallback == 0 and b_fallback == 0 and a_reconcile and b_reconcile)
print(f"Rule 1 (4/4 zero fallback & reconcile PASS): {'PASS' if r1 else 'FAIL'} (A fallback={a_fallback}, B fallback={b_fallback})")

# Rule 2: 4 pairs diff direction consistent, mean diff >= 10%
iops_diffs = summary_data["fg_iops"]["diffs"]
mean_iops_a = summary_data["fg_iops"]["mean_a"]
mean_iops_b = summary_data["fg_iops"]["mean_b"]
mean_iops_lead_pct = (mean_iops_a - mean_iops_b) / mean_iops_b * 100
r2_consistent = all(d > 0 for d in iops_diffs)
r2_lead = mean_iops_lead_pct >= 10.0
r2 = r2_consistent and r2_lead
print(f"Rule 2 (Throughput diff > 0 for all 4 pairs & mean lead >= 10%): {'PASS' if r2 else 'FAIL'} (diffs={iops_diffs}, mean lead={mean_iops_lead_pct:.2f}%)")

# Rule 3: Get P99 & P99.9 no worse than B by > 10%
get_p99_a = summary_data["get_live_p99_us"]["mean_a"]
get_p99_b = summary_data["get_live_p99_us"]["mean_b"]
get_p99_ratio = (get_p99_a - get_p99_b) / get_p99_b * 100
get_p999_a = summary_data["get_live_p999_us"]["mean_a"]
get_p999_b = summary_data["get_live_p999_us"]["mean_b"]
get_p999_ratio = (get_p999_a - get_p999_b) / get_p999_b * 100
r3 = (get_p99_ratio <= 10.0 and get_p999_ratio <= 10.0)
print(f"Rule 3 (Get P99 & P99.9 not worse by > 10%): {'PASS' if r3 else 'FAIL'} (P99 diff={get_p99_ratio:+.2f}%, P99.9 diff={get_p999_ratio:+.2f}%)")

# Rule 4: Put P99 and DeleteRange P99 not worse by > 10%
put_p99_a = summary_data["put_p99_us"]["mean_a"]
put_p99_b = summary_data["put_p99_us"]["mean_b"]
put_p99_ratio = (put_p99_a - put_p99_b) / put_p99_b * 100
del_p99_a = summary_data["delete_range_p99_us"]["mean_a"]
del_p99_b = summary_data["delete_range_p99_us"]["mean_b"]
del_p99_ratio = (del_p99_a - del_p99_b) / del_p99_b * 100
r4 = (put_p99_ratio <= 10.0 and del_p99_ratio <= 10.0)
print(f"Rule 4 (Put P99 & DeleteRange P99 not worse by > 10%): {'PASS' if r4 else 'FAIL'} (Put P99 diff={put_p99_ratio:+.2f}%, Del P99 diff={del_p99_ratio:+.2f}%)")

# Rule 5: A's peak_actual_sealed_runs < 24
max_peak_runs_a = max(d["peak_actual_sealed_runs"] for d in data_a)
r5 = max_peak_runs_a < 24
print(f"Rule 5 (A peak_actual_sealed_runs < 24): {'PASS' if r5 else 'FAIL'} (max={max_peak_runs_a})")

# Rule 6: A converges to strict stable signed_backlog == 0
a_drain_stable = all(d["phase_b_end_to_merge_stable_us"] > 0 and d["foreground_end_to_merge_stable_us"] == 0 and d.get("theoretical_distribution_matched", False) for d in data_a)
r6 = a_drain_stable
print(f"Rule 6 (A strictly converges to signed_backlog == 0): {'PASS' if r6 else 'FAIL'}")

# Rule 7: A has no millisecond Get P99.9 or abnormal Max long tail
max_get_p999_a = max(d["get_live_p999_us"] for d in data_a)
r7 = max_get_p999_a < 1000.0
print(f"Rule 7 (A no ms Get P99.9): {'PASS' if r7 else 'FAIL'} (max P99.9={max_get_p999_a:.2f} us)")

# Rule 8: Auxiliary memory proxies no unexplained growth
rss_a = summary_data["peak_rss_kb"]["mean_a"]
rss_b = summary_data["peak_rss_kb"]["mean_b"]
raw_cap_a = summary_data["raw_entry_capacity_proxy_bytes_peak"]["mean_a"]
raw_cap_b = summary_data["raw_entry_capacity_proxy_bytes_peak"]["mean_b"]
r8 = abs(rss_a - rss_b) / rss_b < 0.05 and abs(raw_cap_a - raw_cap_b) / raw_cap_b < 0.05
print(f"Rule 8 (Memory proxies consistent, no unexplained growth): {'PASS' if r8 else 'FAIL'} (RSS diff={abs(rss_a - rss_b)/rss_b*100:.2f}%, Cap diff={abs(raw_cap_a - raw_cap_b)/raw_cap_b*100:.2f}%)")

all_passed = r1 and r2 and r3 and r4 and r5 and r6 and r7 and r8
print("=" * 80)
print(f"ALL 8 RULES PASSED: {'YES -> FORMAL FREEZE B64/H32 APPROVED' if all_passed else 'NO -> GATES FAILED'}")
print("=" * 80)
