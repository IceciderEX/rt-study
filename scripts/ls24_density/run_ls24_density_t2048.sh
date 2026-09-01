#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_DIR="${BASE_DIR}/configs/ls24_density"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/ls24_density"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation"
SUMMARY_CSV="${SUMMARY_DIR}/ls24_density_all_runs.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/ls24_density_timeseries_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/ls24_density_timeseries_progress.csv"

mkdir -p "${RESULTS_DIR}" "${SUMMARY_DIR}" "${DB_DIR}"

cfg="ls24_dp_t2048"
ini_file="${CONFIG_DIR}/${cfg}.ini"

echo "=================================================================="
echo "   LS24-DENSITY-PRESERVED: RESUMING T=2048 (3 FORMAL RUNS)       "
echo "=================================================================="
echo "Start Time: $(date)"
df -h /home/wam

for rep in 1 2 3; do
    rep_str=$(printf "%02d" "${rep}")
    exp_id="${cfg}_r${rep_str}"
    run_db="${DB_DIR}/db_${exp_id}"
    run_res="${RESULTS_DIR}/${exp_id}"
    
    echo "--------------------------------------------------------"
    echo "[LS24-DensityPreserved Run] Config: ${cfg}.ini, Repetition: ${rep}/3, ID: ${exp_id}"
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
        
    echo "Cleaning up ${run_db} to maintain NVMe hygiene..."
    rm -rf "${run_db}"
done

echo ""
echo ">>> Aggregating LS24-DensityPreserved Full 9-Run Results <<<"
python3 "${BASE_DIR}/scripts/ls24_density/aggregate_ls24_density.py"

echo ""
echo ">>> Generating LS24-DensityPreserved Visualization Charts <<<"
python3 "${BASE_DIR}/scripts/ls24_density/plot_ls24_density.py"

echo "=================================================================="
echo "   LS24-DENSITY-PRESERVED FULL SUITE COMPLETED SUCCESSFULLY       "
echo "   Finished Time: $(date)"
echo "=================================================================="
