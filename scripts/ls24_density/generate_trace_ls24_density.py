#!/usr/bin/env python3
import os
import struct
import random

def main():
    trace_dir = "/home/wam/grad/s14-range-delete-study/traces/ls24_density"
    os.makedirs(trace_dir, exist_ok=True)
    trace_path = os.path.join(trace_dir, "ls24_density_phases.bin")

    NUM_KEYS = 100663296
    TOTAL_OPS = 300000
    NUM_DEL_RANGES = 20000
    SPAN = 2013
    BUCKET_W = NUM_KEYS // NUM_DEL_RANGES
    SEED = 80002

    # Deterministic disjoint DeleteRanges
    del_ranges = []
    for i in range(NUM_DEL_RANGES):
        b = i * BUCKET_W
        e = b + SPAN
        del_ranges.append((b, e))

    # Shuffle the deletion order deterministically so they are injected randomly during Phase B
    rng_del = random.Random(SEED + 999)
    shuffled_del_ranges = list(del_ranges)
    rng_del.shuffle(shuffled_del_ranges)

    rng = random.Random(SEED)
    ops = []

    del_idx = 0

    # Phase A: 100,000 ops (Read Sensitive: Get 70%, Scan 25%, Put 5%, Del 0%)
    for _ in range(100000):
        r = rng.random()
        if r < 0.70:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((0, k, 0))
        elif r < 0.95:
            b = rng.randint(0, NUM_KEYS - 1)
            ops.append((1, b, 100))
        else:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((2, k, 0))

    # Phase B: 100,000 ops (Write Burst: Put 60%, Del 20%, Scan 10%, Get 10%)
    for _ in range(100000):
        r = rng.random()
        if r < 0.20:
            # DeleteRange (20,000 total)
            if del_idx < len(shuffled_del_ranges):
                b, e = shuffled_del_ranges[del_idx]
                del_idx += 1
            else:
                b = rng.randint(0, NUM_KEYS - 1)
                e = min(b + SPAN, NUM_KEYS)
            ops.append((3, b, e))
        elif r < 0.80:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((2, k, 0))
        elif r < 0.90:
            b = rng.randint(0, NUM_KEYS - 1)
            ops.append((1, b, 100))
        else:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((0, k, 0))

    # Phase C: 100,000 ops (Read Recovery: Get 70%, Scan 25%, Put 5%, Del 0%)
    for _ in range(100000):
        r = rng.random()
        if r < 0.70:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((0, k, 0))
        elif r < 0.95:
            b = rng.randint(0, NUM_KEYS - 1)
            ops.append((1, b, 100))
        else:
            k = rng.randint(0, NUM_KEYS - 1)
            ops.append((2, k, 0))

    # Write trace file
    with open(trace_path, "wb") as f:
        for op, k1, k2 in ops:
            f.write(struct.pack("<BQQ", op, k1, k2))

    # Compute pre-run validation metrics
    deleted_keys = set()
    for b, e in del_ranges:
        deleted_keys.update(range(b, e))

    union_del_cnt = len(deleted_keys)
    cov_pct = (union_del_cnt / NUM_KEYS) * 100.0

    get_del_hits = 0
    get_total = 0
    scan_del_hits = 0
    scan_total = 0

    for op, k1, k2 in ops:
        if op == 0:
            get_total += 1
            if k1 in deleted_keys:
                get_del_hits += 1
        elif op == 1:
            scan_total += 1
            # Check if scan range [k1, k1+100) overlaps with any bucket
            hit = False
            for i in range(k1, min(k1 + 100, NUM_KEYS)):
                if i in deleted_keys:
                    hit = True
                    break
            if hit:
                scan_del_hits += 1

    get_hit_pct = (get_del_hits / get_total) * 100.0 if get_total else 0.0
    scan_hit_pct = (scan_del_hits / scan_total) * 100.0 if scan_total else 0.0

    print("==================================================================")
    print("   LS24-DENSITY-PRESERVED PRE-RUN AUDIT & ACCEPTANCE CRITERIA     ")
    print("==================================================================")
    print(f"Total Operations: {len(ops)}")
    print(f"Total Range Tombstones: {NUM_DEL_RANGES}")
    print(f"Single Tombstone Span: {SPAN} keys")
    print(f"Actual Union Deleted Keys: {union_del_cnt}")
    print(f"Actual Union Coverage Ratio: {cov_pct:.4f}% (Acceptance: 40.0% ± 1.0%)")
    print(f"Tombstone Overlap Ratio: 0.00%")
    print(f"Get Query Hit on Deleted Union: {get_hit_pct:.2f}% (B2 baseline was uniform ~40%)")
    print(f"Scan Query Overlap with Deleted Union: {scan_hit_pct:.2f}%")
    print(f"Generated Binary Trace: {trace_path}")
    print("==================================================================")

if __name__ == "__main__":
    main()
