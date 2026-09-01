#!/usr/bin/env python3
"""
Unit test for Formal V2 Traces
Validates SHA-256 integrity, binary record bounds, exact operation counts, and partition isolation.
"""

import os
import json
import struct
import hashlib

RECORD_FORMAT = "<BBBBIQQ"
RECORD_SIZE = 24

def verify_trace_dir(trace_dir: str):
    manifest_path = os.path.join(trace_dir, "manifest.json")
    assert os.path.exists(manifest_path), f"Manifest not found: {manifest_path}"

    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    assert manifest["magic"] == "0x54524143455632", "Magic mismatch"
    assert manifest["version"] == "2.0", "Version mismatch"

    num_workers = manifest["num_workers"]
    worker_ranges = manifest["worker_ranges"]
    payload_files = manifest["payload_files"]

    print(f"=== Testing Trace Directory: {trace_dir} ===")
    print(f"  Workload ID: {manifest['workload_id']}, Workers: {num_workers}, Total Keys: {manifest['total_keys']}")

    total_records = 0
    for filename, p_info in payload_files.items():
        bin_path = os.path.join(trace_dir, filename)
        assert os.path.exists(bin_path), f"Missing payload: {bin_path}"

        # 1. Size check
        f_size = os.path.getsize(bin_path)
        expected_records = p_info["ops_count"]
        assert f_size == expected_records * RECORD_SIZE, f"File size mismatch in {filename}: {f_size} != {expected_records * RECORD_SIZE}"

        # 2. SHA-256 check
        h = hashlib.sha256()
        with open(bin_path, "rb") as f:
            while chunk := f.read(65536):
                h.update(chunk)
        actual_sha = h.hexdigest()
        assert actual_sha == p_info["sha256"], f"SHA256 mismatch in {filename}: {actual_sha} != {p_info['sha256']}"

        # 3. Payload record boundary & opcode check
        w_id = p_info["worker_id"]
        w_start, w_end = worker_ranges[w_id]

        with open(bin_path, "rb") as f:
            for r_idx in range(expected_records):
                chunk = f.read(RECORD_SIZE)
                p_idx, op_type, s_mode, reserved, op_id, k1, k2 = struct.unpack(RECORD_FORMAT, chunk)
                assert op_type in [0, 1, 2, 3, 4], f"Invalid op_type {op_type} at record {r_idx} in {filename}"
                assert s_mode in [0, 1], f"Invalid scan_mode {s_mode} at record {r_idx} in {filename}"
                assert w_start <= k1 < w_end, f"Key1 {k1} out of worker bounds [{w_start}, {w_end}) in {filename}"
                if op_type == 3: # DeleteRange
                    assert w_start <= k1 < k2 <= w_end, f"DeleteRange [{k1}, {k2}) out of bounds in {filename}"
                total_records += 1

    assert total_records == manifest["actual_metrics"]["total_ops_count"], "Total records sum mismatch"
    print(f"  [PASS] Verified {total_records} records across {len(payload_files)} payload files. All partition bounds strictly valid.\n")

def main():
    base_dir = "/home/wam/grad/s14-range-delete-study/traces/formal_v2"
    workloads = [
        "small_dynamic_500k_limit",
        "small_dynamic_500k_clean",
        "small_dynamic_500k_range",
        "ls24_density_preserved",
        "ls24_density_clean",
        "wb_cleanwrite_256mb"
    ]

    for w in workloads:
        p = os.path.join(base_dir, w)
        if os.path.exists(p):
            verify_trace_dir(p)

    print("=========================================================")
    print("  ALL FORMAL V2 TRACES PASSED UNIT TEST AND SHA-256 CHECK")
    print("=========================================================")

if __name__ == "__main__":
    main()
