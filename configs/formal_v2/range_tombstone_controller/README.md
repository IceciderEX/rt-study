# Range Tombstone Controller Pilot

This directory defines the first Formal V2 comparison for the prototype branch.
All modes replay the immutable `small_dynamic_500k_limit` trace and retain the
same RocksDB sizing, L0, cache, and worker settings.

| Mode | Native `memtable_max_range_deletions` | Controller | Meaning |
| --- | ---: | --- | --- |
| `native_t512.ini` | 512 | disabled | Existing native fixed-count comparator. |
| `controller_observe.ini` | 0 | enabled, observe-only | Establishes the controller's would-flush decisions without changing flush behavior. |
| `controller_active.ini` | 0 | enabled, active | Candidate policy: at least 512 range tombstones, 8 MiB active MemTable, 1 s per-CF cooldown, and no L0/pending-compaction pressure. |

The active policy is intentionally not described as an equivalent replacement
for T512: unlike the native fixed trigger, it also requires a MemTable-byte
floor and yields under existing write pressure. Every run records all policy
parameters and exact controller flush counts in the summary CSV; the events
CSV records `Range Tombstone Controller` as the engine-provided flush reason.

Run one interleaved pilot repetition with:

```bash
python3 scripts/formal_v2/run_range_tombstone_controller_pilot.py
```

Use `--repetitions N` for repeated pilot runs and `--output-dir PATH` to set an
explicit output location. For three modes, the runner uses a balanced
Latin-square order and saves it as `schedule.json`; use `--repetitions 5` for
the first repeated comparison. Successful runs remove only their transient DB
directories; raw CSV, logs, and `run_meta.json` remain for audit.
