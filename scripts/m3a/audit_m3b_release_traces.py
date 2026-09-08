#!/usr/bin/env python3
"""
M3b Static Trace Auditor for Release N=5
Audits all 5 Release trace packages:
- Exact binary length
- Exact operation counts per phase
- Hard causality assertion for all Phase B Scan-Intersect operations
- 500K state model simulation (300K Live, 200K Deleted)
- Invariant DeleteRange geometry SHA across all 5 seeds
"""

import os
import sys
import json
from audit_m3a_trace import audit_trace_package

RELEASE_TRACE_DIRS = [
    "traces/m3b_rel_rep1_seed610001",
    "traces/m3b_rel_rep2_seed620001",
    "traces/m3b_rel_rep3_seed630001",
    "traces/m3b_rel_rep4_seed640001",
    "traces/m3b_rel_rep5_seed650001",
]

def main():
    print("======================================================================")
    print("M3b Release N=5 Static Trace Auditor: Binary Layout & Hard Invariants")
    print("======================================================================")

    results = []
    for d in RELEASE_TRACE_DIRS:
        res = audit_trace_package(d)
        results.append(res)
        print(f"[{res['seed']}] Audit PASS: {d}")
        print(f"  Trace SHA:   {res['trace_sha256']}")
        print(f"  Del Geo SHA: {res['delete_geometry_sha256']}")
        print(f"  Scan Keys:   {res['scan_visible_keys_total']} (Exp: 2,500,000)")
        print(f"  Final State: {res['live_keys']} Live, {res['deleted_keys']} Deleted")

    del_shas = set(r['delete_geometry_sha256'] for r in results)
    assert len(del_shas) == 1, f"Delete geometry SHA must be identical across all 5 seeds! Got {del_shas}"
    print(f"\n[PASS] All 5 seeds have 100% identical delete geometry SHA: {list(del_shas)[0]}")

    out_json = "results/amtv_m3b/m3b_trace_static_audit_summary.json"
    os.makedirs(os.path.dirname(out_json), exist_ok=True)
    with open(out_json, "w") as f:
        json.dump(results, f, indent=2)
    print(f"Summary written to {out_json}")
    print("======================================================================")

if __name__ == '__main__':
    main()
