#!/usr/bin/env python3
import os

thresholds = [0, 64, 256, 2048]
os.makedirs("/home/wam/grad/s14-range-delete-study/configs/b2", exist_ok=True)

for t in thresholds:
    t_str = f"t{t:04d}"
    cid = f"b2_dynamic_{t_str}"
    content = f"""# B2: Dynamic Phase Mismatch Experiment ({cid})
exp_id={cid}
group_name=b2_dynamic_phase_mismatch
desc=B2 Dynamic 3-Phase Workload (Threshold T={t})
value_size=256
memtable_max_range_deletions={t}
total_keys=500000
total_ops=300000
trace_path=./traces/b2/b2_dynamic_phases.bin
num_threads=8
random_seed=80002
block_cache_size=134217728
write_buffer_size=67108864
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = f"/home/wam/grad/s14-range-delete-study/configs/b2/{cid}.ini"
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
