#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="/home/wam/grad/s14-range-delete-study"
CONFIG_FILE="${BASE_DIR}/configs/ls24/ls24_pilot_t0256.ini"
BIN="${BASE_DIR}/bin/tv_driver"
RESULTS_DIR="${BASE_DIR}/results/raw/ls24/pilot"
SUMMARY_DIR="${BASE_DIR}/results/summary"
DB_DIR="${BASE_DIR}/run-db/thesis_validation/db_ls24_pilot"
SUMMARY_CSV="${SUMMARY_DIR}/ls24_pilot_summary.csv"
TS_WALLCLOCK_CSV="${SUMMARY_DIR}/ls24_pilot_wallclock.csv"
TS_PROGRESS_CSV="${SUMMARY_DIR}/ls24_pilot_progress.csv"

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
echo "   LS24 PILOT VALIDATION RUN (T=256, 100M KEYS, 24GiB PAYLOAD)    "
echo "=================================================================="
echo "Start Time: $(date)"
echo "Checking initial disk space before pilot:"
df -h /home/wam

rm -rf "${DB_DIR}"
mkdir -p "${DB_DIR}"

"${BIN}" \
    --config "${CONFIG_FILE}" \
    --exp_id "ls24_pilot_t0256" \
    --db_path "${DB_DIR}" \
    --result_dir "${RESULTS_DIR}" \
    --summary_csv "${SUMMARY_CSV}" \
    --ts_wallclock_csv "${TS_WALLCLOCK_CSV}" \
    --ts_progress_csv "${TS_PROGRESS_CSV}"

echo ""
echo "Peak DB disk usage:"
du -sh "${DB_DIR}"
ls -lh "${DB_DIR}"

echo ""
echo "Cleaning up Pilot DB to maintain disk hygiene..."
rm -rf "${DB_DIR}"

echo "=================================================================="
echo "   LS24 PILOT RUN COMPLETED SUCCESSFULLY                          "
echo "   Finished Time: $(date)"
echo "=================================================================="
