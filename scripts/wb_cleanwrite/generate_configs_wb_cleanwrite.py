#!/usr/bin/env python3
import os

configs = [
    ("wb_cleanwrite_16mb", 16777216, "WB-CleanWrite: write_buffer_size=16MB"),
    ("wb_cleanwrite_64mb", 67108864, "WB-CleanWrite: write_buffer_size=64MB"),
    ("wb_cleanwrite_128mb", 134217728, "WB-CleanWrite: write_buffer_size=128MB"),
]

cfg_dir = "/home/wam/grad/s14-range-delete-study/configs/wb_cleanwrite"
os.makedirs(cfg_dir, exist_ok=True)

for cid, wb_sz, desc in configs:
    content = f"""# WB-CleanWrite Experiment: {cid}
exp_id={cid}
group_name=wb_cleanwrite
desc={desc}
value_size=256
memtable_max_range_deletions=0
total_keys=1000000
total_ops=1310720
trace_path=./traces/wb_cleanwrite/wb_cleanwrite.bin
num_threads=8
random_seed=80003
block_cache_size=134217728
write_buffer_size={wb_sz}
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = os.path.join(cfg_dir, f"{cid}.ini")
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
