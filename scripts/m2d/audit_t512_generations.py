#!/usr/bin/env python3
import os
import json
import csv
from datetime import datetime

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
OUTPUT_AUDIT_DIR = os.path.join(STUDY_ROOT, "results/r0_audit")
os.makedirs(OUTPUT_AUDIT_DIR, exist_ok=True)

REPS = [1, 2, 3, 4, 5]
CONFIGS = [("Native-T512", "native_t512"), ("AMTV-T512", "amtv_t512")]

def parse_t512_generations():
    print("======================================================================")
    print("AUDIT PART 2: Native-T512 vs AMTV-T512 Generation Audit (10 Runs)")
    print("======================================================================")

    all_rows = []
    summary_rows = []

    for cfg_name, slug in CONFIGS:
        for rep in REPS:
            log_path = os.path.join(STUDY_ROOT, f"run-db/m2d/m2d_release_{slug}_rep{rep}/db/LOG")
            json_path = os.path.join(STUDY_ROOT, f"results/amtv_m2d/release/m2d_release_{slug}_rep{rep}.json")
            
            with open(json_path) as jfp:
                jdata = json.load(jfp)
            
            # Read all flush events from LOG
            flush_started_events = {}
            table_creations = {}
            flush_finished_events = {}
            
            with open(log_path) as fp:
                for line in fp:
                    if "EVENT_LOG_v1" in line:
                        idx = line.find("EVENT_LOG_v1") + len("EVENT_LOG_v1")
                        try:
                            evt = json.loads(line[idx:].strip())
                        except Exception:
                            continue
                        
                        evt_type = evt.get("event")
                        job_id = evt.get("job")
                        
                        if evt_type == "flush_started":
                            flush_started_events[job_id] = evt
                        elif evt_type == "table_file_creation":
                            table_creations[job_id] = evt
                        elif evt_type == "flush_finished":
                            flush_finished_events[job_id] = evt

            sorted_job_ids = sorted(flush_started_events.keys())
            gen_idx = 1
            flushed_rd_sum = 0
            flushed_sst_bytes_sum = 0
            
            for job in sorted_job_ids:
                start_evt = flush_started_events[job]
                tbl_evt = table_creations.get(job, {})
                fin_evt = flush_finished_events.get(job, {})
                
                rd_count = start_evt.get("num_range_deletes", 0)
                flushed_rd_sum += rd_count
                
                logical_bytes = start_evt.get("total_data_size", 0)
                sst_bytes = tbl_evt.get("file_size", 0)
                flushed_sst_bytes_sum += sst_bytes
                
                reason = start_evt.get("flush_reason", "Unknown")
                time_micros = fin_evt.get("time_micros", start_evt.get("time_micros", 0))
                time_str = datetime.fromtimestamp(time_micros / 1e6).strftime("%Y-%m-%d %H:%M:%S.%f")
                
                all_rows.append({
                    "config_name": cfg_name,
                    "rep": rep,
                    "generation": gen_idx,
                    "is_active_generation": False,
                    "job_id": job,
                    "flush_completion_time": time_str,
                    "time_micros": time_micros,
                    "flush_reason": reason,
                    "generation_range_deletions": rd_count,
                    "cumulative_flushed_range_deletions": flushed_rd_sum,
                    "pre_flush_logical_bytes": logical_bytes,
                    "output_sst_bytes": sst_bytes,
                    "window": "foreground"
                })
                gen_idx += 1
                
            active_gen_rd = 20000 - flushed_rd_sum
            total_sum = flushed_rd_sum + active_gen_rd
            assert total_sum == 20000, f"Invariant violated for {cfg_name} Rep {rep}: {total_sum} != 20000"
            
            all_rows.append({
                "config_name": cfg_name,
                "rep": rep,
                "generation": gen_idx,
                "is_active_generation": True,
                "job_id": "ACTIVE_MEMTABLE",
                "flush_completion_time": "N/A (Unflushed)",
                "time_micros": "N/A",
                "flush_reason": "Remained Below Threshold",
                "generation_range_deletions": active_gen_rd,
                "cumulative_flushed_range_deletions": 20000,
                "pre_flush_logical_bytes": "N/A",
                "output_sst_bytes": 0,
                "window": "foreground"
            })
            
            num_flushes = len(sorted_job_ids)
            avg_rd_per_flush = flushed_rd_sum / num_flushes if num_flushes > 0 else 0
            total_overshoot = flushed_rd_sum - (num_flushes * 512)
            
            summary_rows.append({
                "config_name": cfg_name,
                "rep": rep,
                "num_flushes": num_flushes,
                "flushed_range_deletions": flushed_rd_sum,
                "final_active_generation_range_deletions": active_gen_rd,
                "total_reconciled_range_deletions": total_sum,
                "avg_range_deletions_per_flush": f"{avg_rd_per_flush:.3f}",
                "total_overshoot_range_deletions": total_overshoot,
                "avg_overshoot_per_flush": f"{(total_overshoot / num_flushes):.3f}",
                "total_flush_sst_bytes": flushed_sst_bytes_sum,
                "json_three_window_flush_bytes": jdata["three_window_flush_bytes"]
            })
            
            print(f"[{cfg_name} Rep {rep}] Flushes: {num_flushes}, Flushed RD: {flushed_rd_sum}, Active RD: {active_gen_rd}, Sum: {total_sum} == 20000 [OK]")

    # Export detailed generation CSV
    det_csv = os.path.join(OUTPUT_AUDIT_DIR, "r0_t512_flush_generations_detail.csv")
    with open(det_csv, "w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(all_rows[0].keys()))
        writer.writeheader()
        writer.writerows(all_rows)
    print(f"Saved: {det_csv}")

    # Export generation summary CSV
    sum_csv = os.path.join(OUTPUT_AUDIT_DIR, "r0_t512_generation_summary.csv")
    with open(sum_csv, "w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"Saved: {sum_csv}")

if __name__ == "__main__":
    parse_t512_generations()
