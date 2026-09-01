#!/usr/bin/env bash
# ==============================================================================
# P1: Range Deletion Ratio Sensitivity Experiment Runner
# Repetitions: 3 per configuration
# DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${BASE_DIR}/bin/workload_driver"
SUMMARY_CSV="${BASE_DIR}/results/summary/p1_summary.csv"

CONFIGS=(
    "p1_range_del_ratio_0pct.ini"
    "p1_range_del_ratio_0_5pct.ini"
    "p1_range_del_ratio_1pct.ini"
    "p1_range_del_ratio_2pct.ini"
    "p1_range_del_ratio_5pct.ini"
    "p1_range_del_ratio_10pct.ini"
)

echo "=== P1 Range Deletion Ratio Sensitivity Plan ==="
echo "Total Configurations: ${#CONFIGS[@]}, Repetitions: 3, Total Runs: $((${#CONFIGS[@]} * 3))"

for cfg_name in "${CONFIGS[@]}"; do
    cfg_file="${BASE_DIR}/configs/${cfg_name}"
    base_id="$(basename "${cfg_name}" .ini)"

    for rep in 1 2 3; do
        run_id="${base_id}_rep${rep}"
        db_path="${BASE_DIR}/run-db/db_${run_id}"
        raw_dir="${BASE_DIR}/results/raw/${run_id}"

        echo "--------------------------------------------------------"
        echo "[P1 Run] Config: ${cfg_name}, Repetition: ${rep}/3, ID: ${run_id}"
        echo "  DB Path: ${db_path}"
        echo "  Raw Dir: ${raw_dir}"

        # Ensure safety: db_path must be inside run-db
        if [[ "${db_path}" != "${BASE_DIR}/run-db/"* ]]; then
            echo "SAFETY ERROR: Invalid db_path: ${db_path}" >&2
            exit 1
        fi

        mkdir -p "${raw_dir}"
        rm -rf "${db_path}"

        # Start IO monitor
        iostat -xz 1 > "${raw_dir}/system_iostat.log" 2>&1 &
        IO_PID=$!

        # Run driver with isolated path overrides
        "${BIN}" --config "${cfg_file}" \
                 --exp_id "${run_id}" \
                 --db_path "${db_path}" \
                 --result_dir "${raw_dir}" \
                 --summary_csv "${SUMMARY_CSV}" \
                 | tee "${raw_dir}/driver_stdout.log"

        kill "${IO_PID}" 2>/dev/null || true

        # Archive RocksDB LOG
        if [ -d "${db_path}" ]; then
            cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
            # Clean db directory after archiving raw logs to save disk space
            rm -rf "${db_path}"
        fi
    done
done

echo "=== P1 Benchmark Finished ==="
