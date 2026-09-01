#!/usr/bin/env python3
"""
Generates deterministic DeleteRange trace and frontend request manifest for P6.
Computes SHA-256 checksums for rigorous experiment provenance.
"""
import os
import hashlib
import struct

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TRACE_DIR = os.path.join(BASE_DIR, "traces", "p6")
os.makedirs(TRACE_DIR, exist_ok=True)

TOTAL_KEYS = 1000000
NUM_DEL_RANGES = 4000
DEL_SPAN = 100
STEP = TOTAL_KEYS // NUM_DEL_RANGES # 250

def generate_deleterange_trace():
    txt_path = os.path.join(TRACE_DIR, "deleterange_trace.txt")
    bin_path = os.path.join(TRACE_DIR, "deleterange_trace.bin")

    lines = []
    binary_data = bytearray()

    total_del_keys = 0
    for i in range(NUM_DEL_RANGES):
        b = i * STEP + 75
        e = b + DEL_SPAN # [b, e) -> 100 keys
        lines.append(f"{b} {e}\n")
        binary_data.extend(struct.pack("<QQ", b, e))
        total_del_keys += (e - b)

    assert total_del_keys == 400000, f"Expected 400000 deleted keys, got {total_del_keys}"

    with open(txt_path, "w") as f:
        f.writelines(lines)

    with open(bin_path, "wb") as f:
        f.write(binary_data)

    txt_sha = hashlib.sha256(open(txt_path, "rb").read()).hexdigest()
    bin_sha = hashlib.sha256(binary_data).hexdigest()

    print(f"Generated DeleteRange trace ({NUM_DEL_RANGES} ranges, total deleted {total_del_keys} keys):")
    print(f"  TXT: {txt_path} (SHA-256: {txt_sha})")
    print(f"  BIN: {bin_path} (SHA-256: {bin_sha})")
    return txt_sha, bin_sha

def generate_frontend_trace():
    # Pre-generate 5,000,000 deterministic frontend operations (seed=60001)
    # Ops: 0=Get, 1=RangeScan, 2=Put
    # Ratios: 75% Get, 20% Scan, 5% Put
    import random
    rng = random.Random(60001)

    bin_path = os.path.join(TRACE_DIR, "frontend_ops_trace.bin")
    
    # Pre-build list of all 600,000 live keys in L
    live_keys = []
    for i in range(NUM_DEL_RANGES):
        # surviving: [i*250, i*250+75) and [i*250+175, (i+1)*250)
        for k in range(i * STEP, i * STEP + 75):
            live_keys.append(k)
        for k in range(i * STEP + 175, (i + 1) * STEP):
            live_keys.append(k)

    assert len(live_keys) == 600000, f"Expected 600000 live keys, got {len(live_keys)}"

    # Generate 2,000,000 deterministic frontend requests
    NUM_OPS = 2000000
    records = bytearray()
    
    # struct format: <B Q Q (op_type: uint8, key1: uint64, key2: uint64)
    # op 0: Get (target_key, 0)
    # op 1: RangeScan (begin_key, scan_span)
    # op 2: Put (target_key, 0)
    for _ in range(NUM_OPS):
        r = rng.random()
        if r < 0.05:
            # Put (5%) -> update a live key in L
            k = live_keys[rng.randint(0, len(live_keys) - 1)]
            records.extend(struct.pack("<BQQ", 2, k, 0))
        elif r < 0.80:
            # Get (75%) -> target a live key in L
            k = live_keys[rng.randint(0, len(live_keys) - 1)]
            records.extend(struct.pack("<BQQ", 0, k, 0))
        else:
            # RangeScan (20%) -> scan window of 100
            b = rng.randint(0, TOTAL_KEYS - 100)
            records.extend(struct.pack("<BQQ", 1, b, 100))

    with open(bin_path, "wb") as f:
        f.write(records)

    fe_sha = hashlib.sha256(records).hexdigest()
    print(f"Generated Frontend trace ({NUM_OPS} deterministic ops):")
    print(f"  BIN: {bin_path} (Size: {len(records) / (1024*1024):.2f} MB, SHA-256: {fe_sha})")
    return fe_sha

if __name__ == "__main__":
    generate_deleterange_trace()
    generate_frontend_trace()
