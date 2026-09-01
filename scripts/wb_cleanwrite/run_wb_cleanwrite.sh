#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_DIR="${BASE_DIR}/configs/wb_cleanwrite"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/wb_cleanwrite"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/wb_cleanwrite_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/wb_cleanwrite_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/wb_cleanwrite_timeseries_progress.csv"

mkdir -p "${RESULTS_DIR}" "${SUMMARY_DIR}" "${DB_DIR}"

if [[ -f "${SUMMARY_CSV}" ]]; then
    rm -f "${SUMMARY_CSV}"
fi

CONFIGS=(
    "wb_cleanwrite_16mb"
    "wb_cleanwrite_64mb"
    "wb_cleanwrite_128mb"
)

echo "=================================================================="
echo "   WB-CLEANWRITE: WRITE BUFFER SIZE CLEAN WORKLOAD (9 RUNS)       "
echo "=================================================================="
echo "Start Time: $(date)"
df -h /home/wam

for cfg in "${CONFIGS[@]}"; do
    ini_file="${CONFIG_DIR}/${cfg}.ini"
    for rep in 1 2 3; do
        rep_str=$(printf "%02d" "${rep}")
        exp_id="${cfg}_r${rep_str}"
        run_db="${DB_DIR}/db_${exp_id}"
        run_res="${RESULTS_DIR}/${exp_id}"
        
        echo "--------------------------------------------------------"
        echo "[WB-CleanWrite Run] Config: ${cfg}.ini, Repetition: ${rep}/3, ID: ${exp_id}"
        echo "Time: $(date)"
        
        rm -rf "${run_db}"
        mkdir -p "${run_db}" "${run_res}"
        
        cp "${ini_file}" "${run_res}/${cfg}_snapshot.ini"
        
        "${BIN}" \
            --config "${ini_file}" \
            --exp_id "${exp_id}" \
            --db_path "${run_db}" \
            --result_dir "${run_res}" \
            --summary_csv "${SUMMARY_CSV}" \
            --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
            --ts_progress_csv "${TS_PROGRESS_CSV}"
            
        rm -rf "${run_db}"
    done
done

echo "=================================================================="
echo "   WB-CLEANWRITE EXPERIMENT SUITE COMPLETED                      "
echo "   Finished Time: $(date)"
echo "=================================================================="
