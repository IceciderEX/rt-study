#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_DIR="${BASE_DIR}/configs/b2"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/b2"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/b2_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/b2_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/b2_timeseries_progress.csv"

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
    "b2_dynamic_t0000"
    "b2_dynamic_t0064"
    "b2_dynamic_t0256"
    "b2_dynamic_t2048"
)

echo "=================================================================="
echo "   B2 DYNAMIC 3-PHASE MISMATCH SUITE (12 RUNS)                   "
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
        echo "[B2 Run] Config: ${cfg}.ini, Repetition: ${rep}/3, ID: ${exp_id}"
        
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
echo ">>> Aggregating B2 Results <<<"
python3 "${BASE_DIR}/scripts/b2/aggregate_b2.py"

echo ""
echo ">>> Generating B2 Visualization Charts <<<"
python3 "${BASE_DIR}/scripts/b2/plot_b2.py"

echo "=================================================================="
echo "   B2 EXPERIMENT SUITE COMPLETED SUCCESSFULLY                     "
echo "   Finished Time: $(date)"
echo "=================================================================="
