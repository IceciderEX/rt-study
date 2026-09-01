#!/usr/bin/env python3
import os

configs = [
    ("ls24_pilot_t0256", 256, "LS24 Pilot: Large-Scale 24GiB Validation (T=256)"),
    ("ls24_t0000", 0, "LS24 Formal: 24GiB Workload (T=0 Disabled)"),
    ("ls24_t0256", 256, "LS24 Formal: 24GiB Workload (T=256 Compromise)"),
    ("ls24_t2048", 2048, "LS24 Formal: 24GiB Workload (T=2048 Conservative)"),
]

os.makedirs("/home/wam/grad/s14-range-delete-study/configs/ls24", exist_ok=True)

for cid, t, desc in configs:
    content = f"""# LS24: 24GiB Large-Scale Validation ({cid})
exp_id={cid}
group_name=ls24_large_scale
desc={desc}
value_size=256
memtable_max_range_deletions={t}
total_keys=100663296
total_ops=300000
trace_path=./traces/ls24/ls24_dynamic_phases.bin
num_threads=8
random_seed=80002
block_cache_size=134217728
write_buffer_size=67108864
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = f"/home/wam/grad/s14-range-delete-study/configs/ls24/{cid}.ini"
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
