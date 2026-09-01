#!/usr/bin/env python3
import os

configs = [
    ("b1_cf_wb_016mb", "B1 Counterfactual: write_buffer_size=16MB", 16 * 1024 * 1024),
    ("b1_cf_wb_064mb", "B1 Counterfactual: write_buffer_size=64MB", 64 * 1024 * 1024),
    ("b1_cf_wb_128mb", "B1 Counterfactual: write_buffer_size=128MB", 128 * 1024 * 1024),
]

os.makedirs("/home/wam/grad/s14-range-delete-study/configs/b1_cf", exist_ok=True)

for cid, desc, wb_sz in configs:
    content = f"""# B1-CF: Natural Flush Counterfactual Experiment ({cid})
exp_id={cid}
group_name=b1_natural_flush_counterfactual
desc={desc}
value_size=256
memtable_max_range_deletions=0
total_keys=500000
total_ops=251072
trace_path=./traces/b1/b1_put_050x.bin
num_threads=8
random_seed=80001
block_cache_size=134217728
write_buffer_size={wb_sz}
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = f"/home/wam/grad/s14-range-delete-study/configs/b1_cf/{cid}.ini"
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
