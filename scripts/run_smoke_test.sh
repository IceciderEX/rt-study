#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

CONFIG_FILE="${BASE_DIR}/configs/smoke_test.ini"
RAW_DIR="${BASE_DIR}/results/raw/smoke-test"
SUMMARY_CSV="${BASE_DIR}/results/summary/smoke-test.csv"
DB_DIR="${BASE_DIR}/run-db/smoke-test-db"
BIN="${BASE_DIR}/bin/workload_driver"

echo "=== Starting Smoke Test Execution ==="
echo "Base Directory: ${BASE_DIR}"
echo "Binary: ${BIN}"
echo "Config: ${CONFIG_FILE}"
echo "Target DB: ${DB_DIR}"
echo "Raw Results: ${RAW_DIR}"

# 1. Prepare directories
mkdir -p "${RAW_DIR}"
mkdir -p "${BASE_DIR}/results/summary"
mkdir -p "${BASE_DIR}/run-db"

# 2. Clean only the isolated smoke-test DB
if [ -d "${DB_DIR}" ]; then
    echo "Cleaning previous smoke test database at ${DB_DIR}..."
    rm -rf "${DB_DIR}"
fi

# 3. Clean previous summary CSV for clean run
rm -f "${SUMMARY_CSV}"

# 4. Start background IO monitor
IOSTAT_LOG="${RAW_DIR}/system_iostat.log"
iostat -xz 1 > "${IOSTAT_LOG}" 2>&1 &
IOSTAT_PID=$!

cleanup() {
    if kill -0 "${IOSTAT_PID}" 2>/dev/null; then
        kill "${IOSTAT_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# 5. Execute Workload Driver
echo "Executing Workload Driver..."
"${BIN}" --config "${CONFIG_FILE}" | tee "${RAW_DIR}/driver_stdout.log"

# 6. Archive RocksDB internal LOG
if [ -d "${DB_DIR}" ]; then
    echo "Archiving RocksDB LOG files..."
    cp "${DB_DIR}"/LOG* "${RAW_DIR}/" 2>/dev/null || true
fi

echo "=== Smoke Test Finished Successfully ==="
