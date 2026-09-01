#!/usr/bin/env python3
"""Run the first auditable Formal V2 comparison for the controller prototype."""

import argparse
import csv
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


BASE_DIR = Path("/home/wam/grad/s14-range-delete-study")
DRIVER_BIN = BASE_DIR / "bin" / "formal_driver"
CONFIG_DIR = BASE_DIR / "configs" / "formal_v2" / "range_tombstone_controller"

MODES = (
    {
        "name": "native_t512",
        "group": "RTC-NATIVE-T512",
        "config": CONFIG_DIR / "native_t512.ini",
        "expects_controller_flush": False,
    },
    {
        "name": "controller_observe",
        "group": "RTC-CONTROLLER-OBSERVE",
        "config": CONFIG_DIR / "controller_observe.ini",
        "expects_controller_flush": False,
    },
    {
        "name": "controller_active",
        "group": "RTC-CONTROLLER-ACTIVE",
        "config": CONFIG_DIR / "controller_active.ini",
        "expects_controller_flush": True,
    },
)

# Three-mode balanced Latin-square orders. The first three repetitions form a
# Latin square; repetitions four through six reverse the order, balancing the
# first/last position for a full six-repetition experiment.
INTERLEAVED_ORDERS = (
    ("native_t512", "controller_observe", "controller_active"),
    ("controller_observe", "controller_active", "native_t512"),
    ("controller_active", "native_t512", "controller_observe"),
    ("controller_active", "controller_observe", "native_t512"),
    ("controller_observe", "native_t512", "controller_active"),
    ("native_t512", "controller_active", "controller_observe"),
)


def read_rows(path: Path):
    with path.open(newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def make_schedule(mode_name: str, repetitions: int):
    modes_by_name = {mode["name"]: mode for mode in MODES}
    if mode_name != "all":
        return [
            (repetition, modes_by_name[mode_name])
            for repetition in range(1, repetitions + 1)
        ]
    return [
        (repetition, modes_by_name[name])
        for repetition in range(1, repetitions + 1)
        for name in INTERLEAVED_ORDERS[(repetition - 1) % len(INTERLEAVED_ORDERS)]
    ]


def run_mode(mode, repetition: int, output_dir: Path, summary_csv: Path,
             events_csv: Path, phases_csv: Path, keep_db: bool):
    exp_id = f"rtc_{mode['name']}_rep{repetition:02d}"
    run_dir = output_dir / exp_id
    db_path = output_dir / "run-db" / exp_id
    run_dir.mkdir(parents=True, exist_ok=False)

    command = [
        str(DRIVER_BIN),
        "--config", str(mode["config"]),
        "--exp_id", exp_id,
        "--group_name", mode["group"],
        "--db_path", str(db_path),
        "--result_dir", str(run_dir),
        "--summary_csv", str(summary_csv),
        "--events_csv", str(events_csv),
        "--phases_csv", str(phases_csv),
    ]
    (run_dir / "run_meta.json").write_text(
        json.dumps(
            {
                "exp_id": exp_id,
                "mode": mode["name"],
                "config": str(mode["config"]),
                "command": command,
            },
            indent=2,
        )
        + "\n"
    )

    log_path = run_dir / "driver.log"
    with log_path.open("w") as log_file:
        result = subprocess.run(command, stdout=log_file,
                                stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{exp_id} failed with exit code {result.returncode}; see {log_path}"
        )

    rows = [row for row in read_rows(summary_csv) if row["exp_id"] == exp_id]
    if len(rows) != 1:
        raise RuntimeError(f"{exp_id} should produce exactly one summary row")
    row = rows[0]
    if row["verification_status"] != "PASS":
        raise RuntimeError(f"{exp_id} did not pass deep KV verification")

    controller_flushes = int(row["controller_total_flush_count"])
    if mode["expects_controller_flush"] != (controller_flushes > 0):
        raise RuntimeError(
            f"{exp_id}: controller flush expectation mismatch; observed "
            f"{controller_flushes}"
        )

    if not keep_db:
        shutil.rmtree(db_path)
    return row


def main():
    parser = argparse.ArgumentParser(
        description="Run Formal V2 native T512 vs controller pilot."
    )
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--mode",
        choices=("all",) + tuple(mode["name"] for mode in MODES),
        default="all",
        help="Run all modes or resume one mode in an existing output directory.",
    )
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--keep-db", action="store_true")
    args = parser.parse_args()

    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    if not DRIVER_BIN.is_file():
        parser.error(f"driver not found: {DRIVER_BIN}; build tools/formal_v2 first")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_dir or (
        BASE_DIR / "results" / "formal_v2" / "range_tombstone_controller" / stamp
    )
    if output_dir.exists():
        if args.mode == "all":
            parser.error(f"output directory already exists: {output_dir}")
    else:
        output_dir.mkdir(parents=True)

    summary_csv = output_dir / "summary.csv"
    events_csv = output_dir / "events.csv"
    phases_csv = output_dir / "phases.csv"
    schedule = make_schedule(args.mode, args.repetitions)
    (output_dir / "schedule.json").write_text(
        json.dumps(
            [
                {"repetition": repetition, "mode": mode["name"]}
                for repetition, mode in schedule
            ],
            indent=2,
        )
        + "\n"
    )
    rows = []
    try:
        for repetition, mode in schedule:
            print(f"[START] {mode['name']} rep {repetition}", flush=True)
            row = run_mode(mode, repetition, output_dir, summary_csv,
                           events_csv, phases_csv, args.keep_db)
            rows.append(row)
            print(
                f"[PASS] {row['exp_id']}: "
                f"controller_flushes={row['controller_total_flush_count']}, "
                f"sha256={row['sha256_hex']}",
                flush=True,
            )
    except Exception as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1

    completed_rows = read_rows(summary_csv)
    if any(row["verification_status"] != "PASS" for row in completed_rows):
        print("[FAIL] a completed pilot row did not pass verification", file=sys.stderr)
        return 1
    checksums = {row["sha256_hex"] for row in completed_rows}
    if len(checksums) != 1:
        print("[FAIL] state checksum differs across pilot modes", file=sys.stderr)
        return 1

    print("\nPilot completed successfully.")
    print(f"Summary: {summary_csv}")
    print(f"Events:  {events_csv}")
    print(f"Phases:  {phases_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
