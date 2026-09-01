#!/usr/bin/env python3
import struct
import random
import os

def generate_b1_trace(filename, num_puts, num_del_ranges=20000, num_scans=40000, num_gets=60000, total_keys=500000, seed=80001):
    random.seed(seed)
    # Ops: 0=Get, 1=Scan, 2=Put, 3=DeleteRange
    # Format: uint8 op, uint64 k1, uint64 k2 (17 bytes per op)
    
    ops = []
    # Add Range Deletions
    for _ in range(num_del_ranges):
        b = random.randint(0, total_keys - 101)
        e = b + 100
        ops.append((3, b, e))
        
    # Add Puts
    for _ in range(num_puts):
        k = random.randint(0, total_keys - 1)
        ops.append((2, k, 0))
        
    # Add Scans
    for _ in range(num_scans):
        b = random.randint(0, total_keys - 101)
        ops.append((1, b, 100))
        
    # Add Gets
    for _ in range(num_gets):
        k = random.randint(0, total_keys - 1)
        ops.append((0, k, 0))
        
    # Shuffle to interleave uniformly
    random.shuffle(ops)
    
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    with open(filename, 'wb') as f:
        for op, k1, k2 in ops:
            f.write(struct.pack('<BQQ', op, k1, k2))
            
    print(f"Generated {filename}: {len(ops)} ops (Puts={num_puts}, DelRange={num_del_ranges}, Scans={num_scans}, Gets={num_gets})")

def generate_b2_trace(filename, total_ops=300000, total_keys=500000, seed=80002):
    random.seed(seed)
    # 3 consecutive phases:
    # Phase A (0 - 100k ops): Read-sensitive Range Delete Phase
    #   Get 60%, Scan 25%, Put 5%, DelRange 10%
    # Phase B (100k - 200k ops): Write-sensitive Burst Phase
    #   Put 60%, Get 25%, Scan 10%, DelRange 5%
    # Phase C (200k - 300k ops): Read Recovery Phase
    #   Get 65%, Scan 25%, Put 5%, DelRange 5%
    
    ops = []
    
    # Phase A: 100,000 ops
    ops_a = []
    for _ in range(10000): # 10% del
        b = random.randint(0, total_keys - 101)
        ops_a.append((3, b, b + 100))
    for _ in range(5000): # 5% put
        k = random.randint(0, total_keys - 1)
        ops_a.append((2, k, 0))
    for _ in range(25000): # 25% scan
        b = random.randint(0, total_keys - 101)
        ops_a.append((1, b, 100))
    for _ in range(60000): # 60% get
        k = random.randint(0, total_keys - 1)
        ops_a.append((0, k, 0))
    random.shuffle(ops_a)
    ops.extend(ops_a)
    
    # Phase B: 100,000 ops
    ops_b = []
    for _ in range(5000): # 5% del
        b = random.randint(0, total_keys - 101)
        ops_b.append((3, b, b + 100))
    for _ in range(60000): # 60% put (burst!)
        k = random.randint(0, total_keys - 1)
        ops_b.append((2, k, 0))
    for _ in range(10000): # 10% scan
        b = random.randint(0, total_keys - 101)
        ops_b.append((1, b, 100))
    for _ in range(25000): # 25% get
        k = random.randint(0, total_keys - 1)
        ops_b.append((0, k, 0))
    random.shuffle(ops_b)
    ops.extend(ops_b)
    
    # Phase C: 100,000 ops
    ops_c = []
    for _ in range(5000): # 5% del
        b = random.randint(0, total_keys - 101)
        ops_c.append((3, b, b + 100))
    for _ in range(5000): # 5% put
        k = random.randint(0, total_keys - 1)
        ops_c.append((2, k, 0))
    for _ in range(25000): # 25% scan
        b = random.randint(0, total_keys - 101)
        ops_c.append((1, b, 100))
    for _ in range(65000): # 65% get
        k = random.randint(0, total_keys - 1)
        ops_c.append((0, k, 0))
    random.shuffle(ops_c)
    ops.extend(ops_c)
    
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    with open(filename, 'wb') as f:
        for op, k1, k2 in ops:
            f.write(struct.pack('<BQQ', op, k1, k2))
            
    print(f"Generated {filename}: {len(ops)} ops (Phase A=100k, Phase B=100k, Phase C=100k)")

def main():
    base_dir = "/home/wam/grad/s14-range-delete-study/traces"
    # B1 traces
    generate_b1_trace(f"{base_dir}/b1/b1_put_025x.bin", num_puts=65536)
    generate_b1_trace(f"{base_dir}/b1/b1_put_050x.bin", num_puts=131072)
    generate_b1_trace(f"{base_dir}/b1/b1_put_075x.bin", num_puts=196608)
    generate_b1_trace(f"{base_dir}/b1/b1_put_125x.bin", num_puts=327680)
    
    # B2 trace
    generate_b2_trace(f"{base_dir}/b2/b2_dynamic_phases.bin")

if __name__ == "__main__":
    main()
