#!/usr/bin/env python3
"""
M3b Release N=5 Matrix Runner
Executes 20-run balanced interleaved schedule across 5 reps and 4 configurations.
Hardware constraints: fixed physical CPU affinity [0-19], numactl --interleave=all.
Prohibitions: drop_caches, sudo, sysctl, sleep, rate limiting, artificial flush, compact_range, or pauses between Phase B and C.
Hard stop conditions enforced on all 20 runs.
"""

import os
import sys
import json
import time
import subprocess
import shutil

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
SEED_DB = os.path.join(STUDY_ROOT, "run-db/m2d_canonical_seed_db")
BIN_RELEASE = os.path.join(STUDY_ROOT, "bin/m3a_driver_release")
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m3b_release")
RAW_OUTPUT_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3b/raw")

RELEASE_SEEDS = {
    1: 610001,
    2: 620001,
    3: 630001,
    4: 640001,
    5: 650001,
}

# Pre-registered 20-run balanced interleaved schedule
RELEASE_SCHEDULE = [
    # Rep 1 (seed 610001)
    ("Native-T0", 1, 610001),
    ("Native-T512", 1, 610001),
    ("AMTV-T0", 1, 610001),
    ("AMTV-T512", 1, 610001),
    # Rep 2 (seed 620001)
    ("Native-T512", 2, 620001),
    ("AMTV-T0", 2, 620001),
    ("AMTV-T512", 2, 620001),
    ("Native-T0", 2, 620001),
    # Rep 3 (seed 630001)
    ("AMTV-T0", 3, 630001),
    ("AMTV-T512", 3, 630001),
    ("Native-T0", 3, 630001),
    ("Native-T512", 3, 630001),
    # Rep 4 (seed 640001)
    ("AMTV-T512", 4, 640001),
    ("Native-T0", 4, 640001),
    ("Native-T512", 4, 640001),
    ("AMTV-T0", 4, 640001),
    # Rep 5 (seed 650001)
    ("Native-T0", 5, 650001),
    ("AMTV-T0", 5, 650001),
    ("Native-T512", 5, 650001),
    ("AMTV-T512", 5, 650001),
]

def run_single_release(cfg_name, rep, seed, seed_sha_registry):
    cfg_tag = cfg_name.lower().replace("-", "_")
    exp_id = f"m3b_release_{cfg_tag}_rep{rep}"
    trace_dir = os.path.join(STUDY_ROOT, f"traces/m3b_rel_rep{rep}_seed{seed}")
    db_path = os.path.join(RUN_DB_BASE, exp_id, "db")

    # Clean residual run-db directory
    if os.path.exists(os.path.dirname(db_path)):
        shutil.rmtree(os.path.dirname(db_path), ignore_errors=True)
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    os.makedirs(RAW_OUTPUT_DIR, exist_ok=True)

    cmd = [
        "taskset", "-c", "0-19",
        "numactl", "--interleave=all",
        BIN_RELEASE,
        "--exp-id", exp_id,
        "--config", cfg_name,
        "--mode", "release",
        "--rep", str(rep),
        "--db-path", db_path,
        "--seed-db", SEED_DB,
        "--trace-dir", trace_dir,
        "--output-dir", RAW_OUTPUT_DIR
    ]

    print(f"\n======================================================================")
    print(f"[M3b Release Run] {exp_id} ({cfg_name}, rep={rep}, seed={seed})")
    print(f"Command: {' '.join(cmd)}")
    print(f"======================================================================")

    t0 = time.time()
    res = subprocess.run(cmd)
    t_elapsed = time.time() - t0

    if res.returncode != 0:
        print(f"\n[FATAL HARD-STOP] Run {exp_id} failed with exit code {res.returncode}!", file=sys.stderr)
        sys.exit(res.returncode)

    json_path = os.path.join(RAW_OUTPUT_DIR, f"{exp_id}.json")
    if not os.path.exists(json_path):
        print(f"\n[FATAL HARD-STOP] Expected output JSON not found: {json_path}!", file=sys.stderr)
        sys.exit(1)

    with open(json_path, "r") as f:
        data = json.load(f)

    # 1. State Model Verification (300K Live, 200K Deleted)
    assert data["verified_live_keys"] == 300000, f"[HARD STOP] verified_live_keys != 300000 (got {data['verified_live_keys']})"
    assert data["verified_deleted_keys"] == 200000, f"[HARD STOP] verified_deleted_keys != 200000 (got {data['verified_deleted_keys']})"
    assert data["iterator_scan_visible_keys"] == 300000, f"[HARD STOP] iterator_scan_visible_keys != 300000 (got {data['iterator_scan_visible_keys']})"

    # 2. State SHA-256 Consistency check across 4 configs of same seed
    sha = data["db_state_sha256"]
    if seed not in seed_sha_registry:
        seed_sha_registry[seed] = sha
        print(f"  [REGISTER] Registered expected DB State SHA for Seed {seed}: {sha}")
    else:
        exp_sha = seed_sha_registry[seed]
        assert sha == exp_sha, f"[HARD STOP] State SHA mismatch for Seed {seed}! Expected {exp_sha}, got {sha}"
        print(f"  [PASS] DB State SHA matches Rep expectation: {sha}")

    # 3. AMTV Fallback must be zero
    if "AMTV" in cfg_name:
        assert data["fallback_events"] == 0, f"[HARD STOP] AMTV fallback_events != 0 (got {data['fallback_events']})"
        print(f"  [PASS] AMTV fallback events: 0")

    # 4. T0 vs T512 Invariants
    flushed = data["flushed_tombstones_total"]
    active = data["active_mem_tombstones"]
    flush_cnt = data["flush_count"]
    if "T0" in cfg_name:
        assert flush_cnt == 0, f"[HARD STOP] T0 produced unexpected natural capacity flushes: {flush_cnt}"
        assert active == 20000, f"[HARD STOP] T0 active tombstones != 20000 (got {active})"
        print(f"  [PASS] T0 Zero Flush & 20,000 MemTable Tombstone Conservation Verified.")
    elif "T512" in cfg_name:
        assert flush_cnt > 0, f"[HARD STOP] T512 failed to trigger flushes!"
        assert flushed + active == 20000, f"[HARD STOP] T512 conservation violated: Flushed={flushed} + Active={active} = {flushed+active} != 20000"
        for g in data.get("t512_generations", []):
            assert g["flush_reason"] == "Memtable Max Range Deletions", f"[HARD STOP] Non-threshold flush detected: {g}"
        print(f"  [PASS] T512 Tombstone Conservation Verified: Flushed={flushed} ({flush_cnt} flushes) + Active={active} = 20,000 (100% threshold flushes)")

    print(f"  [PASS] Run completed successfully in {t_elapsed:.2f} s")
    return data

def main():
    print("======================================================================")
    print("M3b Release N=5: Dynamic Mixed Read Load Performance Matrix (20 Runs)")
    print("Configs: Native-T0, Native-T512, AMTV-T0, AMTV-T512")
    print("Seeds:   610001, 620001, 630001, 640001, 650001")
    print("======================================================================")

    # Sanity check prerequisites
    assert os.path.exists(BIN_RELEASE), f"Missing release binary: {BIN_RELEASE}"
    assert os.path.exists(SEED_DB), f"Missing canonical seed db: {SEED_DB}"
    for rep, seed in RELEASE_SEEDS.items():
        tdir = os.path.join(STUDY_ROOT, f"traces/m3b_rel_rep{rep}_seed{seed}")
        assert os.path.exists(tdir), f"Missing trace dir: {tdir}"

    seed_sha_registry = {}
    completed_runs = []
    matrix_t0 = time.time()

    for idx, (cfg_name, rep, seed) in enumerate(RELEASE_SCHEDULE, 1):
        print(f"\n>>> Starting Matrix Step {idx}/20: {cfg_name} (Rep {rep}, Seed {seed})")
        data = run_single_release(cfg_name, rep, seed, seed_sha_registry)
        completed_runs.append({
            "step": idx,
            "config": cfg_name,
            "rep": rep,
            "seed": seed,
            "db_state_sha256": data["db_state_sha256"],
            "phase_a_iops": data["phases"][0]["iops"],
            "phase_b_iops": data["phases"][1]["iops"],
            "phase_c_iops": data["phases"][2]["iops"],
        })

    matrix_elapsed = time.time() - matrix_t0
    print("\n======================================================================")
    print(f"ALL 20 M3b Release N=5 Runs Completed Successfully in {matrix_elapsed:.2f} s!")
    print("======================================================================")

    out_summary_json = os.path.join(STUDY_ROOT, "results/amtv_m3b/m3b_matrix_manifest.json")
    with open(out_summary_json, "w") as f:
        json.dump(completed_runs, f, indent=2)
    print(f"Manifest written to {out_summary_json}")

if __name__ == '__main__':
    main()
