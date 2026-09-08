#!/usr/bin/env python3
import os
import sys
import json
import time
import subprocess
import argparse

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
SEED_DB = os.path.join(STUDY_ROOT, "run-db/m2d_canonical_seed_db")
BIN_AUDIT = os.path.join(STUDY_ROOT, "bin/m3a_driver_audit")
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m3a")
RAW_OUTPUT_DIR = os.path.join(STUDY_ROOT, "results/amtv_m3a/raw")

SMOKE_SEED = 90001
AUDIT_SEEDS = {
    1: 510001,
    2: 520001,
    3: 530001,
}

CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]

SMOKE_SCHEDULE = [
    ("Native-T0", 0, SMOKE_SEED),
    ("Native-T512", 0, SMOKE_SEED),
    ("AMTV-T0", 0, SMOKE_SEED),
    ("AMTV-T512", 0, SMOKE_SEED),
]

AUDIT_SCHEDULE = [
    # Rep 1 (seed 510001): Native-T0 -> Native-T512 -> AMTV-T0 -> AMTV-T512
    ("Native-T0", 1, 510001),
    ("Native-T512", 1, 510001),
    ("AMTV-T0", 1, 510001),
    ("AMTV-T512", 1, 510001),
    # Rep 2 (seed 520001): Native-T512 -> AMTV-T0 -> AMTV-T512 -> Native-T0
    ("Native-T512", 2, 520001),
    ("AMTV-T0", 2, 520001),
    ("AMTV-T512", 2, 520001),
    ("Native-T0", 2, 520001),
    # Rep 3 (seed 530001): AMTV-T0 -> AMTV-T512 -> Native-T0 -> Native-T512
    ("AMTV-T0", 3, 530001),
    ("AMTV-T512", 3, 530001),
    ("Native-T0", 3, 530001),
    ("Native-T512", 3, 530001),
]

SEED_STATE_SHAS = {
    SMOKE_SEED: "1ce8ebf73b80732dc076293d88fcef22668c19a87a27d0aab890e7c3a9a895a6"
}

def run_single(cfg_name, rep, seed, is_smoke=False):
    prefix = "m3a_smoke" if is_smoke else "m3a_audit"
    cfg_tag = cfg_name.lower().replace("-", "_")
    exp_id = f"{prefix}_{cfg_tag}_seed{seed}" if is_smoke else f"{prefix}_{cfg_tag}_rep{rep}"
    
    if is_smoke:
        trace_dir = os.path.join(STUDY_ROOT, f"traces/m3a_smoke_seed{seed}")
    else:
        trace_dir = os.path.join(STUDY_ROOT, f"traces/m3a_rep{rep}_seed{seed}")
        
    db_path = os.path.join(RUN_DB_BASE, exp_id, "db")
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    os.makedirs(RAW_OUTPUT_DIR, exist_ok=True)
    
    cmd = [
        "numactl", "--interleave=all",
        BIN_AUDIT,
        "--exp-id", exp_id,
        "--config", cfg_name,
        "--mode", "audit",
        "--rep", str(rep),
        "--db-path", db_path,
        "--seed-db", SEED_DB,
        "--trace-dir", trace_dir,
        "--output-dir", RAW_OUTPUT_DIR
    ]
    
    print(f"\n======================================================================")
    print(f"Executing: {exp_id} ({cfg_name}, rep={rep}, seed={seed})")
    print(f"Command: {' '.join(cmd)}")
    print(f"======================================================================")
    
    t0 = time.time()
    res = subprocess.run(cmd)
    t_elapsed = time.time() - t0
    
    if res.returncode != 0:
        print(f"\n[FATAL ERROR] {exp_id} failed with exit code {res.returncode}")
        sys.exit(res.returncode)
        
    json_path = os.path.join(RAW_OUTPUT_DIR, f"{exp_id}.json")
    if not os.path.exists(json_path):
        print(f"\n[FATAL ERROR] Expected output JSON {json_path} does not exist!")
        sys.exit(1)
        
    with open(json_path, "r") as f:
        data = json.load(f)
        
    # Invariant validations
    state_sha = data.get("db_state_sha256", "")
    assert len(state_sha) == 64, f"Invalid state SHA format: {state_sha}"
    if seed in SEED_STATE_SHAS:
        assert state_sha == SEED_STATE_SHAS[seed], f"State SHA mismatch within seed {seed}: expected {SEED_STATE_SHAS[seed]}, got {state_sha}"
    else:
        SEED_STATE_SHAS[seed] = state_sha
    
    assert data.get("verified_live_keys") == 300000, f"Live keys mismatch: {data.get('verified_live_keys')}"
    assert data.get("verified_deleted_keys") == 200000, f"Deleted keys mismatch: {data.get('verified_deleted_keys')}"
    
    if "AMTV" in cfg_name:
        fallback_events = data.get("fallback_events", 0)
        assert fallback_events == 0, f"AMTV Fallback occurred: {fallback_events}"
        
    if cfg_name.endswith("-T0"):
        flush_count = data.get("flush_count", 0)
        assert flush_count == 0, f"T0 had unexpected flushes: {flush_count}"
        
    if cfg_name.endswith("-T512"):
        flushed_tb = data.get("flushed_tombstones_total", 0)
        active_tb = data.get("active_mem_tombstones", 0)
        total_tb = flushed_tb + active_tb
        assert total_tb == 20000, f"T512 tombstone conservation violated: flushed={flushed_tb}, active={active_tb}, total={total_tb}"
        
    print(f"[SUCCESS] {exp_id} completed cleanly in {t_elapsed:.2f}s.")
    print(f"  State SHA: {state_sha} [VERIFIED]")
    if cfg_name.endswith("-T512"):
        print(f"  Flushes: {data.get('flush_count')}, Flushed TB: {data.get('flushed_tombstones_total')}, Active TB: {data.get('active_mem_tombstones')}")

def main():
    parser = argparse.ArgumentParser(description="M3a Audit Runner")
    parser.add_argument("--smoke", action="store_true", help="Run 4-group Audit Smoke")
    parser.add_argument("--matrix", action="store_true", help="Run 12-round Audit N=3 Matrix")
    parser.add_argument("--all", action="store_true", help="Run Smoke followed by Matrix")
    args = parser.parse_args()
    
    if not (args.smoke or args.matrix or args.all):
        parser.print_help()
        sys.exit(1)
        
    if args.smoke or args.all:
        print("\n" + "#" * 70)
        print("STARTING M3a AUDIT SMOKE (4 Configurations, Seed 90001)")
        print("#" * 70)
        for cfg, rep, seed in SMOKE_SCHEDULE:
            run_single(cfg, rep, seed, is_smoke=True)
            time.sleep(1)
            
    if args.matrix or args.all:
        print("\n" + "#" * 70)
        print("STARTING M3a AUDIT N=3 MATRIX (12 Runs, Seeds 510001, 520001, 530001)")
        print("#" * 70)
        for cfg, rep, seed in AUDIT_SCHEDULE:
            run_single(cfg, rep, seed, is_smoke=False)
            time.sleep(1)

if __name__ == "__main__":
    main()
