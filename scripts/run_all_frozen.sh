#!/usr/bin/env bash
# ==============================================================================
# Master Orchestrator for Formal Pre-Experiments (P1 ~ P5)
# Repetitions: 3x per condition (Total 54 benchmark runs)
# WARNING: DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=================================================================="
echo "   ROCKSDB RANGE DELETE STUDY - FORMAL PRE-EXPERIMENT SUITE       "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

# Pre-flight Check 1: Verify NVMe filesystem mount
MOUNT_DEV=$(df "${BASE_DIR}" | awk 'NR==2 {print $1}')
if [[ "${MOUNT_DEV}" != "/dev/nvme0n1p2"* ]]; then
    echo "[CRITICAL STOP] Project directory is not on NVMe filesystem (/dev/nvme0n1p2). Found: ${MOUNT_DEV}" >&2
    exit 1
fi

# Pre-flight Check 2: Verify available space (> 50 GB)
AVAIL_KB=$(df "${BASE_DIR}" | awk 'NR==2 {print $4}')
if [ "${AVAIL_KB}" -lt 52428800 ]; then
    echo "[CRITICAL STOP] Insufficient disk space (< 50GB). Available: $((AVAIL_KB / 1024 / 1024)) GB" >&2
    exit 1
fi

# Pre-flight Check 3: Check driver binary
if [ ! -x "${BASE_DIR}/bin/workload_driver" ]; then
    echo "[CRITICAL STOP] Driver binary not found or not executable at ${BASE_DIR}/bin/workload_driver" >&2
    exit 1
fi

echo "[Pre-flight Check] All safety preconditions passed. Available Disk Space: $((AVAIL_KB / 1024 / 1024)) GB"

# Execute P1
echo -e "\n>>> Launching P1: RangeDelete Ratio Sensitivity (18 runs) <<<"
bash "${SCRIPT_DIR}/run_p1.sh"

# Execute P2
echo -e "\n>>> Launching P2: Tombstone Organization Patterns (12 runs) <<<"
bash "${SCRIPT_DIR}/run_p2.sh"

# Execute P3
echo -e "\n>>> Launching P3: Point-Heavy vs Scan-Heavy Composition (6 runs) <<<"
bash "${SCRIPT_DIR}/run_p3.sh"

# Execute P4
echo -e "\n>>> Launching P4: Deletion Interval Locality (9 runs) <<<"
bash "${SCRIPT_DIR}/run_p4.sh"

# Execute P5
echo -e "\n>>> Launching P5: Controlled Reclaim Interference (9 runs) <<<"
bash "${SCRIPT_DIR}/run_p5.sh"

# Aggregate all summaries
echo -e "\n>>> Aggregating All Experiment Results <<<"
python3 "${SCRIPT_DIR}/aggregate_results.py"

echo "=================================================================="
echo "   ALL FORMAL PRE-EXPERIMENTS COMPLETED SUCCESSFULLY              "
echo "   Finished Time: $(date)"
echo "=================================================================="
