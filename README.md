# CrashPad

CrashPad is a C11 storage-engine prototype built to make persistence and recovery behavior inspectable. It includes a file block device, a deterministic simulated device, checksummed slotted pages, a clock buffer pool, a B+-tree index, a redo log, hash-sharded stores, and a parallel in-memory hash join.

## Current implementation status

| Area | Status |
|---|---|
| File and simulated block devices | Implemented; writes are sector aligned; simulator can lose, reorder, and tear unsynced writes. |
| Slotted pages | Implemented with CRC32C, validation, compaction, and page I/O. |
| Clock buffer pool | Implemented with pinning, dirty-page eviction, and a WAL-flush callback; used when writing snapshots. |
| B+-tree | Insert, update, lookup, range scan, leaf/internal splits; leaf key/value pages are persisted at checkpoints and the index is rebuilt on open. |
| WAL | Paired prepare/commit sectors with CRC; acknowledged `put` syncs the log before returning. |
| Checkpoint | Writes checksummed leaf-page snapshots and a checksummed metadata root after syncing the WAL. It does not yet truncate the WAL. |
| Crash explorer | Sweeps trace boundaries, loss policies, seeded subsets, tears, and repeated recovery; oracle requires acknowledged writes and permits complete unacknowledged writes. `--broken-demo` demonstrates commit-before-prepare failure. |
| Shards and joins | File-backed hash shards; join repartitions arrays across worker threads and splits keys with frequency at least 8 by salting left rows and replicating hot right rows. |
| Shell and benchmark | Small key/value shell; benchmark measures the in-memory join path, not durable I/O. |

The checkpoint persists leaf key/value pages; internal index nodes are rebuilt from those pages on open. WAL records are retained after checkpoints and the prototype caps a store at 4,096 transactions to keep the WAL region separate from the snapshot region. WAL size and replay time therefore grow with the number of writes up to that cap.

## Build and run

On Linux or WSL:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Enable AddressSanitizer and UndefinedBehaviorSanitizer with `-DCRASHPAD_SANITIZERS=ON`. Build the page-fuzz smoke target with the normal build, then run `./build/crashpad-page-fuzz`. For a compiler-driven libFuzzer run, compile `tests/fuzz_page.c` with `LLVMFuzzerTestOneInput` as the entry point and link the CrashPad library.

Run the tools with:

```sh
./build/crashpad-shell ./crashpad-data
./build/crashpad-explore
./build/crashpad-bench 10000 4 zipf mitigated
./build/crashpad-bench 10000 4 zipf baseline
```

The shell accepts `put KEY VALUE`, `get KEY`, `scan FIRST LAST`, `join LEFT_FILE RIGHT_FILE`, `explain LEFT_FILE RIGHT_FILE`, and `quit`. Each relation file contains one unsigned `KEY VALUE` pair per line, separated by whitespace or a comma. Join output contains the join key and both values.

## Durability contract and limits

- A `put` is acknowledged after its prepare and commit sectors have been written and the device sync succeeds.
- Recovery replays contiguous, CRC-valid prepare/commit pairs in transaction order. Replaying the log repeatedly is idempotent because each key's latest committed value replaces its earlier value.
- The simulator reconstructs the durable image at the selected historical trace point, then applies the selected fate of unsynced writes.
- An acknowledged write must be recovered. An unacknowledged write may be absent or present if its complete commit pair reached the simulated disk.
- Cross-key transactions, cross-shard transactions, concurrent snapshots, networking, deletes, and variable-length keys are out of scope.

This is a prototype: WAL reclamation is not implemented, the coordinator scan is sequential, and skew detection uses exact in-memory frequency counts rather than sampling. Join timing is wall-clock time for the machine running the in-memory benchmark and should not be interpreted as a disk or distributed-systems measurement.

## Layout

```text
include/crashpad/   public block device, page, pager, and store interfaces
src/                block device, page, pager, store, coordinator, and shell
tools/              crash explorer and join benchmark
tests/              regression tests and page-decoder fuzz harness
docs/bug-ledger.md  reproduced failures, causes, fixes, and regressions
```

## Evidence to record after a run

| Claim | Check | Result |
|---|---|---|
| B+-tree matches reference map | deterministic permutation, 1,000 keys, and reopen | Pass |
| Page decoder rejects malformed input | boundary/corruption tests and 100,000 deterministic fuzz mutations | Pass |
| Acknowledged WAL commits survive explored crashes | 29 trace operations, 30 boundaries, 180 schedules | 0 violations |
| Explorer detects broken WAL ordering | commit durable before prepare | 1 acknowledged-key violation detected |
| Hot-key join returns exact duplicate matches | duplicate-heavy reference cases, mitigation on/off | Pass |
| Zipf join, 5,000 rows per side, alpha 1.1, 4 shards | Windows MinGW, 16 available logical cores | 951,876 rows; 0.028 s mitigated, 0.048 s baseline |
| Uniform join, 5,000 rows per side, 1 shard | same machine | 5,032 rows; 0.007 s |
| Zipf join, 5,000 rows per side, alpha 1.1, 8 shards | same machine | 951,876 rows; 0.029 s |

The recorded local build used GCC 15.2 through MinGW and Ninja on Windows; CTest passed 2/2 tests. WSL was unavailable, and this MinGW installation lacks ASAN/UBSAN runtime libraries, so sanitizer runs remain unverified.
