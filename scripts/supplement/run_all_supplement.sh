#!/usr/bin/env bash
# ==============================================================================
# Master Orchestrator for Evidence Correction Pre-Experiments (S1 ~ S3)
# Total Runs: 12 (S1) + 18 (S2) + 9 (S3) = 39 runs
# WARNING: DO NOT RUN WITHOUT EXPLICIT USER AUTHORIZATION
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

echo "=================================================================="
echo "   ROCKSDB RANGE DELETE STUDY - SUPPLEMENT EXPERIMENTS SUITE     "
echo "=================================================================="
echo "Base Directory: ${BASE_DIR}"
echo "Start Time: $(date)"

# Safety Checks
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

if [ ! -x "${BASE_DIR}/bin/supp_workload_driver" ]; then
    echo "[CRITICAL STOP] Driver binary not found at ${BASE_DIR}/bin/supp_workload_driver" >&2
    exit 1
fi

echo "[Pre-flight Check] Preconditions passed. Disk Available: $((AVAIL_KB / 1024 / 1024)) GB"

# 1. Execute S1
echo -e "\n>>> Launching S1: Static Tombstone Read Cost Isolation (12 runs) <<<"
bash "${SCRIPT_DIR}/run_s1.sh"

# 2. Execute S2
echo -e "\n>>> Launching S2: Fair Tombstone Fragmentation Suite (18 runs) <<<"
bash "${SCRIPT_DIR}/run_s2.sh"

# 3. Execute S3
echo -e "\n>>> Launching S3: Controlled Reclamation Time Series Suite (9 runs) <<<"
bash "${SCRIPT_DIR}/run_s3.sh"

# 4. Aggregation
echo -e "\n>>> Aggregating Supplement Experiment Results <<<"
python3 "${SCRIPT_DIR}/aggregate_supplement.py"

echo "=================================================================="
echo "   ALL SUPPLEMENT PRE-EXPERIMENTS COMPLETED SUCCESSFULLY          "
echo "   Finished Time: $(date)"
echo "=================================================================="
