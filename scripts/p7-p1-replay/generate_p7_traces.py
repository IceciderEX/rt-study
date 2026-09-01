#!/usr/bin/env python3
"""
Generates deterministic 200,000-op request traces for P7 (ratios 0.0%, 0.5%, 1.0%, 2.0%, 5.0%, 10.0%).
Computes SHA-256 checksums for each trace.
"""
import os
import hashlib
import struct
import random

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TRACE_DIR = os.path.join(BASE_DIR, "traces", "p7")
os.makedirs(TRACE_DIR, exist_ok=True)

TOTAL_KEYS = 500000
TOTAL_OPS = 200000
SCAN_SPAN = 100
DEL_RANGE_LEN = 100

# Format: <B Q Q (op_type: uint8, key1: uint64, key2: uint64)
# op 0: Get (target_key, 0)
# op 1: Scan (begin_key, span)
# op 2: Put (target_key, 0)
# op 3: DeleteRange (begin_key, end_key)

CONFIGS = [
    ("p7_trace_ratio_000.bin", 0.0, 70001),
    ("p7_trace_ratio_005.bin", 0.005, 70002),
    ("p7_trace_ratio_010.bin", 0.010, 70003),
    ("p7_trace_ratio_020.bin", 0.020, 70004),
    ("p7_trace_ratio_050.bin", 0.050, 70005),
    ("p7_trace_ratio_100.bin", 0.100, 70006),
]

def generate_traces():
    checksums = {}
    for filename, del_ratio, seed in CONFIGS:
        rng = random.Random(seed)
        records = bytearray()
        
        del_count = int(round(TOTAL_OPS * del_ratio))
        put_count = int(round(TOTAL_OPS * 0.05))
        scan_count = int(round(TOTAL_OPS * 0.20))
        get_count = TOTAL_OPS - del_count - put_count - scan_count
        
        # Build list of operations
        op_types = [3] * del_count + [2] * put_count + [1] * scan_count + [0] * get_count
        rng.shuffle(op_types)
        
        assert len(op_types) == TOTAL_OPS
        
        for op in op_types:
            if op == 0:
                # Get
                k = rng.randint(0, TOTAL_KEYS - 1)
                records.extend(struct.pack("<BQQ", 0, k, 0))
            elif op == 1:
                # Scan
                b = rng.randint(0, TOTAL_KEYS - SCAN_SPAN)
                records.extend(struct.pack("<BQQ", 1, b, SCAN_SPAN))
            elif op == 2:
                # Put
                k = rng.randint(0, TOTAL_KEYS - 1)
                records.extend(struct.pack("<BQQ", 2, k, 0))
            elif op == 3:
                # DeleteRange
                b = rng.randint(0, TOTAL_KEYS - DEL_RANGE_LEN)
                e = b + DEL_RANGE_LEN
                records.extend(struct.pack("<BQQ", 3, b, e))
                
        out_path = os.path.join(TRACE_DIR, filename)
        with open(out_path, "wb") as f:
            f.write(records)
            
        sha = hashlib.sha256(records).hexdigest()
        checksums[filename] = (sha, del_count, get_count, scan_count, put_count)
        print(f"Generated {filename} ({TOTAL_OPS} ops, {del_ratio*100:.1f}% DeleteRange = {del_count} ops, SHA-256: {sha})")
        
    return checksums

if __name__ == "__main__":
    generate_traces()
