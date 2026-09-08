#!/usr/bin/env python3
"""
Deterministic Unit Test for M3b Release Overall Quantile Nearest-Rank Verification
Demonstrates:
1. True Overall nearest-rank calculation requires pooling the raw sample sets:
   idx = ceil(p * N_total) - 1 on the sorted union of samples.
2. Naive secondary arithmetic directly on Phase-level quantiles
   (e.g., weighted average of Phase A, B, C quantiles) is mathematically INVALID
   and produces significant error compared to ground truth.
3. Proves mathematical rigor and defines the benchmark standard.
"""

import math
import numpy as np


def nearest_rank_quantile(sorted_samples, p):
    """
    RocksDB LatencyHistogram nearest-rank quantile algorithm:
    idx = ceil(p * N) - 1, bounded to [0, N - 1].
    """
    n = len(sorted_samples)
    assert n > 0
    idx = math.ceil((p / 100.0) * n) - 1
    if idx < 0:
        idx = 0
    if idx >= n:
        idx = n - 1
    return sorted_samples[idx]


def test_nearest_rank_vs_phase_quantiles():
    print("======================================================================")
    print("Deterministic Unit Test: Overall Nearest-Rank vs Phase-Level Quantiles")
    print("======================================================================")

    np.random.seed(42)

    # Simulate realistic 3-phase latencies (e.g., GetLive in Phase A, B, C)
    # Phase A: 70,000 samples, fast baseline (mean ~7 us, lognormal)
    samples_A = np.random.lognormal(mean=np.log(6.5), sigma=0.4, size=70000)

    # Phase B: 40,000 samples, severe contention in Native-T0 (bimodal: fast + heavy tail ~4000-11000 us)
    fast_B = np.random.lognormal(mean=np.log(15.0), sigma=0.5, size=8000)
    slow_B = np.random.normal(loc=4200.0, scale=800.0, size=32000)
    samples_B = np.concatenate([fast_B, slow_B])
    np.random.shuffle(samples_B)

    # Phase C: 80,000 samples, static read recovery (mean ~8.5 us, lognormal)
    samples_C = np.random.lognormal(mean=np.log(8.0), sigma=0.35, size=80000)

    total_samples = len(samples_A) + len(samples_B) + len(samples_C)
    assert total_samples == 190000

    # Sort each phase individually
    sorted_A = np.sort(samples_A)
    sorted_B = np.sort(samples_B)
    sorted_C = np.sort(samples_C)

    # Compute Phase-level quantiles using exact nearest-rank
    quantiles_to_test = [50.0, 95.0, 99.0, 99.9]
    phase_q_A = {q: nearest_rank_quantile(sorted_A, q) for q in quantiles_to_test}
    phase_q_B = {q: nearest_rank_quantile(sorted_B, q) for q in quantiles_to_test}
    phase_q_C = {q: nearest_rank_quantile(sorted_C, q) for q in quantiles_to_test}

    print("\n1. Phase-Level Nearest-Rank Quantiles:")
    for q in quantiles_to_test:
        print(f"  P{q:<4}: Phase A = {phase_q_A[q]:10.2f} us, Phase B = {phase_q_B[q]:10.2f} us, Phase C = {phase_q_C[q]:10.2f} us")

    # True Ground Truth: Pool all raw samples, sort globally, apply nearest-rank
    pooled_raw = np.concatenate([samples_A, samples_B, samples_C])
    pooled_sorted = np.sort(pooled_raw)
    true_overall = {q: nearest_rank_quantile(pooled_sorted, q) for q in quantiles_to_test}

    # Flawed naive secondary calculation: Sample-weighted average of phase quantiles
    w_A = len(samples_A) / total_samples
    w_B = len(samples_B) / total_samples
    w_C = len(samples_C) / total_samples
    naive_overall = {
        q: (w_A * phase_q_A[q] + w_B * phase_q_B[q] + w_C * phase_q_C[q])
        for q in quantiles_to_test
    }

    print("\n2. Comparison: True Pooled Nearest-Rank vs Naive Phase-Quantile Average:")
    print(f"  {'Quantile':<10} | {'True Overall (Pooled)':<22} | {'Naive Phase Average':<22} | {'Relative Error':<15}")
    print("  " + "-" * 75)
    for q in quantiles_to_test:
        t_val = true_overall[q]
        n_val = naive_overall[q]
        rel_err = abs(n_val - t_val) / t_val * 100.0
        print(f"  P{q:<9} | {t_val:18.2f} us | {n_val:18.2f} us | {rel_err:13.2f} %")

    # Verify that naive calculation fails significantly on bimodal distributions (e.g. P50 relative error > 1000%)
    p50_error = abs(naive_overall[50.0] - true_overall[50.0]) / true_overall[50.0] * 100.0
    print(f"\n3. Mathematical Verification Result:")
    print(f"  P50 True Overall is in fast regime: {true_overall[50.0]:.2f} us (dominated by 150k fast ops in Phase A+C)")
    print(f"  Naive Phase Average erroneously blends slow Phase B: {naive_overall[50.0]:.2f} us (Error: {p50_error:.1f}%)")
    assert p50_error > 100.0, "Naive calculation should diverge heavily from true pooled quantile!"
    print("  [CONFIRMED] Direct secondary arithmetic on Phase quantiles is strictly invalid.")
    print("  [CONFIRMED] Ground-truth Overall nearest-rank MUST be computed from the pooled raw sample collection.")
    print("======================================================================")


if __name__ == '__main__':
    test_nearest_rank_vs_phase_quantiles()
