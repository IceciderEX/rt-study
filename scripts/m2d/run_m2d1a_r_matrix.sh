#!/usr/bin/env bash
set -euo pipefail

STUDY_ROOT="/home/wam/grad/s14-range-delete-study"
cd "$STUDY_ROOT"

DRIVER="./bin/m2d_driver_release"
SEED_DB="./run-db/m2d_canonical_seed_db"
OUT_DIR="./results/amtv_m2d/raw/m2d1a_r_paired"
mkdir -p "$OUT_DIR"

echo "======================================================================"
echo "Starting AMTV M2d.1a-R N=4 Paired Parameter Selection Re-check Matrix"
echo "Candidate A: B64-H32"
echo "Candidate B: B128-H16"
echo "Protocol: Zero Global Cache Drops (No drop_caches, No sudo, No sysctl)"
echo "Ordering: Rep01 (A->B), Rep02 (B->A), Rep03 (B->A), Rep04 (A->B)"
echo "CPU Affinity: NUMA node 0 physical cores (taskset -c 0-19)"
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
    echo "Executing Matrix Round ${round_num}/8: ${exp_id} (${config}, Rep ${rep})"
    echo "Trace Dir: ${trace_dir}"
    echo "----------------------------------------------------------------------"

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

    echo "Round ${round_num}/8 (${exp_id}) completed successfully!"
    rm -rf "$db_path"
}

# Rep01 (seed=120001): A -> B
run_round 1 "m2d1a_r_rep01_b64_h32"  "B64-H32"  1 "./traces/m2d_r_rep01_seed120001"
run_round 2 "m2d1a_r_rep01_b128_h16" "B128-H16" 1 "./traces/m2d_r_rep01_seed120001"

# Rep02 (seed=130001): B -> A
run_round 3 "m2d1a_r_rep02_b128_h16" "B128-H16" 2 "./traces/m2d_r_rep02_seed130001"
run_round 4 "m2d1a_r_rep02_b64_h32"  "B64-H32"  2 "./traces/m2d_r_rep02_seed130001"

# Rep03 (seed=140001): B -> A
run_round 5 "m2d1a_r_rep03_b128_h16" "B128-H16" 3 "./traces/m2d_r_rep03_seed140001"
run_round 6 "m2d1a_r_rep03_b64_h32"  "B64-H32"  3 "./traces/m2d_r_rep03_seed140001"

# Rep04 (seed=150001): A -> B
run_round 7 "m2d1a_r_rep04_b64_h32"  "B64-H32"  4 "./traces/m2d_r_rep04_seed150001"
run_round 8 "m2d1a_r_rep04_b128_h16" "B128-H16" 4 "./traces/m2d_r_rep04_seed150001"

echo ""
echo "======================================================================"
echo "All 8 rounds of M2d.1a-R Paired Re-check Matrix COMPLETED SUCCESSFULLY!"
echo "Output files located in: ${OUT_DIR}"
echo "======================================================================"
