# CrashPad

CrashPad is a C11 storage-engine prototype built to make persistence and recovery behavior inspectable. It includes a file block device, a deterministic simulated device, checksummed slotted pages, a clock buffer pool, a B+-tree index, a redo log, hash-sharded stores, and a parallel in-memory hash join.

## Current implementation status

| Area | Status |
|---|---|
| File and simulated block devices | Implemented; writes are sector aligned; simulator can lose, reorder, and tear unsynced writes. |
| Slotted pages | Implemented with CRC32C, validation, compaction, and page I/O. |
| Clock buffer pool | Implemented with pinning, dirty-page eviction, and a WAL-flush callback; used when writing snapshots. |
| B+-tree | Insert, update, lookup, range scan, leaf/internal splits; leaf key/value pages are persisted at checkpoints and the index is rebuilt on open. |
| WAL | Paired prepare/commit sectors with CRC; acknowledged `put` syncs the log before returning. An ambiguous WAL write/sync error poisons the live store until successful recovery. |
| Checkpoint | Writes checksummed leaf-page snapshots and a checksummed metadata root after syncing the WAL. It does not yet truncate the WAL. |
| Crash explorer | Sweeps trace boundaries, loss policies, seeded subsets, tears, and repeated recovery; oracle requires acknowledged writes and permits complete unacknowledged writes. `--broken-demo` demonstrates commit-before-prepare failure. |
| Shards and joins | File-backed hash shards; join repartitions arrays across worker threads and splits keys with frequency at least 8 by salting left rows and replicating hot right rows. |
| Shell and benchmarks | Small key/value shell; both benchmarks use monotonic timers. The store benchmark uses a no-op in-memory device to isolate tree/WAL CPU overhead, not durable I/O. |

The checkpoint persists leaf key/value pages; internal index nodes are rebuilt from those pages on open. WAL records are retained after checkpoints and the prototype caps a store at 4,096 transactions to keep the WAL region separate from the snapshot region. WAL size and replay time therefore grow with the number of writes up to that cap. For allocation-failure safety, each `put` currently clones the in-memory B+-tree before logging, making write preparation O(tree size); `crashpad-store-bench` makes this scaling visible. Replacing it with path copying or rollback is deferred until that behavior has dedicated correctness coverage.

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
./build/crashpad-store-bench 2048
```

The shell accepts `put KEY VALUE`, `get KEY`, `scan FIRST LAST`, `join LEFT_FILE RIGHT_FILE`, `explain LEFT_FILE RIGHT_FILE`, and `quit`. Each relation file contains one unsigned `KEY VALUE` pair per line, separated by whitespace or a comma. Join output contains the join key and both values. Coordinator scans visit shards sequentially without a global snapshot; their callback runs while the current shard/store locks are held and must not re-enter the coordinator or store.

## Durability contract and limits

- A `put` is acknowledged after its prepare and commit sectors have been written and the device sync succeeds.
- Recovery replays contiguous, CRC-valid prepare/commit pairs in transaction order. Replaying the log repeatedly is idempotent because each key's latest committed value replaces its earlier value.
- The simulator reconstructs the durable image at the selected historical trace point, then applies the selected fate of unsynced writes.
- An acknowledged write must be recovered. An unacknowledged write may be absent or present if its complete commit pair reached the simulated disk.
- After an ambiguous WAL write or sync error, reads continue to see the last published in-memory tree, but puts return `CP_E_IO` until `cp_store_recover()` succeeds. Recovery may include the failed operation if a complete pair is present.
- Cross-key transactions, cross-shard transactions, concurrent snapshots, networking, deletes, and variable-length keys are out of scope.

This is a prototype: WAL reclamation is not implemented, the coordinator scan is sequential, and skew detection uses exact in-memory frequency counts rather than sampling. Benchmark timings are local elapsed time for in-memory paths and should not be interpreted as disk or distributed-systems measurements.

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
| Acknowledged WAL commits survive explored crashes | 63 trace operations, 64 boundaries, 384 schedules; 16 puts include repeated updates and 3 checkpoints | 0 violations |
| Explorer detects broken WAL ordering | commit durable before prepare | 1 acknowledged-key violation detected |
| Hot-key join returns exact duplicate matches | duplicate-heavy reference cases, mitigation on/off | Pass |
| Zipf join, 5,000 rows per side, alpha 1.1, 4 shards | Windows MinGW Release, 16 available logical cores; 7 runs per mode; same parallel hash join in both modes; nested-loop result multiset built and checked outside timed region | 951,876 rows in both; median 0.0227 s mitigated, 0.0360 s unmitigated |
| Store put CPU scaling | Windows MinGW Release; no-op in-memory device; 3 runs; B+-tree/WAL code path, no disk latency | 128 puts: 0.000182 s; 512: 0.001541 s; 2,048: 0.017419 s median |
| WAL prepare, commit, and sync failures | one-shot injected failures; second put rejected until recovery | Pass |
| Debug and Release CTest | Windows MinGW and Ninja | 4/4 tests pass in each configuration; hosted CI not run |
| Hosted CI | Linux Debug/Release, Linux ASan/UBSan, Windows Debug | Workflow added; hosted runs pending |

The recorded local build used GCC 15.2 through MinGW and Ninja on Windows. WSL was unavailable, and this MinGW installation lacks ASAN/UBSAN runtime libraries, so sanitizer runs remain unverified. The join timings are local wall-clock measurements of the in-memory path, not a disk or distributed-systems measurement.
