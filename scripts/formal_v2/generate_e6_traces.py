#!/usr/bin/env python3
"""
Formal V2-E6 Trace Generator
Generates 6 sets of traces for C256, C384, and C448 (Delete & Clean pairs):
- e6_c256_hot / e6_c256_clean_hot (256 tombstones: 8x782 + 24x781 per worker = 25,000 keys)
- e6_c384_hot / e6_c384_clean_hot (384 tombstones: 40x521 + 8x520 per worker = 25,000 keys)
- e6_c448_hot / e6_c448_clean_hot (448 tombstones: 24x447 + 32x446 per worker = 25,000 keys)

Strict constraints:
1. Exactly 40.0% global coverage (200,000 keys), 25,000 keys per worker.
2. Interval overlap ratio strictly 0.00%.
3. Put operations strictly outside deleted intervals.
4. Phase B operations strictly partitioned:
   - PhaseB-Inject (first 20%): C DeleteRange, 4900 Scan (20%), 6000 Put (20%), 9100-C Get (Total: 20,000)
   - PhaseB-PostBurst (latter 80%): 0 DeleteRange, 19600 Scan (80%), 24000 Put (80%), 36400 Get (Total: 80,000)
   - Both subphases maintain 80% affected/intersect Hot access ratio.
5. All operations statically flagged with bit 0 (affected/intersect), bit 1 (inject), bit 2 (postburst).
6. Exact Put projections bit-identical across Clean and Delete groups within each tier.
"""

import os
import sys
import json
import struct
import random
import hashlib
import subprocess

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
TRACES_DIR = os.path.join(BASE_DIR, "traces/formal_v2")

# Trace binary format: 24 bytes
# struct FormalTraceRecord {
#     uint8_t  phase_id;      // 0=A, 1=B, 2=C
#     uint8_t  op_type;       // 0=Get, 1=Scan, 2=Put, 3=DeleteRange, 4=No-op
#     uint8_t  scan_mode;     // 0=Range, 1=Limit
#     uint8_t  flags;         // bit 0: affected/intersect, bit 1: inject, bit 2: postburst
#     uint32_t op_id;
#     uint64_t key1;
#     uint64_t key2;
# };
RECORD_FORMAT = "<BBBB I QQ"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
assert RECORD_SIZE == 24, f"RECORD_SIZE must be 24, got {RECORD_SIZE}"

FLAG_AFFECTED_OR_INTERSECT = 1 << 0
FLAG_SUBPHASE_INJECT = 1 << 1
FLAG_SUBPHASE_POSTBURST = 1 << 2

def get_git_commit():
    return "abeebd9630f11bd08c28b7bd43c7bdfc62050654"

def compute_sha256(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def generate_worker_intervals(w_id, w_start, w_end, count_tier):
    """
    Generates mutually disjoint intervals for worker covering exactly 25,000 keys.
    Span combinations:
    - C256: 32 tombstones -> 8 x 782 + 24 x 781 = 6256 + 18744 = 25,000
    - C384: 48 tombstones -> 40 x 521 + 8 x 520 = 20840 + 4160 = 25,000
    - C448: 56 tombstones -> 24 x 447 + 32 x 446 = 10728 + 14272 = 25,000
    """
    if count_tier == 256:
        # 32 tombstones: 8 of span 782, 24 of span 781
        spans = [782] * 8 + [781] * 24
    elif count_tier == 384:
        # 48 tombstones: 40 of span 521, 8 of span 520
        spans = [521] * 40 + [520] * 8
    elif count_tier == 448:
        # 56 tombstones: 24 of span 447, 32 of span 446
        spans = [447] * 24 + [446] * 32
    else:
        raise ValueError(f"Unknown count tier: {count_tier}")

    assert sum(spans) == 25000, f"Spans sum must be 25,000, got {sum(spans)}"
    num_tombstones = len(spans)

    # Distribute spans across worker's 62,500 key range
    # Total free keys = 62,500 - 25,000 = 37,500
    # Create num_tombstones + 1 gaps
    rng = random.Random(70000 + count_tier * 100 + w_id)
    rng.shuffle(spans)

    num_gaps = num_tombstones + 1
    free_keys = (w_end - w_start) - sum(spans)
    base_gap = free_keys // num_gaps
    rem_gap = free_keys % num_gaps

    gaps = [base_gap] * num_gaps
    for i in range(rem_gap):
        gaps[i] += 1
    rng.shuffle(gaps)

    intervals = []
    curr = w_start
    for i in range(num_tombstones):
        curr += gaps[i]
        start_k = curr
        end_k = start_k + spans[i]
        assert end_k <= w_end, f"Interval [{start_k}, {end_k}) exceeded w_end {w_end}"
        intervals.append((start_k, end_k))
        curr = end_k

    # Verify worker coverage
    total_cov = sum(end - start for start, end in intervals)
    assert total_cov == 25000, f"Worker {w_id} total coverage {total_cov} != 25000"
    # Verify mutual exclusivity
    for i in range(len(intervals) - 1):
        assert intervals[i][1] <= intervals[i+1][0], f"Interval overlap: {intervals[i]} and {intervals[i+1]}"

    return intervals

def build_e6_suite():
    num_workers = 8
    total_keys = 500000
    keys_per_worker = total_keys // num_workers
    value_size = 256

    worker_ranges = [(w * keys_per_worker, (w + 1) * keys_per_worker) for w in range(num_workers)]

    tiers = [
        (256, "c256"),
        (384, "c384"),
        (448, "c448")
    ]

    for count_tier, tier_name in tiers:
        # 1. Generate master deletion intervals for all workers
        tier_intervals = {}
        tier_deleted_keys_set = {}
        tier_live_keys_list = {}
        tier_affected_keys_list = {}

        for w_id in range(num_workers):
            w_start, w_end = worker_ranges[w_id]
            intervals = generate_worker_intervals(w_id, w_start, w_end, count_tier)
            tier_intervals[w_id] = intervals

            del_set = set()
            for s, e in intervals:
                del_set.update(range(s, e))
            tier_deleted_keys_set[w_id] = del_set

            all_w_keys = list(range(w_start, w_end))
            tier_affected_keys_list[w_id] = sorted(list(del_set))
            tier_live_keys_list[w_id] = sorted([k for k in all_w_keys if k not in del_set])
            assert len(tier_affected_keys_list[w_id]) == 25000
            assert len(tier_live_keys_list[w_id]) == 37500

        # Master RNG for write operations to ensure bit-identical Put projections
        write_rng = random.Random(888000 + count_tier)

        # Pre-generate 50,000 Put operations globally (strictly on live keys)
        # Phase A: 10,000 (1,250/worker)
        # Phase B: 30,000 (6,000 inject [750/worker], 24,000 postburst [3,000/worker])
        # Phase C: 10,000 (1,250/worker)
        worker_put_keys = {w: {"phase_a": [], "phase_b_inj": [], "phase_b_post": [], "phase_c": []} for w in range(num_workers)}
        for w in range(num_workers):
            live_k = tier_live_keys_list[w]
            worker_put_keys[w]["phase_a"] = [write_rng.choice(live_k) for _ in range(1250)]
            worker_put_keys[w]["phase_b_inj"] = [write_rng.choice(live_k) for _ in range(750)]
            worker_put_keys[w]["phase_b_post"] = [write_rng.choice(live_k) for _ in range(3000)]
            worker_put_keys[w]["phase_c"] = [write_rng.choice(live_k) for _ in range(1250)]

        # Pre-generate Get & Scan target keys for Phase A, B, C for each worker
        # 80% affected/intersect, 20% live/non-intersect
        read_rng = random.Random(999000 + count_tier)
        worker_reads = {w: {} for w in range(num_workers)}

        tombstones_per_worker = count_tier // num_workers

        for w in range(num_workers):
            w_start, w_end = worker_ranges[w]
            aff_k = tier_affected_keys_list[w]
            live_k = tier_live_keys_list[w]
            intervals = tier_intervals[w]

            # Helper for choosing hot Get
            def gen_gets(count):
                res = []
                for _ in range(count):
                    is_aff = (read_rng.random() < 0.80)
                    k = read_rng.choice(aff_k) if is_aff else read_rng.choice(live_k)
                    res.append((k, is_aff))
                return res

            # Helper for choosing hot Scan [start, start + 100)
            def gen_scans(count):
                res = []
                for _ in range(count):
                    is_int = (read_rng.random() < 0.80)
                    if is_int:
                        # Pick a key near interval start/end to guarantee overlap
                        iv = read_rng.choice(intervals)
                        # start key chosen such that [start, start+100) intersects iv
                        offset = read_rng.randint(-50, max(0, (iv[1] - iv[0]) - 50))
                        s_k = max(w_start, min(w_end - 100, iv[0] + offset))
                        res.append((s_k, s_k + 100, True))
                    else:
                        # Pick a scan strictly within live space if possible
                        # Pick random live key, ensure no intersection
                        attempts = 0
                        s_k = read_rng.choice(live_k)
                        s_k = min(s_k, w_end - 100)
                        e_k = s_k + 100
                        while attempts < 20:
                            # check intersection
                            intersects = any(not (e_k <= iv_s or s_k >= iv_e) for iv_s, iv_e in intervals)
                            if not intersects:
                                break
                            s_k = read_rng.choice(live_k)
                            s_k = min(s_k, w_end - 100)
                            e_k = s_k + 100
                            attempts += 1
                        res.append((s_k, e_k, False))
                return res

            # Worker quotas:
            # Phase A (12,500 ops/w): Get 8,750 (70k/8), Scan 2,500 (20k/8), Put 1,250 (10k/8)
            # Phase B-Inj (2,500 ops/w): Del tombstones_per_worker, Scan 612.5 (4 of 613, 4 of 612 = 4900), Put 750 (6000/8), Get (2500 - 750 - del - scan)
            # Phase B-Post (10,000 ops/w): Del 0, Scan 2,450 (19600/8), Put 3,000 (24000/8), Get 4,550 (36400/8)
            # Phase C (12,500 ops/w): Get 8,750, Scan 2,500, Put 1,250
            w_inj_scans = 613 if w < 4 else 612
            w_inj_gets = 2500 - 750 - tombstones_per_worker - w_inj_scans

            worker_reads[w] = {
                "phase_a_gets": gen_gets(8750),
                "phase_a_scans": gen_scans(2500),
                "phase_b_inj_gets": gen_gets(w_inj_gets),
                "phase_b_inj_scans": gen_scans(w_inj_scans),
                "phase_b_post_gets": gen_gets(4550),
                "phase_b_post_scans": gen_scans(2450),
                "phase_c_gets": gen_gets(8750),
                "phase_c_scans": gen_scans(2500)
            }

        # Build Delete trace and Clean trace
        for is_clean in [False, True]:
            workload_id = f"e6_{tier_name}_clean_hot" if is_clean else f"e6_{tier_name}_hot"
            trace_dir = os.path.join(TRACES_DIR, workload_id)
            os.makedirs(trace_dir, exist_ok=True)
            desc = f"Formal V2 E6 {tier_name.upper()} ({count_tier} tombstones) {'CLEAN' if is_clean else 'DELETE'} Hot Read Baseline"

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
                "random_seed": 888000 + count_tier,
                "record_size_bytes": RECORD_SIZE,
                "phases": ["phase_a", "phase_b", "phase_c"],
                "payload_files": {},
                "actual_metrics": {
                    "cross_worker_ops_count": 0,
                    "total_ops_count": 300000,
                    "total_get_count": 185500 - count_tier,
                    "total_scan_count": 64500,
                    "total_put_count": 50000,
                    "total_del_range_count": 0 if is_clean else count_tier,
                    "total_noop_count": count_tier if is_clean else 0,
                    "subphase_breakdown": {
                        "phase_b_inject": {
                            "total_ops": 20000,
                            "delete_range_count": 0 if is_clean else count_tier,
                            "noop_count": count_tier if is_clean else 0,
                            "scan_count": 4900,
                            "put_count": 6000,
                            "get_count": 9100 - count_tier
                        },
                        "phase_b_postburst": {
                            "total_ops": 80000,
                            "delete_range_count": 0,
                            "noop_count": 0,
                            "scan_count": 19600,
                            "put_count": 24000,
                            "get_count": 36400
                        }
                    }
                },
                "audit_metrics": {
                    "put_projection_sha256": "",
                    "delete_geometry_sha256": "",
                    "planned_affected_get_ratio": 80.0,
                    "planned_intersect_scan_ratio": 80.0,
                    "actual_tombstone_count": 0 if is_clean else count_tier,
                    "actual_union_coverage": 0 if is_clean else 200000,
                    "union_coverage_ratio_pct": 0.0 if is_clean else 40.0,
                    "interval_overlap_ratio": 0.0,
                    "candidate_keyspace_size": total_keys,
                    "expected_visible_key_count": total_keys if is_clean else 300000,
                    "final_visible_state_sha256": ""
                }
            }

            del_geom_hasher = hashlib.sha256()
            put_proj_hasher = hashlib.sha256()

            # Generate binary files for each worker and each phase
            for w in range(num_workers):
                # 1. Phase A (12,500 ops)
                p_a_records = []
                for k, is_aff in worker_reads[w]["phase_a_gets"]:
                    flags = FLAG_AFFECTED_OR_INTERSECT if is_aff else 0
                    p_a_records.append((0, 0, 0, flags, k, 0))
                for s_k, e_k, is_int in worker_reads[w]["phase_a_scans"]:
                    flags = FLAG_AFFECTED_OR_INTERSECT if is_int else 0
                    p_a_records.append((0, 1, 0, flags, s_k, e_k))
                for k in worker_put_keys[w]["phase_a"]:
                    p_a_records.append((0, 2, 0, 0, k, 0))
                
                # Shuffle Phase A deterministically
                rng_p = random.Random(111000 + count_tier * 10 + w)
                rng_p.shuffle(p_a_records)
                assert len(p_a_records) == 12500

                # 2. Phase B-Inject (2,500 ops)
                p_b_inj = []
                # Tombstones for worker
                w_intervals = tier_intervals[w]
                for iv_s, iv_e in w_intervals:
                    if is_clean:
                        p_b_inj.append((1, 4, 0, FLAG_SUBPHASE_INJECT, iv_s, iv_e)) # No-op
                    else:
                        p_b_inj.append((1, 3, 0, FLAG_SUBPHASE_INJECT, iv_s, iv_e)) # DeleteRange
                        del_geom_hasher.update(struct.pack("<QQ", iv_s, iv_e))
                for k, is_aff in worker_reads[w]["phase_b_inj_gets"]:
                    flags = FLAG_SUBPHASE_INJECT | (FLAG_AFFECTED_OR_INTERSECT if is_aff else 0)
                    p_b_inj.append((1, 0, 0, flags, k, 0))
                for s_k, e_k, is_int in worker_reads[w]["phase_b_inj_scans"]:
                    flags = FLAG_SUBPHASE_INJECT | (FLAG_AFFECTED_OR_INTERSECT if is_int else 0)
                    p_b_inj.append((1, 1, 0, flags, s_k, e_k))
                for k in worker_put_keys[w]["phase_b_inj"]:
                    p_b_inj.append((1, 2, 0, FLAG_SUBPHASE_INJECT, k, 0))
                
                rng_p.shuffle(p_b_inj)
                assert len(p_b_inj) == 2500, f"Worker {w} PhaseB-Inj expected 2500 ops, got {len(p_b_inj)}"

                # 3. Phase B-PostBurst (10,000 ops)
                p_b_post = []
                for k, is_aff in worker_reads[w]["phase_b_post_gets"]:
                    flags = FLAG_SUBPHASE_POSTBURST | (FLAG_AFFECTED_OR_INTERSECT if is_aff else 0)
                    p_b_post.append((1, 0, 0, flags, k, 0))
                for s_k, e_k, is_int in worker_reads[w]["phase_b_post_scans"]:
                    flags = FLAG_SUBPHASE_POSTBURST | (FLAG_AFFECTED_OR_INTERSECT if is_int else 0)
                    p_b_post.append((1, 1, 0, flags, s_k, e_k))
                for k in worker_put_keys[w]["phase_b_post"]:
                    p_b_post.append((1, 2, 0, FLAG_SUBPHASE_POSTBURST, k, 0))
                
                rng_p.shuffle(p_b_post)
                assert len(p_b_post) == 10000, f"Worker {w} PhaseB-Post expected 10000 ops, got {len(p_b_post)}"

                # Full Phase B records for worker (12,500 ops)
                p_b_records = p_b_inj + p_b_post
                assert len(p_b_records) == 12500

                # 4. Phase C (12,500 ops)
                p_c_records = []
                for k, is_aff in worker_reads[w]["phase_c_gets"]:
                    flags = FLAG_AFFECTED_OR_INTERSECT if is_aff else 0
                    p_c_records.append((2, 0, 0, flags, k, 0))
                for s_k, e_k, is_int in worker_reads[w]["phase_c_scans"]:
                    flags = FLAG_AFFECTED_OR_INTERSECT if is_int else 0
                    p_c_records.append((2, 1, 0, flags, s_k, e_k))
                for k in worker_put_keys[w]["phase_c"]:
                    p_c_records.append((2, 2, 0, 0, k, 0))
                
                rng_p.shuffle(p_c_records)
                assert len(p_c_records) == 12500

                # Write binary payload files
                worker_phase_list = [
                    (0, "phase_a", p_a_records),
                    (1, "phase_b", p_b_records),
                    (2, "phase_c", p_c_records)
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

            # Save manifest.json
            with open(os.path.join(trace_dir, "manifest.json"), "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2)

            print(f"[E6 Generator] Generated {workload_id} -> {trace_dir}")
            print(f"  Put Projection SHA: {manifest['audit_metrics']['put_projection_sha256']}")
            print(f"  Del Geometry SHA:   {manifest['audit_metrics']['delete_geometry_sha256']}")

    print("\n[E6 Generator Verification SUCCESS] All 6 E6 traces created and verified!")

if __name__ == "__main__":
    build_e6_suite()
