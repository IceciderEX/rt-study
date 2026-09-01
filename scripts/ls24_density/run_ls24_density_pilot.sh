#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_FILE="${BASE_DIR}/configs/ls24_density/ls24_dp_pilot_t0256.ini"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/ls24_density/pilot"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation/db_ls24_dp_pilot"
SUMMARY_CSV="${SUMMARY_DIR}/ls24_dp_pilot_summary.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/ls24_dp_pilot_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/ls24_dp_pilot_progress.csv"

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
echo "   LS24-DENSITY-PRESERVED PILOT (T=256, 40% COVERAGE, 24GiB)      "
echo "=================================================================="
echo "Start Time: $(date)"
df -h /home/wam

rm -rf "${DB_DIR}"
mkdir -p "${DB_DIR}"

"${BIN}" \
    --config "${CONFIG_FILE}" \
    --exp_id "ls24_dp_pilot_t0256" \
    --db_path "${DB_DIR}" \
    --result_dir "${RESULTS_DIR}" \
    --summary_csv "${SUMMARY_CSV}" \
    --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
    --ts_progress_csv "${TS_PROGRESS_CSV}"

echo "Peak DB disk usage:"
du -sh "${DB_DIR}"

echo "Cleaning up Pilot DB to maintain NVMe hygiene..."
rm -rf "${DB_DIR}"

echo "=================================================================="
echo "   LS24-DENSITY-PRESERVED PILOT COMPLETED                         "
echo "   Finished Time: $(date)"
echo "=================================================================="
