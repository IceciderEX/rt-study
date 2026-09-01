#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_DIR="${BASE_DIR}/configs/b1_cf"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/b1_cf"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/b1_cf_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/b1_cf_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/b1_cf_timeseries_progress.csv"

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

CONFIGS=(
    "b1_cf_wb_016mb"
    "b1_cf_wb_064mb"
    "b1_cf_wb_128mb"
)

echo "=================================================================="
echo "   B1 COUNTERFACTUAL FLUSH EXPERIMENT SUITE (9 RUNS)              "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

for cfg in "${CONFIGS[@]}"; do
    ini_file="${CONFIG_DIR}/${cfg}.ini"
    for rep in 1 2 3; do
        rep_str=$(printf "%02d" "${rep}")
        exp_id="${cfg}_r${rep_str}"
        run_db="${DB_DIR}/db_${exp_id}"
        run_res="${RESULTS_DIR}/${exp_id}"
        
        echo "--------------------------------------------------------"
        echo "[B1-CF Run] Config: ${cfg}.ini, Repetition: ${rep}/3, ID: ${exp_id}"
        
        rm -rf "${run_db}"
        mkdir -p "${run_db}" "${run_res}"
        
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

echo ""
echo "=================================================================="
echo "   B1 COUNTERFACTUAL SUITE COMPLETED SUCCESSFULLY                 "
echo "   Finished Time: $(date)"
echo "=================================================================="
