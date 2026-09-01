#!/usr/bin/env bash
# ==============================================================================
# P1: E2 Value Size & Native Fixed Threshold Sanity Run
# Tests 64B and 4096B with T=0 (disabled) and T=256 (native threshold)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${BASE_DIR}/bin/tv_driver"
SANITY_SUMMARY="${BASE_DIR}/results/summary/e2_sanity_summary.csv"
SANITY_WALL="${BASE_DIR}/results/summary/e2_sanity_wallclock.csv"
SANITY_PROG="${BASE_DIR}/results/summary/e2_sanity_progress.csv"

rm -f "${SANITY_SUMMARY}" "${SANITY_WALL}" "${SANITY_PROG}"

echo "=================================================================="
echo "   E2 SANITY RUN: VALUE SIZES (64B, 4096B) x THRESHOLDS (T=0, T=256)"
echo "=================================================================="

# Test cases: (value_size, threshold, group_name)
TESTS=(
    "64 0 e2_val_0064_t000_sanity"
    "64 256 e2_val_0064_t256_sanity"
    "4096 0 e2_val_4096_t000_sanity"
    "4096 256 e2_val_4096_t256_sanity"
)

for test_item in "${TESTS[@]}"; do
    read -r v_sz thresh exp_id <<< "${test_item}"
    db_path="${BASE_DIR}/run-db/thesis_validation/db_${exp_id}"
    raw_dir="${BASE_DIR}/results/raw/e2/${exp_id}"

    echo "--------------------------------------------------------"
    echo "[Sanity Test] ValueSize: ${v_sz} B, Threshold: ${thresh}, ExpID: ${exp_id}"

    mkdir -p "${raw_dir}"
    rm -rf "${db_path}"

    "${BIN}" --exp_id "${exp_id}" \
             --group_name "${exp_id}" \
             --db_path "${db_path}" \
             --result_dir "${raw_dir}" \
             --summary_csv "${SANITY_SUMMARY}" \
             --ts_wallclock_csv "${SANITY_WALL}" \
             --ts_progress_csv "${SANITY_PROG}" \
             --value_size "${v_sz}" \
             --memtable_max_range_deletions "${thresh}" \
             | tee "${raw_dir}/driver_stdout.log"

    if [ -d "${db_path}" ]; then
        cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
        rm -rf "${db_path}"
    fi
done

echo -e "\n=================================================================="
echo "   E2 SANITY RUN FINISHED SUCCESSFULLY"
echo "=================================================================="
