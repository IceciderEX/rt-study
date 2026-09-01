#!/usr/bin/env python3
"""
Results Aggregator for RocksDB Range Deletion Study
Calculates Mean, Std, Min, Max across 3 repetitions for each experiment configuration.
"""

import os
import glob
import pandas as pd
import numpy as np

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
TABLES_DIR = os.path.join(SUMMARY_DIR, "tables")

os.makedirs(TABLES_DIR, exist_ok=True)

def process_summary_file(csv_path, group_name):
    if not os.path.exists(csv_path):
        print(f"Summary CSV not found: {csv_path} (Skipping)")
        return

    df = pd.read_csv(csv_path)
    if df.empty:
        print(f"Summary CSV is empty: {csv_path}")
        return

    # Extract base config name by stripping _rep\d+
    df['config_base'] = df['exp_id'].str.replace(r'_rep\d+$', '', regex=True)

    numeric_cols = df.select_dtypes(include=[np.number]).columns.tolist()
    if 'config_base' in numeric_cols:
        numeric_cols.remove('config_base')

    grouped = df.groupby('config_base')[numeric_cols]
    mean_df = grouped.mean().add_suffix('_mean')
    std_df = grouped.std().add_suffix('_std')
    min_df = grouped.min().add_suffix('_min')
    max_df = grouped.max().add_suffix('_max')

    aggregated = pd.concat([mean_df, std_df, min_df, max_df], axis=1).reset_index()
    out_csv = os.path.join(TABLES_DIR, f"{group_name}_aggregated.csv")
    aggregated.to_csv(out_csv, index=False)
    print(f"[{group_name}] Aggregated summary saved to: {out_csv}")

def main():
    print("=== Aggregating Experiment Results ===")
    groups = {
        "p1_ratio": os.path.join(SUMMARY_DIR, "p1_summary.csv"),
        "p2_tombstone": os.path.join(SUMMARY_DIR, "p2_summary.csv"),
        "p3_composition": os.path.join(SUMMARY_DIR, "p3_summary.csv"),
        "p4_locality": os.path.join(SUMMARY_DIR, "p4_summary.csv"),
        "p5_compaction": os.path.join(SUMMARY_DIR, "p5_summary.csv"),
    }

    for name, path in groups.items():
        process_summary_file(path, name)

if __name__ == "__main__":
    main()
