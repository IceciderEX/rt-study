#!/usr/bin/env bash
# ==============================================================================
# E1: Fixed Tombstone Count & Variable Coverage Span Runner (18 Runs)
# 3 Spans (10, 100, 400 Keys) x 2 Modes (static, dynamic) x 3 reps
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${BASE_DIR}/bin/tv_driver"
SUMMARY_CSV="${BASE_DIR}/results/summary/e1_all_runs.csv"
TS_WALLCLOCK_CSV="${BASE_DIR}/results/summary/e1_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${BASE_DIR}/results/summary/e1_timeseries_progress.csv"

echo "=================================================================="
echo "   E1 FIXED TOMBSTONE COUNT & VARIABLE SPAN SUITE (18 RUNS)       "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

MOUNT_DEV=$(df "${BASE_DIR}" | awk 'NR==2 {print $1}')
if [[ "${MOUNT_DEV}" != "/dev/nvme0n1p2"* ]]; then
    echo "[CRITICAL STOP] Project directory is not on NVMe filesystem (/dev/nvme0n1p2). Found: ${MOUNT_DEV}" >&2
    exit 1
fi

if [ ! -x "${BIN}" ]; then
    echo "[CRITICAL STOP] Driver binary not found at ${BIN}" >&2
    exit 1
fi

for cfg_path in "${BASE_DIR}"/configs/e1/*.ini; do
    cfg_name="$(basename "${cfg_path}")"
    base_id="$(basename "${cfg_name}" .ini)"

    for rep in 1 2 3; do
        run_id="${base_id}_r0${rep}"
        db_path="${BASE_DIR}/run-db/thesis_validation/db_${run_id}"
        raw_dir="${BASE_DIR}/results/raw/e1/${run_id}"

        echo "--------------------------------------------------------"
        echo "[E1 Run] Config: ${cfg_name}, Repetition: ${rep}/3, ID: ${run_id}"

        mkdir -p "${raw_dir}"
        rm -rf "${db_path}"

        "${BIN}" --config "${cfg_path}" \
                 --exp_id "${run_id}" \
                 --db_path "${db_path}" \
                 --result_dir "${raw_dir}" \
                 --summary_csv "${SUMMARY_CSV}" \
                 --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
                 --ts_progress_csv "${TS_PROGRESS_CSV}" \
                 | tee "${raw_dir}/driver_stdout.log"

        if [ -d "${db_path}" ]; then
            cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
            rm -rf "${db_path}"
        fi
    done
done

echo -e "\n>>> Aggregating E1 Results <<<"
python3 "${SCRIPT_DIR}/aggregate_e1.py"

echo -e "\n>>> Generating E1 Visualization Charts <<<"
python3 "${SCRIPT_DIR}/plot_e1.py"

echo "=================================================================="
echo "   E1 EXPERIMENT SUITE COMPLETED SUCCESSFULLY                     "
echo "   Finished Time: $(date)"
echo "=================================================================="
