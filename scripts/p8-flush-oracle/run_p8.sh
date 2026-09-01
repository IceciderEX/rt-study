#!/usr/bin/env bash
# ==============================================================================
# P8: Range Tombstone Pressure Flush Oracle Validation Runner (18 Runs)
# (2.0%, 5.0%, 10.0% x [default, oracle-flush] x 3 repetitions)
# WARNING: DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${BASE_DIR}/bin/p8_driver"
SUMMARY_CSV="${BASE_DIR}/results/summary/p8-flush-oracle/all-runs.csv"
TS_WALLCLOCK_CSV="${BASE_DIR}/results/summary/p8-flush-oracle/timeseries-wallclock.csv"
TS_PROGRESS_CSV="${BASE_DIR}/results/summary/p8-flush-oracle/timeseries-progress.csv"

CONFIGS=(
    "p8_default_ratio_020.ini"
    "p8_flush_ratio_020.ini"
    "p8_default_ratio_050.ini"
    "p8_flush_ratio_050.ini"
    "p8_default_ratio_100.ini"
    "p8_flush_ratio_100.ini"
)

echo "=================================================================="
echo "   P8 FLUSH ORACLE VALIDATION EXPERIMENT SUITE (18 RUNS)         "
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
    cfg_file="${BASE_DIR}/configs/p8-flush-oracle/${cfg_name}"
    base_id="$(basename "${cfg_name}" .ini)"

    for rep in 1 2 3; do
        run_id="${base_id}-r0${rep}"
        db_path="${BASE_DIR}/run-db/p8-flush-oracle/db_${run_id}"
        raw_dir="${BASE_DIR}/results/raw/p8-flush-oracle/${run_id}"

        echo "--------------------------------------------------------"
        echo "[P8 Run] Config: ${cfg_name}, Repetition: ${rep}/3, ID: ${run_id}"

        if [[ "${db_path}" != "${BASE_DIR}/run-db/p8-flush-oracle/"* ]]; then
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
                 --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
                 --ts_progress_csv "${TS_PROGRESS_CSV}" \
                 | tee "${raw_dir}/driver_stdout.log"

        kill "${IO_PID}" 2>/dev/null || true

        if [ -d "${db_path}" ]; then
            cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
            rm -rf "${db_path}"
        fi
    done
done

echo -e "\n>>> Aggregating P8 Results <<<"
python3 "${SCRIPT_DIR}/aggregate_p8.py"

echo -e "\n>>> Generating P8 Visualization Charts <<<"
python3 "${SCRIPT_DIR}/plot_p8.py"

echo "=================================================================="
echo "   P8 EXPERIMENT SUITE COMPLETED SUCCESSFULLY                     "
echo "   Finished Time: $(date)"
echo "=================================================================="
