#!/usr/bin/env bash
# ==============================================================================
# Resume Script for Remaining Formal Pre-Experiments:
# 1. P3 Scan-Heavy (3 runs)
# 2. P4 Locality (9 runs)
# 3. P5 Compaction Reclaim (9 runs)
# 4. Aggregation of all results
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${BASE_DIR}/bin/workload_driver"

echo "=================================================================="
echo "   ROCKSDB RANGE DELETE STUDY - RESUMING REMAINING EXPERIMENTS    "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

# Pre-flight checks
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

# Step 1: Execute P3 Scan-Heavy
echo -e "\n>>> Launching P3: Scan-Heavy Composition (3 runs) <<<"
P3_SUMMARY_CSV="${BASE_DIR}/results/summary/p3_summary.csv"
for rep in 1 2 3; do
    run_id="p3_scan_heavy_rep${rep}"
    db_path="${BASE_DIR}/run-db/db_${run_id}"
    raw_dir="${BASE_DIR}/results/raw/${run_id}"
    cfg_file="${BASE_DIR}/configs/p3_scan_heavy.ini"

    echo "--------------------------------------------------------"
    echo "[P3 Run] Config: p3_scan_heavy.ini, Repetition: ${rep}/3, ID: ${run_id}"

    if [[ "${db_path}" != "${BASE_DIR}/run-db/"* ]]; then
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
             --summary_csv "${P3_SUMMARY_CSV}" \
             | tee "${raw_dir}/driver_stdout.log"

    kill "${IO_PID}" 2>/dev/null || true

    if [ -d "${db_path}" ]; then
        cp "${db_path}"/LOG* "${raw_dir}/" 2>/dev/null || true
        rm -rf "${db_path}"
    fi
done

# Step 2: Execute P4
echo -e "\n>>> Launching P4: Deletion Interval Locality (9 runs) <<<"
bash "${SCRIPT_DIR}/run_p4.sh"

# Step 3: Execute P5
echo -e "\n>>> Launching P5: Controlled Reclaim Interference (9 runs) <<<"
bash "${SCRIPT_DIR}/run_p5.sh"

# Step 4: Aggregate all summaries
echo -e "\n>>> Aggregating All Experiment Results <<<"
python3 "${SCRIPT_DIR}/aggregate_results.py"

echo "=================================================================="
echo "   ALL REMAINING PRE-EXPERIMENTS COMPLETED SUCCESSFULLY           "
echo "   Finished Time: $(date)"
echo "=================================================================="
