#!/usr/bin/env python3
import struct
import random
import os

def generate_ls24_trace(filename, total_ops=300000, total_keys=100663296, seed=80002):
    random.seed(seed)
    
    # 3 consecutive phases identical to B2:
    # Phase A (0 - 100k ops): Read-sensitive Range Delete Phase
    #   Get 60%, Scan 25%, Put 5%, DelRange 10%
    # Phase B (100k - 200k ops): Write-sensitive Burst Phase
    #   Put 60%, Get 25%, Scan 10%, DelRange 5%
    # Phase C (200k - 300k ops): Read Recovery Phase
    #   Get 65%, Scan 25%, Put 5%, DelRange 5%
    
    ops = []
    del_span = 100 # B2 DeleteRange length
    scan_span = 100 # B2 Scan length
    
    # Phase A: 100,000 ops
    ops_a = []
    for _ in range(10000): # 10% del
        b = random.randint(0, total_keys - del_span - 1)
        ops_a.append((3, b, b + del_span))
    for _ in range(5000): # 5% put
        k = random.randint(0, total_keys - 1)
        ops_a.append((2, k, 0))
    for _ in range(25000): # 25% scan
        b = random.randint(0, total_keys - scan_span - 1)
        ops_a.append((1, b, scan_span))
    for _ in range(60000): # 60% get
        k = random.randint(0, total_keys - 1)
        ops_a.append((0, k, 0))
    random.shuffle(ops_a)
    ops.extend(ops_a)
    
    # Phase B: 100,000 ops
    ops_b = []
    for _ in range(5000): # 5% del
        b = random.randint(0, total_keys - del_span - 1)
        ops_b.append((3, b, b + del_span))
    for _ in range(60000): # 60% put (burst!)
        k = random.randint(0, total_keys - 1)
        ops_b.append((2, k, 0))
    for _ in range(10000): # 10% scan
        b = random.randint(0, total_keys - scan_span - 1)
        ops_b.append((1, b, scan_span))
    for _ in range(25000): # 25% get
        k = random.randint(0, total_keys - 1)
        ops_b.append((0, k, 0))
    random.shuffle(ops_b)
    ops.extend(ops_b)
    
    # Phase C: 100,000 ops
    ops_c = []
    for _ in range(5000): # 5% del
        b = random.randint(0, total_keys - del_span - 1)
        ops_c.append((3, b, b + del_span))
    for _ in range(5000): # 5% put
        k = random.randint(0, total_keys - 1)
        ops_c.append((2, k, 0))
    for _ in range(25000): # 25% scan
        b = random.randint(0, total_keys - scan_span - 1)
        ops_c.append((1, b, scan_span))
    for _ in range(65000): # 65% get
        k = random.randint(0, total_keys - 1)
        ops_c.append((0, k, 0))
    random.shuffle(ops_c)
    ops.extend(ops_c)
    
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    with open(filename, 'wb') as f:
        for op, k1, k2 in ops:
            f.write(struct.pack('<BQQ', op, k1, k2))
            
    print(f"Generated {filename}: {len(ops)} ops across key space [0, {total_keys})")

if __name__ == "__main__":
    out_file = "/home/wam/grad/s14-range-delete-study/traces/ls24/ls24_dynamic_phases.bin"
    generate_ls24_trace(out_file)
