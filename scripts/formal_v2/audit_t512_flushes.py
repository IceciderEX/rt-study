#!/usr/bin/env python3
import json
import re
import os

def audit_t512(rep):
    exp_id = f"e9_t512_r{rep}"
    log_file = f"run-db/e9_dynamic_audit/{exp_id}/db/LOG"
    driver_log = f"results/summary/e9_dynamic_audit/matrix/{exp_id}/driver.log"
    events_csv = f"results/summary/e9_dynamic_audit/matrix/{exp_id}/events.csv"
    
    p_dur = []
    with open(driver_log) as f:
        for line in f:
            m = re.search(r"Phase (\d+) completed in ([0-9\.]+) s", line)
            if m:
                p_dur.append((int(m.group(1)), float(m.group(2))))
    
    t_p0_end = p_dur[0][1]
    t_p1_end = t_p0_end + p_dur[1][1]
    t_p2_end = t_p1_end + p_dur[2][1]
    
    print(f"\n=======================================================")
    print(f"Audit Native-T512 Repetition {rep} ({exp_id})")
    print(f"=======================================================")
    print(f"Phase Windows (Relative to Foreground T0):")
    print(f"  Phase A (Read Sensitive): [0.0000 s, {t_p0_end:.4f} s] (duration: {p_dur[0][1]:.4f} s)")
    print(f"  Phase B (Write Burst):   [{t_p0_end:.4f} s, {t_p1_end:.4f} s] (duration: {p_dur[1][1]:.4f} s)")
    print(f"  Phase C (Read Recovery):  [{t_p1_end:.4f} s, {t_p2_end:.4f} s] (duration: {p_dur[2][1]:.4f} s)")
    
    # Parse LOG file for all flush events and memtable switches
    # Extract wallclock times of events
    # Find T0 wallclock: first foreground event or start
    jobs = {}
    with open(log_file) as f:
        for line in f:
            m_flushing = re.search(r"(\d{2}:\d{2}:\d{2}\.\d{6}).*\[JOB (\d+)\] Flushing memtable id (\d+)", line)
            if m_flushing:
                ts, job_id, mem_id = m_flushing.groups()
                if job_id not in jobs: jobs[job_id] = {}
                jobs[job_id]["mem_id"] = mem_id
                jobs[job_id]["req_ts"] = ts
            if "EVENT_LOG_v1" in line:
                try:
                    js = json.loads(line[line.find("{"):])
                    job_id = str(js.get("job"))
                    if job_id not in jobs: jobs[job_id] = {}
                    if js.get("event") == "flush_started":
                        jobs[job_id]["start_micros"] = js.get("time_micros")
                        jobs[job_id]["reason"] = js.get("flush_reason")
                        jobs[job_id]["num_range_deletes"] = js.get("num_range_deletes")
                        jobs[job_id]["num_entries"] = js.get("total_num_input_entries")
                    elif js.get("event") == "flush_finished":
                        jobs[job_id]["finish_micros"] = js.get("time_micros")
                        jobs[job_id]["lsm_state"] = js.get("lsm_state")
                except:
                    pass

    # Now read events.csv which has exact relative timestamps recorded by EventListener during foreground run!
    fg_flushes = []
    with open(events_csv) as f:
        for line in f:
            if "FLUSH" in line and "FOREGROUND" in line:
                parts = line.strip().split(",")
                job_id = parts[3]
                rel_t = float(parts[4])
                reason = parts[5].strip("\"")
                sst = parts[7].strip("\"").split("/")[-1]
                fg_flushes.append({
                    "job_id": job_id,
                    "rel_finish_sec": rel_t,
                    "reason": reason,
                    "sst": sst,
                    "mem_id": jobs.get(job_id, {}).get("mem_id", "N/A"),
                    "num_range_deletes": jobs.get(job_id, {}).get("num_range_deletes", "N/A"),
                    "start_micros": jobs.get(job_id, {}).get("start_micros"),
                    "finish_micros": jobs.get(job_id, {}).get("finish_micros"),
                })

    print(f"\nForeground Flush Events Detail ({len(fg_flushes)} total flushes):")
    print(f"{'Job':<5} | {'MemTable Gen':<12} | {'Reason':<28} | {'RangeDels':<9} | {'Finish Rel(s)':<13} | {'Trigger Phase':<14} | {'Finish Phase':<14}")
    print(f"-" * 110)

    phase_trigger_counts = {"Phase A": 0, "Phase B": 0, "Phase C": 0, "Other": 0}
    phase_finish_counts = {"Phase A": 0, "Phase B": 0, "Phase C": 0, "Other": 0}

    # First event calculation
    if fg_flushes and fg_flushes[0]["start_micros"] and fg_flushes[0]["finish_micros"]:
        t0_micros = fg_flushes[0]["finish_micros"] - int(fg_flushes[0]["rel_finish_sec"] * 1e6)
    else:
        t0_micros = 0

    for f_ev in fg_flushes:
        # Determine finish phase based on rel_finish_sec
        r_finish = f_ev["rel_finish_sec"]
        if r_finish <= t_p0_end:
            fin_p = "Phase A"
        elif r_finish <= t_p1_end:
            fin_p = "Phase B"
        elif r_finish <= t_p2_end:
            fin_p = "Phase C"
        else:
            fin_p = "Cooldown"
        phase_finish_counts[fin_p] = phase_finish_counts.get(fin_p, 0) + 1

        # Determine trigger / switch phase:
        # In RocksDB with memtable_max_range_deletions, switch happens immediately when 512th DeleteRange is written.
        # Then Flush job is queued and started.
        if t0_micros > 0 and f_ev["start_micros"]:
            rel_start = (f_ev["start_micros"] - t0_micros) / 1e6
        else:
            rel_start = r_finish

        if rel_start <= t_p0_end:
            trig_p = "Phase A"
        elif rel_start <= t_p1_end:
            trig_p = "Phase B"
        elif rel_start <= t_p2_end:
            trig_p = "Phase C"
        else:
            trig_p = "Cooldown"
        phase_trigger_counts[trig_p] = phase_trigger_counts.get(trig_p, 0) + 1

        print(f"{f_ev['job_id']:<5} | Gen {f_ev['mem_id']:<8} | {f_ev['reason']:<28} | {str(f_ev['num_range_deletes']):<9} | {r_finish:<13.4f} | {trig_p:<14} | {fin_p:<14}")

    print(f"\nPhase Flush Summary Breakdown for {exp_id}:")
    print(f"  By Flush Completion Phase: Phase A = {phase_finish_counts.get('Phase A', 0)}, Phase B = {phase_finish_counts.get('Phase B', 0)}, Phase C = {phase_finish_counts.get('Phase C', 0)}")
    print(f"  By Trigger/Switch Phase:   Phase A = {phase_trigger_counts.get('Phase A', 0)}, Phase B = {phase_trigger_counts.get('Phase B', 0)}, Phase C = {phase_trigger_counts.get('Phase C', 0)}")

for r in [1, 2, 3]:
    audit_t512(r)
