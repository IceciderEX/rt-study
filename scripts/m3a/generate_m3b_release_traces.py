#!/usr/bin/env python3
"""
M3b Release N=5 Trace Generator
Seeds: 610001 (Rep 1), 620001 (Rep 2), 630001 (Rep 3), 640001 (Rep 4), 650001 (Rep 5)
Follows exact M3a trace geometry, operation counts, pre-registered Scan semantics, and hard causality.
"""

import os
import sys
import json
from generate_trace_m3a import generate_trace_package

M3B_RELEASE_SEEDS = {
    'rep1': (610001, "traces/m3b_rel_rep1_seed610001"),
    'rep2': (620001, "traces/m3b_rel_rep2_seed620001"),
    'rep3': (630001, "traces/m3b_rel_rep3_seed630001"),
    'rep4': (640001, "traces/m3b_rel_rep4_seed640001"),
    'rep5': (650001, "traces/m3b_rel_rep5_seed650001"),
}

def main():
    print("======================================================================")
    print("M3b Release N=5 Trace Generation: 5 New Seeds (610001..650001)")
    print("======================================================================")

    manifests = {}
    for name, (seed, out_dir) in M3B_RELEASE_SEEDS.items():
        manifests[name] = generate_trace_package(seed, out_dir)

    print("\n[Release Gate Assertions]")
    del_shas = set(m['delete_geometry_sha256'] for m in manifests.values())
    assert len(del_shas) == 1, f"Delete geometry SHA must be identical across all seeds, got: {del_shas}"
    print(f"  [PASS] Delete Geometry SHA is invariant across all 5 seeds: {list(del_shas)[0]}")

    for name, m in manifests.items():
        gates = m['audit_gates']
        assert gates['phase_a_planned_intersect_scans'] == 5000
        assert gates['phase_b_intersect_scans'] == 5000
        assert gates['phase_c_intersect_scans'] == 5000
        assert gates['total_non_intersect_scans'] == 15000
        assert gates['total_delete_ranges'] == 20000
        assert gates['total_puts'] == 60000
        assert gates['total_get_lives'] == 190000
        assert gates['total_scan_visible_keys'] == 2500000
        print(f"  [PASS] All gate assertions passed for {name}")

    print("\nAll 5 M3b Release trace sets generated successfully!")
    print("======================================================================")

if __name__ == '__main__':
    main()
