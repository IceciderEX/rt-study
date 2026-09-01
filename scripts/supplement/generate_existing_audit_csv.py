#!/usr/bin/env python3
"""
Combines all 54 runs from P1 to P5 into a single audit CSV.
"""
import os
import pandas as pd

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SUMMARY_DIR = os.path.join(BASE_DIR, "results", "summary")
SUPP_SUMMARY_DIR = os.path.join(SUMMARY_DIR, "supplement")
os.makedirs(SUPP_SUMMARY_DIR, exist_ok=True)

files = [
    os.path.join(SUMMARY_DIR, "p1_summary.csv"),
    os.path.join(SUMMARY_DIR, "p2_summary.csv"),
    os.path.join(SUMMARY_DIR, "p3_summary.csv"),
    os.path.join(SUMMARY_DIR, "p4_summary.csv"),
    os.path.join(SUMMARY_DIR, "p5_summary.csv"),
]

dfs = []
for f in files:
    if os.path.exists(f):
        df = pd.read_csv(f)
        dfs.append(df)
        print(f"Loaded {len(df)} rows from {f}")

all_runs = pd.concat(dfs, ignore_index=True)
out_path = os.path.join(SUPP_SUMMARY_DIR, "existing-runs-audit.csv")
all_runs.to_csv(out_path, index=False)
print(f"Successfully saved {len(all_runs)} runs to {out_path}")
