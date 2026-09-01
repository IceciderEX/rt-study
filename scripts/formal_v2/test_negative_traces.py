#!/usr/bin/env python3
"""
Formal V2 Negative Trace & Manifest Admission Rejection Suite
Verifies that the formal driver strictly catches and rejects invalid, tampered,
cross-partition, corrupt, duplicate, or mismatching traces/manifests before database creation.
"""

import os
import sys
import shutil
import struct
import subprocess
import hashlib
import json

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DRIVER_BIN = os.path.join(BASE_DIR, "bin", "formal_driver")
VALID_TRACE_DIR = os.path.join(BASE_DIR, "traces", "formal_v2", "small_dynamic_500k_limit")
TEST_SCRATCH = os.path.join(BASE_DIR, "results", "formal_v2", "negative_tests")

RECORD_FORMAT = "<BBBBIQQ"

def compute_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def update_manifest_sha(trace_dir: str, filename: str):
    manifest_path = os.path.join(trace_dir, "manifest.json")
    with open(manifest_path, "r") as f:
        m = json.load(f)
    new_sha = compute_sha256(os.path.join(trace_dir, filename))
    m["payload_files"][filename]["sha256"] = new_sha
    with open(manifest_path, "w") as f:
        json.dump(m, f, indent=2)

def run_driver_expect_fail(trace_dir: str, expected_err_sub: str, test_name: str, check_no_db_created: bool = True):
    db_path = os.path.join(TEST_SCRATCH, "db_neg_test")
    if os.path.exists(db_path):
        shutil.rmtree(db_path)
    
    cmd = [
        DRIVER_BIN,
        "--exp_id", f"neg_{test_name}",
        "--group_name", "neg_test",
        "--db_path", db_path,
        "--result_dir", TEST_SCRATCH,
        "--summary_csv", os.path.join(TEST_SCRATCH, "neg_summary.csv"),
        "--events_csv", os.path.join(TEST_SCRATCH, "neg_events.csv"),
        "--phases_csv", os.path.join(TEST_SCRATCH, "neg_phases.csv"),
        "--trace_dir", trace_dir,
        "--total_keys", "500000",
        "--value_size", "256",
        "--memtable_max_range_deletions", "0"
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    assert res.returncode != 0, f"[{test_name}] Driver succeeded unexpectedly on bad trace!"
    combined_err = res.stdout + "\n" + res.stderr
    assert expected_err_sub in combined_err, f"[{test_name}] Expected '{expected_err_sub}' in output, got:\n{combined_err}"
    
    if check_no_db_created:
        assert not os.path.exists(db_path), f"[{test_name}] DB directory was created before trace admission rejection!"

    if os.path.exists(db_path):
        shutil.rmtree(db_path)

    print(f"  [PASS] {test_name}: Driver rejected invalid trace before DB creation with error:\n    >> {expected_err_sub}")

def test_1_tampered_sha256():
    t_dir = os.path.join(TEST_SCRATCH, "case1_tampered_sha")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    target_bin = os.path.join(t_dir, "phase_a-worker-00.bin")
    with open(target_bin, "r+b") as f:
        f.seek(10)
        f.write(b"\xFF")
    run_driver_expect_fail(t_dir, "SHA-256 verification failed", "Case 1 (Tampered Checksum)")

def test_2_cross_partition_key():
    t_dir = os.path.join(TEST_SCRATCH, "case2_cross_partition")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    target_bin = os.path.join(t_dir, "phase_a-worker-00.bin")
    with open(target_bin, "r+b") as f:
        data = bytearray(f.read())
        rec_buf = struct.pack(RECORD_FORMAT, 0, 0, 1, 0, 1, 70000, 0)
        data[:24] = rec_buf
        f.seek(0)
        f.write(data)
    update_manifest_sha(t_dir, "phase_a-worker-00.bin")
    run_driver_expect_fail(t_dir, "out of partition", "Case 2 (Cross-Partition Bound Violation)")

def test_3_invalid_op_type():
    t_dir = os.path.join(TEST_SCRATCH, "case3_invalid_op_type")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    target_bin = os.path.join(t_dir, "phase_b-worker-01.bin")
    with open(target_bin, "r+b") as f:
        data = bytearray(f.read())
        rec_buf = struct.pack(RECORD_FORMAT, 1, 9, 1, 0, 100, 65000, 0)
        data[:24] = rec_buf
        f.seek(0)
        f.write(data)
    update_manifest_sha(t_dir, "phase_b-worker-01.bin")
    run_driver_expect_fail(t_dir, "Invalid op_type", "Case 3 (Invalid OpType 9)")

def test_4_duplicate_op_id():
    t_dir = os.path.join(TEST_SCRATCH, "case4_duplicate_op_id")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    target_bin = os.path.join(t_dir, "phase_c-worker-02.bin")
    with open(target_bin, "r+b") as f:
        data = bytearray(f.read())
        r0 = struct.unpack(RECORD_FORMAT, data[:24])
        dup_op_id = r0[4]
        r1 = struct.unpack(RECORD_FORMAT, data[24:48])
        r1_new = (r1[0], r1[1], r1[2], r1[3], dup_op_id, r1[5], r1[6])
        data[24:48] = struct.pack(RECORD_FORMAT, *r1_new)
        f.seek(0)
        f.write(data)
    update_manifest_sha(t_dir, "phase_c-worker-02.bin")
    run_driver_expect_fail(t_dir, "Duplicate op_id", "Case 4 (Duplicate OpId)")

def test_5_manifest_total_ops_mismatch():
    t_dir = os.path.join(TEST_SCRATCH, "case5_total_ops_mismatch")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    manifest_path = os.path.join(t_dir, "manifest.json")
    with open(manifest_path, "r") as f:
        m = json.load(f)
    m["actual_metrics"]["total_ops_count"] = 299999 # Wrong total ops
    with open(manifest_path, "w") as f:
        json.dump(m, f, indent=2)
    run_driver_expect_fail(t_dir, "total_ops_count", "Case 5 (Manifest Total Ops Mismatch)", check_no_db_created=True)

def test_6_manifest_value_size_mismatch():
    t_dir = os.path.join(TEST_SCRATCH, "case6_value_size_mismatch")
    shutil.copytree(VALID_TRACE_DIR, t_dir)
    manifest_path = os.path.join(t_dir, "manifest.json")
    with open(manifest_path, "r") as f:
        m = json.load(f)
    m["value_size"] = 128 # Driver expects 256
    with open(manifest_path, "w") as f:
        json.dump(m, f, indent=2)
    run_driver_expect_fail(t_dir, "value_size", "Case 6 (Manifest Value Size Mismatch)", check_no_db_created=True)

def verify_original_traces():
    print("\n--- Verifying Original Clean Trace Manifests & Payloads ---")
    manifest_path = os.path.join(VALID_TRACE_DIR, "manifest.json")
    with open(manifest_path, "r") as f:
        m = json.load(f)
    assert m["actual_metrics"]["total_ops_count"] == 300000, "Corrupted original total_ops_count"
    assert m["value_size"] == 256, "Corrupted original value_size"
    for fname, pinfo in m["payload_files"].items():
        fpath = os.path.join(VALID_TRACE_DIR, fname)
        assert os.path.exists(fpath), f"Missing {fname}"
        sha = compute_sha256(fpath)
        assert sha == pinfo["sha256"], f"SHA mismatch on original {fname}"
    print(f"  [PASS] Original Trace and Manifest SHA-256 100% verified.")

def main():
    print("=== Running Formal V2 Comprehensive Negative Trace & Admission Suite ===")
    if os.path.exists(TEST_SCRATCH):
        shutil.rmtree(TEST_SCRATCH)
    os.makedirs(TEST_SCRATCH, exist_ok=True)

    test_1_tampered_sha256()
    test_2_cross_partition_key()
    test_3_invalid_op_type()
    test_4_duplicate_op_id()
    test_5_manifest_total_ops_mismatch()
    test_6_manifest_value_size_mismatch()
    verify_original_traces()

    shutil.rmtree(TEST_SCRATCH)
    print("\n=========================================================")
    print("  ALL 6 NEGATIVE ADMISSION TESTS PASSED (100%)")
    print("=========================================================")

if __name__ == "__main__":
    main()
