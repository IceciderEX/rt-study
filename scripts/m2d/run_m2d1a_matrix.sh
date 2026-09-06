#!/usr/bin/env bash
set -euo pipefail

STUDY_ROOT="/home/wam/grad/s14-range-delete-study"
cd "$STUDY_ROOT"

DRIVER="./bin/m2d_driver_release"
SEED_DB="./run-db/m2d_canonical_seed_db"
OUT_DIR="./results/amtv_m2d/raw/m2d1a_paired"
mkdir -p "$OUT_DIR"

echo "======================================================================"
echo "Starting AMTV M2d.1a Paired Interleaved Parameter Selection Matrix"
echo "Candidate A: B64-H32"
echo "Candidate B: B128-H16"
echo "Ordering: Rep 1 (A->B), Rep 2 (B->A), Rep 3 (A->B)"
echo "CPU Affinity: NUMA node 0 (taskset -c 0-19)"
echo "======================================================================"

run_round() {
    local round_num="$1"
    local exp_id="$2"
    local config="$3"
    local rep="$4"
    local trace_dir="$5"
    local db_path="./run-db/m2d/${exp_id}_db"

    echo ""
    echo "----------------------------------------------------------------------"
    echo "Executing Matrix Round ${round_num}/6: ${exp_id} (${config}, Rep ${rep})"
    echo "Trace Dir: ${trace_dir}"
    echo "----------------------------------------------------------------------"

    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sleep 2

    taskset -c 0-19 "$DRIVER" \
        --exp-id "$exp_id" \
        --config "$config" \
        --mode release \
        --rep "$rep" \
        --seed-db "$SEED_DB" \
        --db-path "$db_path" \
        --trace-dir "$trace_dir" \
        --output-dir "$OUT_DIR"

    echo "Round ${round_num}/6 (${exp_id}) completed successfully!"
    rm -rf "$db_path"
}

# Rep 1: Candidate A -> Candidate B
run_round 1 "m2d1a_rep1_b64_h32"  "B64-H32"  1 "./traces/m2d_rep1_seed90001"
run_round 2 "m2d1a_rep1_b128_h16" "B128-H16" 1 "./traces/m2d_rep1_seed90001"

# Rep 2: Candidate B -> Candidate A
run_round 3 "m2d1a_rep2_b128_h16" "B128-H16" 2 "./traces/m2d_rep2_seed100001"
run_round 4 "m2d1a_rep2_b64_h32"  "B64-H32"  2 "./traces/m2d_rep2_seed100001"

# Rep 3: Candidate A -> Candidate B
run_round 5 "m2d1a_rep3_b64_h32"  "B64-H32"  3 "./traces/m2d_rep3_seed110001"
run_round 6 "m2d1a_rep3_b128_h16" "B128-H16" 3 "./traces/m2d_rep3_seed110001"

echo ""
echo "======================================================================"
echo "All 6 rounds of M2d.1a Paired Interleaved Matrix COMPLETED SUCCESSFULLY!"
echo "Output files located in: ${OUT_DIR}"
echo "======================================================================"
