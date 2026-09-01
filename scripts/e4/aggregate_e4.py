#!/usr/bin/env python3
"""
Aggregates E4 results into statistical summary tables.
"""
import os
import pandas as pd
import numpy as np

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
ALL_RUNS_CSV = os.path.join(SUMMARY_DIR, "e4_all_runs.csv")
OUT_TABLE_CSV = os.path.join(SUMMARY_DIR, "e4_aggregated.csv")

def aggregate():
    if not os.path.exists(ALL_RUNS_CSV):
        print(f"File not found: {ALL_RUNS_CSV}")
        return

    df = pd.read_csv(ALL_RUNS_CSV)
    if df.empty:
        print("CSV is empty.")
        return

    df['config_base'] = df['exp_id'].str.replace(r'_r\d+$', '', regex=True)

    numeric_cols = df.select_dtypes(include=[np.number]).columns.tolist()
    if 'config_base' in numeric_cols:
        numeric_cols.remove('config_base')

    grouped = df.groupby('config_base')[numeric_cols]
    mean_df = grouped.mean().add_suffix('_mean')
    std_df = grouped.std().add_suffix('_std')
    min_df = grouped.min().add_suffix('_min')
    max_df = grouped.max().add_suffix('_max')

    aggregated = pd.concat([mean_df, std_df, min_df, max_df], axis=1).reset_index()
    aggregated.to_csv(OUT_TABLE_CSV, index=False)
    print(f"E4 aggregated summary saved to: {OUT_TABLE_CSV}")

if __name__ == "__main__":
    aggregate()
