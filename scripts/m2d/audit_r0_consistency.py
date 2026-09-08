#!/usr/bin/env python3
import os
import json
import glob
import hashlib
import struct
import csv
import re

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
RUN_DB_BASE = os.path.join(STUDY_ROOT, "run-db/m2d")
RESULTS_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d/release")
OUTPUT_AUDIT_DIR = os.path.join(STUDY_ROOT, "results/r0_audit")
os.makedirs(OUTPUT_AUDIT_DIR, exist_ok=True)

REPS = [1, 2, 3, 4, 5]
SEEDS = {1: 310001, 2: 320001, 3: 330001, 4: 340001, 5: 350001}
CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]
CFG_SLUGS = {
    "Native-T0": "native_t0",
    "Native-T512": "native_t512",
    "AMTV-T0": "amtv_t0",
    "AMTV-T512": "amtv_t512"
}

def audit_part1_traces():
    print("======================================================================")
    print("AUDIT PART 1: Trace and Logical State Reconciliation (20 Rounds)")
    print("======================================================================")
    
    rows = []
    
    for rep in REPS:
        seed = SEEDS[rep]
        trace_dir = os.path.join(STUDY_ROOT, f"traces/m2d_release_rep{rep}_seed{seed}")
        manifest_path = os.path.join(trace_dir, "manifest.json")
        
        with open(manifest_path, "rb") as fp:
            m_bytes = fp.read()
        manifest_sha = hashlib.sha256(m_bytes).hexdigest()
        manifest_data = json.loads(m_bytes.decode("utf-8"))
        
        read_proj_sha = manifest_data["read_projection_sha256"]
        write_proj_sha = manifest_data["write_projection_sha256"]
        
        # Parse binary trace files
        del_h = hashlib.sha256()
        total_del = 0
        total_put = 0
        total_get = 0
        
        for w in range(8):
            t_path = os.path.join(trace_dir, f"worker_{w}.trace")
            with open(t_path, "rb") as tf:
                while True:
                    buf = tf.read(24)
                    if not buf:
                        break
                    ph_id, op_t, scan_m, flags, op_id, k1, k2 = struct.unpack("<BBBB I QQ", buf)
                    if op_t == 3:
                        del_h.update(struct.pack("<H B I Q Q", w, ph_id, op_id, k1, k2))
                        total_del += 1
                    elif op_t == 2:
                        total_put += 1
                    elif op_t == 0:
                        total_get += 1
                        
        del_geo_sha = del_h.hexdigest()
        
        assert total_del == 20000, f"Rep {rep} total DeleteRange is {total_del} != 20000"
        assert total_put == 60000, f"Rep {rep} total Put is {total_put} != 60000"
        assert total_get == 220000, f"Rep {rep} total Get is {total_get} != 220000"
        
        rep_state_shas = {}
        for cfg in CONFIGS:
            slug = CFG_SLUGS[cfg]
            json_path = os.path.join(RESULTS_DIR, f"m2d_release_{slug}_rep{rep}.json")
            with open(json_path) as jfp:
                jdata = json.load(jfp)
            
            db_sha = jdata["db_sha256"]
            model_sha = jdata["expected_model_sha"]
            assert db_sha == model_sha, f"Mismatch in {cfg} rep {rep}: {db_sha} != {model_sha}"
            rep_state_shas[cfg] = db_sha
            
            rows.append({
                "rep": rep,
                "seed": seed,
                "config_name": cfg,
                "total_delete_range": total_del,
                "total_put": total_put,
                "total_get": total_get,
                "manifest_sha256": manifest_sha,
                "read_projection_sha256": read_proj_sha,
                "write_projection_sha256": write_proj_sha,
                "delete_range_geometry_sha256": del_geo_sha,
                "state_sha256": db_sha,
                "warmup_ops": 10000,
                "warmup_type": "Deterministic Read-Only GetLive"
            })
            
        # Verify identical state SHA across all 4 configs in same rep
        rep_shas = set(rep_state_shas.values())
        assert len(rep_shas) == 1, f"Rep {rep} has differing state SHAs across configs: {rep_state_shas}"
        print(f"[PASS] Rep {rep} (Seed {seed}): 4 configs 100% matched State SHA: {list(rep_shas)[0]}")

    out_csv = os.path.join(OUTPUT_AUDIT_DIR, "r0_trace_and_state_reconciliation.csv")
    with open(out_csv, "w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"Saved: {out_csv}")
    return rows

if __name__ == "__main__":
    audit_part1_traces()
