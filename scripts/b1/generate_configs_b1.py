#!/usr/bin/env python3
import os

configs = [
    ("b1_put_025x", "B1 Put 0.25x (16MB logical)", "./traces/b1/b1_put_025x.bin", 185536),
    ("b1_put_050x", "B1 Put 0.50x (32MB logical)", "./traces/b1/b1_put_050x.bin", 251072),
    ("b1_put_075x", "B1 Put 0.75x (48MB logical)", "./traces/b1/b1_put_075x.bin", 316608),
    ("b1_put_125x", "B1 Put 1.25x (80MB logical)", "./traces/b1/b1_put_125x.bin", 447680),
]

os.makedirs("/home/wam/grad/s14-range-delete-study/configs/b1", exist_ok=True)

for cid, desc, trace, ops in configs:
    content = f"""# B1: Natural Flush Boundary Experiment ({cid})
exp_id={cid}
group_name=b1_natural_flush_boundary
desc={desc}
value_size=256
memtable_max_range_deletions=0
total_keys=500000
total_ops={ops}
trace_path={trace}
num_threads=8
random_seed=80001
block_cache_size=134217728
write_buffer_size=67108864
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = f"/home/wam/grad/s14-range-delete-study/configs/b1/{cid}.ini"
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
