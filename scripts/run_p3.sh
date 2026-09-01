#!/usr/bin/env bash
# ==============================================================================
# P3: Point-Heavy vs Scan-Heavy Read Composition Runner
# Repetitions: 3 per configuration
# DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${BASE_DIR}/bin/workload_driver"
SUMMARY_CSV="${BASE_DIR}/results/summary/p3_summary.csv"

CONFIGS=(
    "p3_point_heavy.ini"
    "p3_scan_heavy.ini"
)

echo "=== P3 Read Composition (Point vs Scan Heavy) Plan ==="
echo "Total Configurations: ${#CONFIGS[@]}, Repetitions: 3, Total Runs: $((${#CONFIGS[@]} * 3))"

for cfg_name in "${CONFIGS[@]}"; do
    cfg_file="${BASE_DIR}/configs/${cfg_name}"
    base_id="$(basename "${cfg_name}" .ini)"

    for rep in 1 2 3; do
        run_id="${base_id}_rep${rep}"
        db_path="${BASE_DIR}/run-db/db_${run_id}"
        raw_dir="${BASE_DIR}/results/raw/${run_id}"

        echo "--------------------------------------------------------"
        echo "[P3 Run] Config: ${cfg_name}, Repetition: ${rep}/3, ID: ${run_id}"

        if [[ "${db_path}" != "${BASE_DIR}/run-db/"* ]]; then
            echo "SAFETY ERROR: Invalid db_path: ${db_path}" >&2
            exit 1
        fi

        mkdir -p "${raw_dir}"
        rm -rf "${db_path}"

        iostat -xz 1 > "${raw_dir}/system_iostat.log" 2>&1 &
        IO_PID=$!

        "${BIN}" --config "${cfg_file}" \
                 --exp_id "${run_id}" \
                 --db_path "${db_path}" \
                 --result_dir "${raw_dir}" \
                 --summary_csv "${SUMMARY_CSV}" \
                 | tee "${raw_dir}/driver_stdout.log"

        kill "${IO_PID}" 2>/dev/null || true

        if [ -d "${db_path}" ]; then
            cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
            rm -rf "${db_path}"
        fi
    done
done

echo "=== P3 Benchmark Finished ==="
