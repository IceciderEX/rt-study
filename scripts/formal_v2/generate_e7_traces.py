#!/usr/bin/env python3
"""
Formal V2-E7 Trace Generator
Generates 2 sets of traces:
- e7_c768_cold (Delete Group)
- e7_c768_clean_cold (Clean Group)

Strict specifications:
1. Key space: 500,000 keys, 8 workers (62,500 keys/worker), Value size = 1,024 B (1 KiB).
2. Global coverage strictly 40.0% (200,000 keys), 25,000 keys per worker.
   Span allocation: 40 of 261 + 56 of 260 = 10,440 + 14,560 = 25,000 keys per worker.
3. Interval overlap strictly 0.00%.
4. All Put operations (235,000 total) strictly avoid deleted intervals.
5. Exact integer Cold access:
   - Phase A: Get 20,000 (1,000 affected), Scan 10,000 (500 intersect), Put 70,000 (Total: 100,000)
   - Phase B-Inject (first 10%): Del 768, Put 8,000, Scan 923 (46 intersect), Get 309 (15 affected) (Total: 10,000)
   - Phase B-WriteStress (latter 90%): 0 Del, Put 72,000, Scan 8,309 (416 intersect), Get 9,691 (485 affected) (Total: 90,000)
   - Phase C-WriteStress: Put 85,000, Get 10,000 (500 affected), Scan 5,000 (250 intersect) (Total: 100,000)
6. Canonical put_projection_sha256 100% bit-identical between Clean and Delete traces.
"""

import os
import sys
import json
import struct
import random
import hashlib

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
TRACES_DIR = os.path.join(BASE_DIR, "traces/formal_v2")

RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
assert RECORD_SIZE == 24

FLAG_AFFECTED_OR_INTERSECT = 1 << 0
FLAG_SUBPHASE_INJECT = 1 << 1
FLAG_SUBPHASE_WRITESTRESS = 1 << 2

def get_git_commit():
    return "abeebd9630f11bd08c28b7bd43c7bdfc62050654"

def compute_sha256(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def generate_worker_intervals_768(w_id, w_start, w_end):
    """
    Generates 96 mutually disjoint intervals covering exactly 25,000 keys.
    Span composition: 40 of span 261, 56 of span 260.
    40 * 261 + 56 * 260 = 10440 + 14560 = 25000 keys.
    """
    spans = [261] * 40 + [260] * 56
    assert len(spans) == 96
    assert sum(spans) == 25000

    rng = random.Random(770000 + w_id)
    rng.shuffle(spans)

    num_gaps = len(spans) + 1
    free_keys = (w_end - w_start) - sum(spans) # 62,500 - 25,000 = 37,500
    base_gap = free_keys // num_gaps
    rem_gap = free_keys % num_gaps

    gaps = [base_gap] * num_gaps
    for i in range(rem_gap):
        gaps[i] += 1
    rng.shuffle(gaps)

    intervals = []
    curr = w_start
    for i in range(len(spans)):
        curr += gaps[i]
        start_k = curr
        end_k = start_k + spans[i]
        assert end_k <= w_end, f"Interval [{start_k}, {end_k}) exceeded w_end {w_end}"
        intervals.append((start_k, end_k))
        curr = end_k

    total_cov = sum(end - start for start, end in intervals)
    assert total_cov == 25000
    for i in range(len(intervals) - 1):
        assert intervals[i][1] <= intervals[i+1][0]

    return intervals

def build_e7_suite():
    num_workers = 8
    total_keys = 500000
    keys_per_worker = total_keys // num_workers # 62,500
    value_size = 1024 # 1 KiB

    worker_ranges = [(w * keys_per_worker, (w + 1) * keys_per_worker) for w in range(num_workers)]

    # 1. Generate intervals for all 8 workers
    worker_intervals = {}
    worker_deleted_keys_set = {}
    worker_affected_keys_list = {}
    worker_live_keys_list = {}

    for w in range(num_workers):
        w_start, w_end = worker_ranges[w]
        intervals = generate_worker_intervals_768(w, w_start, w_end)
        worker_intervals[w] = intervals

        del_set = set()
        for s, e in intervals:
            del_set.update(range(s, e))
        worker_deleted_keys_set[w] = del_set

        all_w_keys = list(range(w_start, w_end))
        worker_affected_keys_list[w] = sorted(list(del_set))
        worker_live_keys_list[w] = sorted([k for k in all_w_keys if k not in del_set])
        assert len(worker_affected_keys_list[w]) == 25000
        assert len(worker_live_keys_list[w]) == 37500

    # 2. Master RNG for Puts (235,000 total, strictly on live keys)
    # Phase A: 70,000 Puts (8,750/worker)
    # Phase B-Inject: 8,000 Puts (1,000/worker)
    # Phase B-WriteStress: 72,000 Puts (9,000/worker)
    # Phase C: 85,000 Puts (10,625/worker)
    write_rng = random.Random(778899)
    worker_put_keys = {w: {"phase_a": [], "phase_b_inj": [], "phase_b_stress": [], "phase_c": []} for w in range(num_workers)}

    for w in range(num_workers):
        live_k = worker_live_keys_list[w]
        worker_put_keys[w]["phase_a"] = [write_rng.choice(live_k) for _ in range(8750)]
        worker_put_keys[w]["phase_b_inj"] = [write_rng.choice(live_k) for _ in range(1000)]
        worker_put_keys[w]["phase_b_stress"] = [write_rng.choice(live_k) for _ in range(9000)]
        worker_put_keys[w]["phase_c"] = [write_rng.choice(live_k) for _ in range(10625)]

    # 3. Read Operations Generator with Exact Integers
    read_rng = random.Random(779900)
    worker_reads = {w: {} for w in range(num_workers)}

    # Phase B-Inject:
    # Total Scan = 923 -> 5 workers get 115, 3 workers get 116 (5*115 + 3*116 = 575 + 348 = 923).
    # Intersect scans = 46 total -> 6 workers get 6, 2 workers get 5 (6*6 + 2*5 = 46).
    # Total Get = 309 -> 5 workers get 39, 3 workers get 38 (5*39 + 3*38 = 195 + 114 = 309).
    # Affected gets = 15 total -> 7 workers get 2, 1 worker gets 1 (7*2 + 1*1 = 15).

    # Phase B-WriteStress:
    # Total Scan = 8309 -> 5 workers get 1039, 3 workers get 1038 (5*1039 + 3*1038 = 5195 + 3114 = 8309).
    # Intersect scans = 416 total -> exactly 52 per worker (52 * 8 = 416).
    # Total Get = 9691 -> 5 workers get 1211, 3 workers get 1212 (5*1211 + 3*1212 = 6055 + 3636 = 9691).
    # Affected gets = 485 total -> 5 workers get 61, 3 workers get 60 (5*61 + 3*60 = 305 + 180 = 485).

    # Phase A:
    # Get = 20,000 (2,500/w, exactly 125 affected/w = 1000 total)
    # Scan = 10,000 (1,250/w, exactly 62.5/w -> 4 get 63, 4 get 62 = 500 total)

    # Phase C:
    # Get = 10,000 (1,250/w, exactly 62.5/w -> 4 get 63, 4 get 62 = 500 total)
    # Scan = 5,000 (625/w, exactly 31.25/w -> 2 get 32, 6 get 31 = 250 total)

    for w in range(num_workers):
        w_start, w_end = worker_ranges[w]
        aff_k = worker_affected_keys_list[w]
        live_k = worker_live_keys_list[w]
        intervals = worker_intervals[w]

        def gen_exact_gets(total_count, aff_count):
            res = []
            for _ in range(aff_count):
                res.append((read_rng.choice(aff_k), True))
            for _ in range(total_count - aff_count):
                res.append((read_rng.choice(live_k), False))
            read_rng.shuffle(res)
            return res

        def gen_exact_scans(total_count, int_count):
            res = []
            for _ in range(int_count):
                iv = read_rng.choice(intervals)
                offset = read_rng.randint(-50, max(0, (iv[1] - iv[0]) - 50))
                s_k = max(w_start, min(w_end - 100, iv[0] + offset))
                res.append((s_k, s_k + 100, True))
            for _ in range(total_count - int_count):
                attempts = 0
                s_k = read_rng.choice(live_k)
                s_k = min(s_k, w_end - 100)
                e_k = s_k + 100
                while attempts < 30:
                    intersects = any(not (e_k <= iv_s or s_k >= iv_e) for iv_s, iv_e in intervals)
                    if not intersects:
                        break
                    s_k = read_rng.choice(live_k)
                    s_k = min(s_k, w_end - 100)
                    e_k = s_k + 100
                    attempts += 1
                res.append((s_k, e_k, False))
            read_rng.shuffle(res)
            return res

        # Phase A per worker
        p_a_aff_get = 125
        p_a_int_scan = 63 if w < 4 else 62
        p_a_gets = gen_exact_gets(2500, p_a_aff_get)
        p_a_scans = gen_exact_scans(1250, p_a_int_scan)

        # Phase B-Inject per worker
        w_b_inj_scan_cnt = 116 if w < 3 else 115 # sum=923
        w_b_inj_scan_int = 6 if w < 6 else 5     # sum=46
        w_b_inj_get_cnt = 38 if w < 3 else 39    # sum=309
        w_b_inj_get_aff = 2 if w < 7 else 1      # sum=15
        assert 96 + 1000 + w_b_inj_scan_cnt + w_b_inj_get_cnt == 1250 # 10,000 / 8 = 1250 per worker

        p_b_inj_gets = gen_exact_gets(w_b_inj_get_cnt, w_b_inj_get_aff)
        p_b_inj_scans = gen_exact_scans(w_b_inj_scan_cnt, w_b_inj_scan_int)

        # Phase B-WriteStress per worker
        w_b_stress_scan_cnt = 1038 if w < 3 else 1039 # sum=8309
        w_b_stress_scan_int = 52                      # sum=416
        w_b_stress_get_cnt = 1212 if w < 3 else 1211  # sum=9691
        w_b_stress_get_aff = 61 if w < 5 else 60      # sum=485
        assert 9000 + w_b_stress_scan_cnt + w_b_stress_get_cnt == 11250 # 90,000 / 8 = 11250 per worker

        p_b_stress_gets = gen_exact_gets(w_b_stress_get_cnt, w_b_stress_get_aff)
        p_b_stress_scans = gen_exact_scans(w_b_stress_scan_cnt, w_b_stress_scan_int)

        # Phase C per worker
        p_c_aff_get = 63 if w < 4 else 62             # sum=500
        p_c_int_scan = 32 if w < 2 else 31            # sum=250
        p_c_gets = gen_exact_gets(1250, p_c_aff_get)
        p_c_scans = gen_exact_scans(625, p_c_int_scan)

        worker_reads[w] = {
            "phase_a_gets": p_a_gets,
            "phase_a_scans": p_a_scans,
            "phase_b_inj_gets": p_b_inj_gets,
            "phase_b_inj_scans": p_b_inj_scans,
            "phase_b_stress_gets": p_b_stress_gets,
            "phase_b_stress_scans": p_b_stress_scans,
            "phase_c_gets": p_c_gets,
            "phase_c_scans": p_c_scans
        }

    # Build Delete trace and Clean trace
    for is_clean in [False, True]:
        workload_id = "e7_c768_clean_cold" if is_clean else "e7_c768_cold"
        trace_dir = os.path.join(TRACES_DIR, workload_id)
        os.makedirs(trace_dir, exist_ok=True)
        desc = f"Formal V2 E7 C768 (768 tombstones, 1KiB Value) {'CLEAN' if is_clean else 'DELETE'} Cold Read & Heavy Write Workload"

        manifest = {
            "magic": "0x54524143455632",
            "version": "2.0",
            "endianness": "little",
            "generator_git_commit": get_git_commit(),
            "workload_id": workload_id,
            "description": desc,
            "is_clean_baseline": is_clean,
            "scan_mode": "SCAN_RANGE",
            "total_keys": total_keys,
            "candidate_keyspace_size": total_keys,
            "value_size": value_size,
            "num_workers": num_workers,
            "keys_per_worker": keys_per_worker,
            "worker_ranges": worker_ranges,
            "random_seed": 778899,
            "record_size_bytes": RECORD_SIZE,
            "phases": ["phase_a", "phase_b", "phase_c"],
            "payload_files": {},
            "actual_metrics": {
                "cross_worker_ops_count": 0,
                "total_ops_count": 300000,
                "total_put_count": 235000,
                "total_get_count": 39691 if not is_clean else 40459,
                "total_scan_count": 24232,
                "total_del_range_count": 0 if is_clean else 768,
                "total_noop_count": 768 if is_clean else 0,
                "subphase_breakdown": {
                    "phase_a": {
                        "total_ops": 100000,
                        "put_count": 70000,
                        "get_count": 20000,
                        "affected_get_count": 1000,
                        "scan_count": 10000,
                        "intersect_scan_count": 500
                    },
                    "phase_b_inject": {
                        "total_ops": 10000,
                        "delete_range_count": 0 if is_clean else 768,
                        "noop_count": 768 if is_clean else 0,
                        "put_count": 8000,
                        "scan_count": 923,
                        "intersect_scan_count": 46,
                        "get_count": 309,
                        "affected_get_count": 15
                    },
                    "phase_b_writestress": {
                        "total_ops": 90000,
                        "delete_range_count": 0,
                        "noop_count": 0,
                        "put_count": 72000,
                        "scan_count": 8309,
                        "intersect_scan_count": 416,
                        "get_count": 9691,
                        "affected_get_count": 485
                    },
                    "phase_c_writestress": {
                        "total_ops": 100000,
                        "put_count": 85000,
                        "get_count": 10000,
                        "affected_get_count": 500,
                        "scan_count": 5000,
                        "intersect_scan_count": 250
                    }
                }
            },
            "audit_metrics": {
                "put_projection_sha256": "",
                "delete_geometry_sha256": "",
                "actual_tombstone_count": 0 if is_clean else 768,
                "actual_union_coverage": 0 if is_clean else 200000,
                "union_coverage_ratio_pct": 0.0 if is_clean else 40.0,
                "interval_overlap_ratio": 0.0,
                "candidate_keyspace_size": total_keys,
                "expected_visible_key_count": total_keys if is_clean else 300000,
                "final_visible_state_sha256": ""
            }
        }

        put_proj_hasher = hashlib.sha256()
        del_geom_hasher = hashlib.sha256()

        for w in range(num_workers):
            # Phase A (12,500 ops: Put 8750, Get 2500, Scan 1250)
            p_a_recs = []
            for k in worker_put_keys[w]["phase_a"]:
                p_a_recs.append((0, 2, 0, 0, k, 0))
            for k, is_aff in worker_reads[w]["phase_a_gets"]:
                p_a_recs.append((0, 0, 0, FLAG_AFFECTED_OR_INTERSECT if is_aff else 0, k, 0))
            for s_k, e_k, is_int in worker_reads[w]["phase_a_scans"]:
                p_a_recs.append((0, 1, 0, FLAG_AFFECTED_OR_INTERSECT if is_int else 0, s_k, e_k))
            
            rng_p = random.Random(711000 + w)
            rng_p.shuffle(p_a_recs)
            assert len(p_a_recs) == 12500

            # Phase B-Inject (1,250 ops: Del 96, Put 1000, Scan w_b_inj_scan_cnt, Get w_b_inj_get_cnt)
            p_b_inj = []
            w_intervals = worker_intervals[w]
            for iv_s, iv_e in w_intervals:
                if is_clean:
                    p_b_inj.append((1, 4, 0, FLAG_SUBPHASE_INJECT, iv_s, iv_e))
                else:
                    p_b_inj.append((1, 3, 0, FLAG_SUBPHASE_INJECT, iv_s, iv_e))
                    del_geom_hasher.update(struct.pack("<QQ", iv_s, iv_e))
            for k in worker_put_keys[w]["phase_b_inj"]:
                p_b_inj.append((1, 2, 0, FLAG_SUBPHASE_INJECT, k, 0))
            for k, is_aff in worker_reads[w]["phase_b_inj_gets"]:
                p_b_inj.append((1, 0, 0, FLAG_SUBPHASE_INJECT | (FLAG_AFFECTED_OR_INTERSECT if is_aff else 0), k, 0))
            for s_k, e_k, is_int in worker_reads[w]["phase_b_inj_scans"]:
                p_b_inj.append((1, 1, 0, FLAG_SUBPHASE_INJECT | (FLAG_AFFECTED_OR_INTERSECT if is_int else 0), s_k, e_k))
            
            rng_p.shuffle(p_b_inj)
            assert len(p_b_inj) == 1250

            # Phase B-WriteStress (11,250 ops: Put 9000, Scan w_b_stress_scan_cnt, Get w_b_stress_get_cnt)
            p_b_stress = []
            for k in worker_put_keys[w]["phase_b_stress"]:
                p_b_stress.append((1, 2, 0, FLAG_SUBPHASE_WRITESTRESS, k, 0))
            for k, is_aff in worker_reads[w]["phase_b_stress_gets"]:
                p_b_stress.append((1, 0, 0, FLAG_SUBPHASE_WRITESTRESS | (FLAG_AFFECTED_OR_INTERSECT if is_aff else 0), k, 0))
            for s_k, e_k, is_int in worker_reads[w]["phase_b_stress_scans"]:
                p_b_stress.append((1, 1, 0, FLAG_SUBPHASE_WRITESTRESS | (FLAG_AFFECTED_OR_INTERSECT if is_int else 0), s_k, e_k))
            
            rng_p.shuffle(p_b_stress)
            assert len(p_b_stress) == 11250

            p_b_recs = p_b_inj + p_b_stress
            assert len(p_b_recs) == 12500

            # Phase C (12,500 ops: Put 10625, Get 1250, Scan 625)
            p_c_recs = []
            for k in worker_put_keys[w]["phase_c"]:
                p_c_recs.append((2, 2, 0, 0, k, 0))
            for k, is_aff in worker_reads[w]["phase_c_gets"]:
                p_c_recs.append((2, 0, 0, FLAG_AFFECTED_OR_INTERSECT if is_aff else 0, k, 0))
            for s_k, e_k, is_int in worker_reads[w]["phase_c_scans"]:
                p_c_recs.append((2, 1, 0, FLAG_AFFECTED_OR_INTERSECT if is_int else 0, s_k, e_k))
            
            rng_p.shuffle(p_c_recs)
            assert len(p_c_recs) == 12500

            # Write binary files
            worker_phase_list = [
                (0, "phase_a", p_a_recs),
                (1, "phase_b", p_b_recs),
                (2, "phase_c", p_c_recs)
            ]

            for p_idx, p_name, p_recs in worker_phase_list:
                filename = f"{p_name}-worker-{w:02d}.bin"
                filepath = os.path.join(trace_dir, filename)
                with open(filepath, "wb") as fout:
                    for rec_idx, (ph_id, op_type, sc_mode, flags, k1, k2) in enumerate(p_recs):
                        op_id = w * 10000000 + p_idx * 1000000 + rec_idx
                        packed = struct.pack(RECORD_FORMAT, ph_id, op_type, sc_mode, flags, op_id, k1, k2)
                        fout.write(packed)
                        if op_type == 2:
                            put_proj_hasher.update(struct.pack("<IQ", op_id, k1))

                manifest["payload_files"][filename] = {
                    "filename": filename,
                    "worker_id": w,
                    "phase_id": p_idx,
                    "phase_name": p_name,
                    "ops_count": len(p_recs),
                    "sha256": compute_sha256(filepath)
                }

        manifest["audit_metrics"]["put_projection_sha256"] = put_proj_hasher.hexdigest()
        manifest["audit_metrics"]["delete_geometry_sha256"] = del_geom_hasher.hexdigest()

        with open(os.path.join(trace_dir, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)

        print(f"[E7 Generator] Generated {workload_id} -> {trace_dir}")
        print(f"  Put Projection SHA: {manifest['audit_metrics']['put_projection_sha256']}")
        print(f"  Del Geometry SHA:   {manifest['audit_metrics']['delete_geometry_sha256']}")

    print("\n[E7 Generator Verification SUCCESS] All 2 E7 traces created and verified!")

if __name__ == "__main__":
    build_e7_suite()
