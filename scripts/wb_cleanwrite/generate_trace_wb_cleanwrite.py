#!/usr/bin/env python3
import os
import struct
import random

def main():
    trace_dir = "/home/wam/grad/s14-range-delete-study/traces/wb_cleanwrite"
    os.makedirs(trace_dir, exist_ok=True)
    trace_path = os.path.join(trace_dir, "wb_cleanwrite.bin")

    TOTAL_OPS = 1310720
    NUM_KEYS = 1000000
    SEED = 80003

    rng = random.Random(SEED)

    # Operations: Put 80%, Get 10%, Scan 10%, DeleteRange 0%
    # Op codes: 0=Get, 1=Scan, 2=Put, 3=DeleteRange
    print(f"Generating deterministic WB-CleanWrite trace: {TOTAL_OPS} ops (80% Put, 10% Get, 10% Scan)...")
    
    with open(trace_path, "wb") as f:
        for _ in range(TOTAL_OPS):
            r = rng.random()
            if r < 0.80:
                # Put
                k = rng.randint(0, NUM_KEYS - 1)
                f.write(struct.pack("<BQQ", 2, k, 0))
            elif r < 0.90:
                # Get
                k = rng.randint(0, NUM_KEYS - 1)
                f.write(struct.pack("<BQQ", 0, k, 0))
            else:
                # Scan
                b = rng.randint(0, NUM_KEYS - 1)
                span = 100
                f.write(struct.pack("<BQQ", 1, b, span))

    size_mb = os.path.getsize(trace_path) / (1024.0 * 1024.0)
    print(f"Generated {trace_path} ({size_mb:.2f} MB)")

if __name__ == "__main__":
    main()
