#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_DIR="${BASE_DIR}/configs/ls24"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/ls24"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/ls24_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/ls24_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/ls24_timeseries_progress.csv"

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
    "ls24_t0000"
    "ls24_t0256"
    "ls24_t2048"
)

echo "=================================================================="
echo "   LS24: 24GiB LARGE-SCALE FORMAL SUITE (9 RUNS)                 "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"
echo "Initial Disk Space:"
df -h /home/wam

for cfg in "${CONFIGS[@]}"; do
    ini_file="${CONFIG_DIR}/${cfg}.ini"
    for rep in 1 2 3; do
        rep_str=$(printf "%02d" "${rep}")
        exp_id="${cfg}_r${rep_str}"
        run_db="${DB_DIR}/db_${exp_id}"
        run_res="${RESULTS_DIR}/${exp_id}"
        
        echo "--------------------------------------------------------"
        echo "[LS24 Run] Config: ${cfg}.ini, Repetition: ${rep}/3, ID: ${exp_id}"
        echo "Time: $(date)"
        
        rm -rf "${run_db}"
        mkdir -p "${run_db}" "${run_res}"
        
        # Save snapshot of config
        cp "${ini_file}" "${run_res}/${cfg}_snapshot.ini"
        
        "${BIN}" \
            --config "${ini_file}" \
            --exp_id "${exp_id}" \
            --db_path "${run_db}" \
            --result_dir "${run_res}" \
            --summary_csv "${SUMMARY_CSV}" \
            --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
            --ts_progress_csv "${TS_PROGRESS_CSV}"
            
        echo "Cleaning up ${run_db} to maintain NVMe hygiene..."
        rm -rf "${run_db}"
    done
done

echo ""
echo ">>> Aggregating LS24 Results <<<"
python3 "${BASE_DIR}/scripts/ls24/aggregate_ls24.py"

echo ""
echo ">>> Generating LS24 Visualization Charts <<<"
python3 "${BASE_DIR}/scripts/ls24/plot_ls24.py"

echo "=================================================================="
echo "   LS24 EXPERIMENT SUITE COMPLETED SUCCESSFULLY                   "
echo "   Finished Time: $(date)"
echo "=================================================================="
