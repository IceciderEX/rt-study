#!/usr/bin/env bash
# ==============================================================================
# P6: Dynamic DeleteRange Origin Localization Runner (9 runs: D0, D1, D2)
# Repetitions: 3 per configuration
# WARNING: DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${BASE_DIR}/bin/p6_driver"
SUMMARY_CSV="${BASE_DIR}/results/summary/p6-dynamic-origin/all-runs.csv"
TS_CSV="${BASE_DIR}/results/summary/p6-dynamic-origin/timeseries.csv"

CONFIGS=(
    "p6_d0_clean.ini"
    "p6_d1_static.ini"
    "p6_d2_dynamic.ini"
)

echo "=================================================================="
echo "   P6 DYNAMIC RANGE DELETION ORIGIN EXPERIMENT SUITE             "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

# Safety Checks
MOUNT_DEV=$(df "${BASE_DIR}" | awk 'NR==2 {print $1}')
if [[ "${MOUNT_DEV}" != "/dev/nvme0n1p2"* ]]; then
    echo "[CRITICAL STOP] Project directory is not on NVMe filesystem (/dev/nvme0n1p2). Found: ${MOUNT_DEV}" >&2
    exit 1
fi

AVAIL_KB=$(df "${BASE_DIR}" | awk 'NR==2 {print $4}')
if [ "${AVAIL_KB}" -lt 52428800 ]; then
    echo "[CRITICAL STOP] Insufficient disk space (< 50GB). Available: $((AVAIL_KB / 1024 / 1024)) GB" >&2
    exit 1
fi

if [ ! -x "${BIN}" ]; then
    echo "[CRITICAL STOP] Driver binary not found at ${BIN}" >&2
    exit 1
fi

for cfg_name in "${CONFIGS[@]}"; do
    cfg_file="${BASE_DIR}/configs/p6-dynamic-origin/${cfg_name}"
    base_id="$(basename "${cfg_name}" .ini)"

    for rep in 1 2 3; do
        run_id="${base_id}_rep${rep}"
        db_path="${BASE_DIR}/run-db/p6-dynamic-origin/db_${run_id}"
        raw_dir="${BASE_DIR}/results/raw/p6-dynamic-origin/${run_id}"

        echo "--------------------------------------------------------"
        echo "[P6 Run] Config: ${cfg_name}, Repetition: ${rep}/3, ID: ${run_id}"

        if [[ "${db_path}" != "${BASE_DIR}/run-db/p6-dynamic-origin/"* ]]; then
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
                 --timeseries_csv "${TS_CSV}" \
                 | tee "${raw_dir}/driver_stdout.log"

        kill "${IO_PID}" 2>/dev/null || true

        if [ -d "${db_path}" ]; then
            cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
            rm -rf "${db_path}"
        fi
    done
done

echo -e "\n>>> Aggregating P6 Results <<<"
python3 "${SCRIPT_DIR}/aggregate_p6.py"

echo -e "\n>>> Generating P6 Visualization Charts <<<"
python3 "${SCRIPT_DIR}/plot_p6.py"

echo "=================================================================="
echo "   P6 EXPERIMENT SUITE COMPLETED SUCCESSFULLY                     "
echo "   Finished Time: $(date)"
echo "=================================================================="
