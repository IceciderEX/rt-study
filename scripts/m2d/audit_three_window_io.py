#!/usr/bin/env python3
import os
import json
import csv

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
OUTPUT_AUDIT_DIR = os.path.join(STUDY_ROOT, "results/r0_audit")
os.makedirs(OUTPUT_AUDIT_DIR, exist_ok=True)

REPS = [1, 2, 3, 4, 5]
CONFIGS = [("Native-T0", "native_t0"), ("Native-T512", "native_t512"), ("AMTV-T0", "amtv_t0"), ("AMTV-T512", "amtv_t512")]

def parse_three_window_io():
    print("======================================================================")
    print("AUDIT PART 3: Three-Window Output Consistency Audit (20 Rounds)")
    print("======================================================================")

    rows = []

    for cfg_name, slug in CONFIGS:
        for rep in REPS:
            json_path = os.path.join(STUDY_ROOT, f"results/amtv_m2d/release/m2d_release_{slug}_rep{rep}.json")
            with open(json_path) as jfp:
                d = json.load(jfp)
                
            b = d["boundary_telemetry"]
            b0 = b["b0_baseline"]
            b1 = b["b1_foreground_end"]
            b2 = b["b2_cooldown_end"]
            b3 = b["b3_drain_end"]
            
            w1_f = d["window1_foreground_flush_bytes"]
            w2_f = d["window2_cooldown_flush_bytes"]
            w3_f = d["window3_drain_flush_bytes"]
            tot_f = d["three_window_flush_bytes"]
            
            w1_cw = d["window1_foreground_compaction_write_bytes"]
            w2_cw = d["window2_cooldown_compaction_write_bytes"]
            w3_cw = d["window3_drain_compaction_write_bytes"]
            tot_cw = d["three_window_compaction_write_bytes"]
            
            w1_cr = d["window1_foreground_compaction_read_bytes"]
            w2_cr = d["window2_cooldown_compaction_read_bytes"]
            w3_cr = d["window3_drain_compaction_read_bytes"]
            tot_cr = d["three_window_compaction_read_bytes"]
            
            tot_out = d["three_window_output_bytes"]
            assert tot_out == tot_f + tot_cw
            
            # Mathematical identities
            assert w1_f == b1["cumulative_flush_bytes"] - b0["cumulative_flush_bytes"]
            assert w2_f == b2["cumulative_flush_bytes"] - b1["cumulative_flush_bytes"]
            assert w3_f == b3["cumulative_flush_bytes"] - b2["cumulative_flush_bytes"]
            assert tot_f == b3["cumulative_flush_bytes"] - b0["cumulative_flush_bytes"]
            assert tot_f == w1_f + w2_f + w3_f
            
            assert w1_cw == b1["cumulative_compaction_write_bytes"] - b0["cumulative_compaction_write_bytes"]
            assert w2_cw == b2["cumulative_compaction_write_bytes"] - b1["cumulative_compaction_write_bytes"]
            assert w3_cw == b3["cumulative_compaction_write_bytes"] - b2["cumulative_compaction_write_bytes"]
            assert tot_cw == b3["cumulative_compaction_write_bytes"] - b0["cumulative_compaction_write_bytes"]
            assert tot_cw == w1_cw + w2_cw + w3_cw
            
            assert w1_cr == b1["cumulative_compaction_read_bytes"] - b0["cumulative_compaction_read_bytes"]
            assert w2_cr == b2["cumulative_compaction_read_bytes"] - b1["cumulative_compaction_read_bytes"]
            assert w3_cr == b3["cumulative_compaction_read_bytes"] - b2["cumulative_compaction_read_bytes"]
            assert tot_cr == b3["cumulative_compaction_read_bytes"] - b0["cumulative_compaction_read_bytes"]
            assert tot_cr == w1_cr + w2_cr + w3_cr
            
            calc_pwa = tot_out / 15360000.0
            assert abs(calc_pwa - d["pwa_total"]) < 1e-5
            
            # Stable checks
            assert b2["running_flushes"] == 0
            assert b2["running_compactions"] == 0
            assert b3["running_flushes"] == 0
            assert b3["running_compactions"] == 0
            assert b3["pending_compaction_bytes"] == 0
            
            rows.append({
                "config_name": cfg_name,
                "rep": rep,
                "w1_flush_bytes": w1_f,
                "w2_flush_bytes": w2_f,
                "w3_flush_bytes": w3_f,
                "total_flush_bytes": tot_f,
                "w1_compaction_read_bytes": w1_cr,
                "w2_compaction_read_bytes": w2_cr,
                "w3_compaction_read_bytes": w3_cr,
                "total_compaction_read_bytes": tot_cr,
                "w1_compaction_write_bytes": w1_cw,
                "w2_compaction_write_bytes": w2_cw,
                "w3_compaction_write_bytes": w3_cw,
                "total_compaction_write_bytes": tot_cw,
                "total_engine_output_bytes": tot_out,
                "put_value_bytes": 15360000,
                "pwa_fg": f"{d['pwa_fg']:.5f}",
                "pwa_cooldown": f"{d['pwa_cooldown']:.5f}",
                "pwa_drain": f"{d['pwa_drain']:.5f}",
                "pwa_total": f"{d['pwa_total']:.5f}",
                "identity_check_passed": True,
                "b0_l0_files": b0["l0_files"],
                "b1_l0_files": b1["l0_files"],
                "b2_l0_files": b2["l0_files"],
                "b3_l0_files": b3["l0_files"],
                "b0_pending_compaction_bytes": b0["pending_compaction_bytes"],
                "b1_pending_compaction_bytes": b1["pending_compaction_bytes"],
                "b2_pending_compaction_bytes": b2["pending_compaction_bytes"],
                "b3_pending_compaction_bytes": b3["pending_compaction_bytes"],
                "b2_running_flushes": b2["running_flushes"],
                "b2_running_compactions": b2["running_compactions"],
                "b3_running_flushes": b3["running_flushes"],
                "b3_running_compactions": b3["running_compactions"],
                "post_measurement_teardown_flush_bytes": d["post_measurement_teardown_flush_bytes"],
                "post_measurement_teardown_compaction_write_bytes": d["post_measurement_teardown_compaction_write_bytes"],
                "post_measurement_teardown_output": d["post_measurement_teardown_output"]
            })
            print(f"[{cfg_name} Rep {rep}] Total Flush={tot_f} B, Compaction Write={tot_cw} B, Output={tot_out} B, PWA={d['pwa_total']:.4f}x [PASS]")

    out_csv = os.path.join(OUTPUT_AUDIT_DIR, "r0_three_window_io_reconciliation.csv")
    with open(out_csv, "w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"Saved: {out_csv}")

if __name__ == "__main__":
    parse_three_window_io()
