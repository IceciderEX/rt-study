#!/usr/bin/env python3
import json
import glob
import os
import numpy as np

STUDY_ROOT = "/home/wam/grad/s14-range-delete-study"
AUDIT_DIR = os.path.join(STUDY_ROOT, "results/amtv_m2d/audit")
CONFIGS = ["Native-T0", "Native-T512", "AMTV-T0", "AMTV-T512"]

def load_data():
    by_cfg = {c: [] for c in CONFIGS}
    for f in sorted(glob.glob(os.path.join(AUDIT_DIR, "*.json"))):
        with open(f) as fp:
            d = json.load(fp)
        c = d["config_name"]
        if c in by_cfg:
            by_cfg[c].append(d)
    return by_cfg

def print_section(title):
    print("\n" + "=" * 80)
    print(title)
    print("=" * 80)

def main():
    by_cfg = load_data()

    print_section("TABLE 1: PHASE RUNTIMES & FOREGROUND THROUGHPUT (MEAN ± STD, N=3)")
    print(f"{'Config':<12} | {'Phase A (s)':<14} | {'Phase B (s)':<14} | {'Phase C (s)':<14} | {'Total FG (s)':<14} | {'FG IOPS':<14}")
    print("-" * 90)
    for c in CONFIGS:
        runs = by_cfg[c]
        pa = [r["phase_a_sec"] for r in runs]
        pb = [r["phase_b_sec"] for r in runs]
        pc = [r["phase_c_sec"] for r in runs]
        pt = [r["fg_elapsed_sec"] for r in runs]
        iops = [r["fg_iops"] for r in runs]
        print(f"{c:<12} | {np.mean(pa):.4f}±{np.std(pa, ddof=1):.4f} | {np.mean(pb):.4f}±{np.std(pb, ddof=1):.4f} | {np.mean(pc):.4f}±{np.std(pc, ddof=1):.4f} | {np.mean(pt):.4f}±{np.std(pt, ddof=1):.4f} | {np.mean(iops):.1f}±{np.std(iops, ddof=1):.1f}")

    print_section("TABLE 2: GETLIVE LATENCY PROFILES (US) (MEAN ± STD, N=3)")
    print(f"{'Config':<12} | {'P50 (us)':<12} | {'P95 (us)':<14} | {'P99 (us)':<14} | {'P99.9 (us)':<14} | {'Max (us)':<14}")
    print("-" * 90)
    for c in CONFIGS:
        runs = by_cfg[c]
        p50 = [r["get_live_p50_us"] for r in runs]
        p95 = [r["get_live_p95_us"] for r in runs]
        p99 = [r["get_live_p99_us"] for r in runs]
        p999 = [r["get_live_p999_us"] for r in runs]
        pmax = [r["get_live_max_us"] for r in runs]
        print(f"{c:<12} | {np.mean(p50):.2f}±{np.std(p50, ddof=1):.2f}  | {np.mean(p95):.2f}±{np.std(p95, ddof=1):.2f}   | {np.mean(p99):.2f}±{np.std(p99, ddof=1):.2f}   | {np.mean(p999):.2f}±{np.std(p999, ddof=1):.2f}   | {np.mean(pmax):.1f}±{np.std(pmax, ddof=1):.1f}")

    print_section("TABLE 3: NATIVE READ-PATH AUDIT METRICS (MATERIALIZATION & LOCK CONTENTION)")
    print(f"{'Config':<12} | {'Mat Count':<12} | {'Mat Time (s)':<14} | {'Cache Inval':<12} | {'Lock Contended':<16} | {'Lock Wait (s)':<14}")
    print("-" * 90)
    for c in CONFIGS:
        runs = by_cfg[c]
        cnt = [r.get("audit_mat_count", 0) for r in runs]
        t_s = [r.get("audit_mat_nanos", 0) / 1e9 for r in runs]
        cinv = [r.get("audit_cache_inv_count", 0) for r in runs]
        latt = [r.get("audit_lock_attempt_count", 0) for r in runs]
        lcont = [r.get("audit_lock_contended_count", 0) for r in runs]
        lwait_s = [r.get("audit_lock_wait_nanos", 0) / 1e9 for r in runs]
        cont_rate = (np.mean(lcont) / np.mean(latt) * 100) if np.mean(latt) > 0 else 0.0
        print(f"{c:<12} | {np.mean(cnt):<12.1f} | {np.mean(t_s):.2f}±{np.std(t_s, ddof=1):.2f} s   | {np.mean(cinv):<12.0f} | {np.mean(lcont):.0f} ({cont_rate:.1f}%) | {np.mean(lwait_s):.2f}±{np.std(lwait_s, ddof=1):.2f} s")

    print_section("TABLE 4: AMTV EXCLUSIVE WRITE TIMERS (FOR 20,000 DELETERANGE OPERATIONS)")
    print(f"{'Metric':<30} | {'AMTV-T0 Total':<16} | {'AMTV-T0 Per-Op':<16} | {'AMTV-T512 Total':<16} | {'AMTV-T512 Per-Op':<16}")
    print("-" * 100)
    write_keys = [
        ("amtv_write_state_lock_wait_nanos", "1. write_state_lock_wait"),
        ("amtv_write_append_nanos", "2. write_append"),
        ("amtv_write_snapshot_clone_nanos", "3. write_snapshot_clone"),
        ("amtv_write_seal_build_nanos", "4. write_seal_build"),
        ("amtv_write_publish_nanos", "5. write_publish"),
    ]
    for key, label in write_keys:
        t0_vals = [r.get(key, 0) / 1e6 for r in by_cfg["AMTV-T0"]]  # ms
        t512_vals = [r.get(key, 0) / 1e6 for r in by_cfg["AMTV-T512"]]
        t0_mean = np.mean(t0_vals)
        t512_mean = np.mean(t512_vals)
        t0_op = t0_mean / 20000.0 * 1000.0  # us
        t512_op = t512_mean / 20000.0 * 1000.0
        print(f"{label:<30} | {t0_mean:.2f} ms         | {t0_op:.3f} us        | {t512_mean:.2f} ms         | {t512_op:.3f} us")

    print_section("TABLE 5: AMTV PROBING DEPTH & OPEN DELTA SIZES (GET PROBING)")
    print(f"{'Config':<12} | {'Avg Probed Runs':<16} | {'Max Probed Runs':<16} | {'Avg Open Delta':<16} | {'Max Open Delta':<16} | {'Fallback':<10}")
    print("-" * 90)
    for c in ["AMTV-T0", "AMTV-T512"]:
        runs = by_cfg[c]
        p_runs_avg = [r.get("get_probe_avg_sealed_runs", 0) for r in runs]
        p_runs_max = [r.get("get_probe_max_sealed_runs", 0) for r in runs]
        p_od_avg = [r.get("get_probe_avg_open_delta", 0) for r in runs]
        p_od_max = [r.get("get_probe_max_open_delta", 0) for r in runs]
        fb = [r.get("amtv_fallback_events", 0) for r in runs]
        print(f"{c:<12} | {np.mean(p_runs_avg):.3f}±{np.std(p_runs_avg, ddof=1):.3f}      | {np.mean(p_runs_max):.1f}±{np.std(p_runs_max, ddof=1):.1f}           | {np.mean(p_od_avg):.2f}±{np.std(p_od_avg, ddof=1):.2f}        | {np.mean(p_od_max):.0f}               | {np.mean(fb):.0f}")

    print_section("TABLE 6: AMTV BACKGROUND MERGE & RESOURCE CONSUMPTION")
    print(f"{'Metric':<35} | {'AMTV-T0':<20} | {'AMTV-T512':<20}")
    print("-" * 80)
    merge_keys = [
        ("amtv_merge_computed", "Computed Merges", ""),
        ("amtv_merge_published", "Published Merges", ""),
        ("amtv_merge_discarded", "Discarded Merges", ""),
        ("amtv_merge_cpu_time_us", "Merge CPU Time", "us"),
        ("amtv_merge_wall_time_us", "Merge Wall Time", "us"),
        ("amtv_merge_input_runs", "Input Runs Merged", ""),
        ("amtv_merge_input_tombstones", "Input Tombstones Merged", ""),
        ("amtv_reconstruction_amplification", "Reconstruction Amplification", "x"),
        ("peak_signed_backlog", "Peak Signed Backlog", "runs"),
        ("peak_backlog_excess", "Peak Backlog Excess", "runs"),
        ("peak_actual_sealed_runs", "Peak Actual Sealed Runs", "runs"),
        ("amtv_raw_entries_struct_bytes_peak", "Peak Raw Entries (bytes)", "B"),
        ("amtv_in_flight_merge_struct_bytes_peak", "Peak In-Flight Merge (bytes)", "B"),
    ]
    for key, label, unit in merge_keys:
        t0_vals = [float(r.get(key, 0)) for r in by_cfg["AMTV-T0"]]
        t512_vals = [float(r.get(key, 0)) for r in by_cfg["AMTV-T512"]]
        print(f"{label:<35} | {np.mean(t0_vals):.2f} {unit:<5} (±{np.std(t0_vals, ddof=1):.2f}) | {np.mean(t512_vals):.2f} {unit:<5} (±{np.std(t512_vals, ddof=1):.2f})")

    print_section("TABLE 7: ENGINE WRITE AMPLIFICATION & FLUSH METRICS")
    print(f"{'Config':<12} | {'Capacity Flush':<15} | {'Threshold Flush':<16} | {'Flush Bytes (MB)':<18} | {'Compaction Bytes (MB)':<22} | {'Engine Output WA':<16}")
    print("-" * 105)
    for c in CONFIGS:
        runs = by_cfg[c]
        cf = [r.get("fg_capacity_flushes", 0) for r in runs]
        tf = [r.get("fg_threshold_flushes", 0) for r in runs]
        fb = [r.get("fg_flush_bytes", 0) / (1024*1024) for r in runs]
        cb = [r.get("fg_compaction_write_bytes", 0) / (1024*1024) for r in runs]
        wa = [r.get("engine_output_wa", 0) for r in runs]
        print(f"{c:<12} | {np.mean(cf):<15.0f} | {np.mean(tf):<16.0f} | {np.mean(fb):.2f}±{np.std(fb, ddof=1):.2f} MB       | {np.mean(cb):.2f}±{np.std(cb, ddof=1):.2f} MB          | {np.mean(wa):.3f}±{np.std(wa, ddof=1):.3f}x")

    print_section("TABLE 8: VERIFICATION OF STATE CONSERVATION & BIT-FOR-BIT RECONCILIATION")
    for rep in [1, 2, 3]:
        print(f"\n--- Repetition {rep} ---")
        for c in CONFIGS:
            r = [x for x in by_cfg[c] if x["rep"] == rep][0]
            print(f"  [{c} Rep {rep}] DB SHA: {r['db_sha256'][:16]}... | Model SHA: {r['expected_model_sha'][:16]}... | Match: {r['db_sha256'] == r['expected_model_sha']} | Drained Runs: {r.get('drained_sealed_runs', 'N/A')}, Delta: {r.get('drained_open_delta_len', 'N/A')}, DistMatched: {r.get('theoretical_distribution_matched', 'N/A')}")

if __name__ == "__main__":
    main()
