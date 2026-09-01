#!/usr/bin/env python3
import os

configs = [
    ("ls24_dp_pilot_t0256", 256, "LS24-DensityPreserved Pilot (T=256, 40% Coverage)"),
    ("ls24_dp_t0000", 0, "LS24-DensityPreserved Formal (T=0 Disabled, 40% Coverage)"),
    ("ls24_dp_t0256", 256, "LS24-DensityPreserved Formal (T=256 Compromise, 40% Coverage)"),
    ("ls24_dp_t2048", 2048, "LS24-DensityPreserved Formal (T=2048 Conservative, 40% Coverage)"),
]

cfg_dir = "/home/wam/grad/s14-range-delete-study/configs/ls24_density"
os.makedirs(cfg_dir, exist_ok=True)

for cid, t, desc in configs:
    content = f"""# LS24-DensityPreserved Experiment: {cid}
exp_id={cid}
group_name=ls24_density_preserved
desc={desc}
value_size=256
memtable_max_range_deletions={t}
total_keys=100663296
total_ops=300000
trace_path=./traces/ls24_density/ls24_density_phases.bin
num_threads=8
random_seed=80002
block_cache_size=134217728
write_buffer_size=67108864
max_write_buffer_number=4
level0_file_num_compaction_trigger=4
max_background_jobs=8
"""
    path = os.path.join(cfg_dir, f"{cid}.ini")
    with open(path, "w") as f:
        f.write(content)
    print(f"Created {path}")
