#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <cassert>
#include "thread_local_histogram.h"

using namespace study::formal;

void TestSingleThreadBasic() {
    std::cout << "[Test 1] Running Single-Thread Basic Correctness Test..." << std::endl;
    DoubleBufferedHistogram db_hist;
    ThreadLocalHistogram agg;

    for (uint64_t i = 1; i <= 1000; ++i) {
        db_hist.Record(i);
    }

    db_hist.FreezeAndMergeInto(agg);
    assert(agg.GetCount() == 1000);
    assert(agg.GetSumNs() == 1000 * 1001 / 2);
    assert(agg.GetMinNs() == 1);
    assert(agg.GetMaxNs() == 1000);

    // Next round after reset
    ThreadLocalHistogram agg2;
    for (uint64_t i = 1001; i <= 2000; ++i) {
        db_hist.Record(i);
    }
    db_hist.FreezeAndMergeInto(agg2);
    assert(agg2.GetCount() == 1000);
    assert(agg2.GetSumNs() == (1000 * (1001 + 2000)) / 2);
    assert(agg2.GetMinNs() == 1001);
    assert(agg2.GetMaxNs() == 2000);

    std::cout << "[Test 1] PASSED." << std::endl;
}

void TestHighConcurrencyStress(int num_workers, int ops_per_worker) {
    std::cout << "[Test 2] Running High Concurrency Stress Test (" 
              << num_workers << " workers, " << ops_per_worker << " ops/worker, total "
              << static_cast<uint64_t>(num_workers) * ops_per_worker << " ops)..." << std::endl;

    // Allocate per-worker DoubleBufferedHistogram
    std::vector<std::unique_ptr<DoubleBufferedHistogram>> worker_hists;
    for (int i = 0; i < num_workers; ++i) {
        worker_hists.push_back(std::make_unique<DoubleBufferedHistogram>());
    }

    std::atomic<bool> start_flag{false};
    std::atomic<int> completed_workers{0};

    std::vector<uint64_t> worker_counts(num_workers, 0);
    std::vector<uint64_t> worker_sums(num_workers, 0);

    // Spawn worker threads
    std::vector<std::thread> workers;
    for (int w = 0; w < num_workers; ++w) {
        workers.emplace_back([&, w]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint64_t local_sum = 0;
            std::mt19937_64 rng(1337 + w);
            std::uniform_int_distribution<uint64_t> dist(1, 50000); // 1ns to 50us

            for (int i = 0; i < ops_per_worker; ++i) {
                uint64_t val = dist(rng);
                worker_hists[w]->Record(val);
                local_sum += val;
            }

            worker_counts[w] = ops_per_worker;
            worker_sums[w] = local_sum;
            completed_workers.fetch_add(1, std::memory_order_release);
        });
    }

    // Controller aggregator thread
    ThreadLocalHistogram global_agg;
    uint64_t total_freeze_cycles = 0;

    auto controller_fn = [&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        while (completed_workers.load(std::memory_order_acquire) < num_workers) {
            for (int w = 0; w < num_workers; ++w) {
                worker_hists[w]->FreezeAndMergeInto(global_agg);
            }
            total_freeze_cycles++;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        // Final drainage pass once all workers finish
        for (int w = 0; w < num_workers; ++w) {
            worker_hists[w]->FreezeAndMergeInto(global_agg);
        }
        total_freeze_cycles++;
    };

    std::thread controller(controller_fn);

    // Launch all threads simultaneously
    start_flag.store(true, std::memory_order_release);

    for (auto& t : workers) {
        t.join();
    }
    controller.join();

    // Verification
    uint64_t expected_total_count = 0;
    uint64_t expected_total_sum = 0;
    for (int w = 0; w < num_workers; ++w) {
        expected_total_count += worker_counts[w];
        expected_total_sum += worker_sums[w];
    }

    std::cout << "  Controller completed " << total_freeze_cycles << " freeze cycles." << std::endl;
    std::cout << "  Expected total count: " << expected_total_count 
              << ", Aggregated count: " << global_agg.GetCount() << std::endl;
    std::cout << "  Expected total sum:   " << expected_total_sum 
              << ", Aggregated sum:   " << global_agg.GetSumNs() << std::endl;

    assert(global_agg.GetCount() == expected_total_count && "COUNT MISMATCH! Possible lost or duplicate samples!");
    assert(global_agg.GetSumNs() == expected_total_sum && "SUM MISMATCH! Possible data corruption!");

    double p50, p90, p95, p99, p999, mean, max_v;
    global_agg.ComputeQuantiles(p50, p90, p95, p99, p999, mean, max_v);
    std::cout << "  P50: " << p50 << " us, P99: " << p99 << " us, Max: " << max_v << " us, Mean: " << mean << " us" << std::endl;
    assert(p50 <= p90 && p90 <= p95 && p95 <= p99 && p99 <= p999 && p999 <= max_v);

    std::cout << "[Test 2] PASSED (Strict Conservation & Quantile Monotonicity Verified)." << std::endl;
}

int main() {
    std::cout << "=========================================================" << std::endl;
    std::cout << "DoubleBufferedHistogram Concurrency & Correctness Tests" << std::endl;
    std::cout << "=========================================================" << std::endl;

    TestSingleThreadBasic();
    TestHighConcurrencyStress(8, 500000);   // 8 workers x 500k = 4M ops
    TestHighConcurrencyStress(16, 500000);  // 16 workers x 500k = 8M ops

    std::cout << "=========================================================" << std::endl;
    std::cout << "ALL TESTS PASSED SUCCESSFULLY!" << std::endl;
    std::cout << "=========================================================" << std::endl;
    return 0;
}
