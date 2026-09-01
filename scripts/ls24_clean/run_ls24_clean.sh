#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_FILE="${BASE_DIR}/configs/ls24_clean/ls24_clean_t0000.ini"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/ls24_clean"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/ls24_clean_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/ls24_clean_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/ls24_clean_timeseries_progress.csv"

mkdir -p "${RESULTS_DIR}" "${SUMMARY_DIR}" "${DB_DIR}"

if [[ -f "${SUMMARY_CSV}" ]]; then
    rm -f "${SUMMARY_CSV}"
fi
if [[ -f "${TS_WALLCLOCK_CSV}" ]]; then
    rm -f "${TS_WALLCLOCK_CSV}"
fi
if [[ -f "${TS_PROGRESS_CSV}" ]]; then
    rm -f "${TS_PROGRESS_CSV}"
fi

echo "=================================================================="
echo "   LS24-CLEAN: 24GiB CLEAN BASELINE WORKLOAD (3 RUNS)             "
echo "=================================================================="
echo "Start Time: $(date)"
df -h /home/wam

for rep in 1 2 3; do
    rep_str=$(printf "%02d" "${rep}")
    exp_id="ls24_clean_t0000_r${rep_str}"
    run_db="${DB_DIR}/db_${exp_id}"
    run_res="${RESULTS_DIR}/${exp_id}"
    
    echo "--------------------------------------------------------"
    echo "[LS24-Clean Run] Repetition: ${rep}/3, ID: ${exp_id}"
    echo "Time: $(date)"
    
    rm -rf "${run_db}"
    mkdir -p "${run_db}" "${run_res}"
    
    cp "${CONFIG_FILE}" "${run_res}/ls24_clean_snapshot.ini"
    
    "${BIN}" \
        --config "${CONFIG_FILE}" \
        --exp_id "${exp_id}" \
        --db_path "${run_db}" \
        --result_dir "${run_res}" \
        --summary_csv "${SUMMARY_CSV}" \
        --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
        --ts_progress_csv "${TS_PROGRESS_CSV}"
        
    echo "Cleaning up ${run_db} to maintain NVMe hygiene..."
    rm -rf "${run_db}"
done

echo ""
echo ">>> Aggregating LS24-Clean Results <<<"
python3 "${BASE_DIR}/scripts/ls24_clean/aggregate_ls24_clean.py"

echo "=================================================================="
echo "   LS24-CLEAN SUITE COMPLETED SUCCESSFULLY                        "
echo "   Finished Time: $(date)"
echo "=================================================================="
