#!/usr/bin/env python3
import csv
import json

path = "/home/wam/grad/s14-range-delete-study/results/amtv_m2d/raw/m2d1a_r_paired/m2d1a_r_rep02_b64_h32_timeline.csv"

with open(path, "r") as f:
    reader = list(csv.DictReader(f))

print(f"Total rows in timeline: {len(reader)}")

# Find the peak sealed_run_count
max_sealed = 0
peak_rows = []
for idx, row in enumerate(reader):
    sealed = int(row["sealed_run_count"])
    if sealed > max_sealed:
        max_sealed = sealed
        peak_rows = [(idx, row)]
    elif sealed == max_sealed:
        peak_rows.append((idx, row))

print(f"Peak sealed_run_count: {max_sealed}")
print(f"Number of rows with peak sealed_run_count: {len(peak_rows)}")

for idx, r in peak_rows:
    print(f"\n--- Peak Row Index {idx} ---")
    for k in ["monotonic_timestamp_us", "phase", "foreground_ops_completed", "delete_ranges_issued",
              "event_type", "open_delta_size", "sealed_run_count", "hard_run_limit",
              "input_run_ids", "output_run_id", "input_levels", "output_level",
              "input_chunks", "output_chunk_count", "input_tombstones", "output_tombstone_count",
              "merge_queue_wait_us", "merge_wall_time_us", "merge_cpu_time_us",
              "pre_publish_hist", "post_publish_hist", "fallback_details"]:
        print(f"  {k}: {r[k]}")

# Let's also print context around the first peak row
first_peak_idx = peak_rows[0][0]
print(f"\nContext around row {first_peak_idx}:")
for idx in range(max(0, first_peak_idx - 15), min(len(reader), first_peak_idx + 10)):
    r = reader[idx]
    mark = " >>> PEAK <<<" if idx == first_peak_idx else ""
    print(f"Row {idx:4d} | ts={r['monotonic_timestamp_us']} | ev={r['event_type']:14s} | sealed={r['sealed_run_count']:2s} | out_lvl={r['output_level']:2s} | out_id={r['output_run_id']:4s} | in_ids={r['input_run_ids']:15s} | del_issued={r['delete_ranges_issued']:6s} | post={r['post_publish_hist']}{mark}")
