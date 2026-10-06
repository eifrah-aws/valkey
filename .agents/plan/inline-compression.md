USE SIMPLE ENGLISH (CEFR B1) IN ALL TEXT: CODE COMMENTS, COMMIT MESSAGES, DOCS, AND THIS PLAN. SHORT SENTENCES. COMMON WORDS.
BEFORE YOU START CODING, EXPLAIN THE PLAN AND GET A CLEAR "YES" FROM THE USER. DO NOT START CODING WITHOUT IT.

# Inline In-memory Compression - Execution Plan

Source design: `/Users/eifrah/devl/valkey-inline-compression-design/design-docs/inline-compression.md`
Issue: valkey-io/valkey #3423
Branch: `valkey-inline-compression`

Each phase below is one PR. Each PR must build with no warnings, pass
`make -C src test-unit`, and pass the relevant Tcl tests. Each PR must keep the
feature unreachable or safe when `compression-mode off` (the default).

---

## Status

| Phase | State | Commit |
|---|---|---|
| 0 - Compressor interface and LZ4 backend | DONE | `04bd1f1cb` |
| 1 - Compression frame | DONE | "add the compression frame" |
| 2 - Configuration | DONE, not committed | - |
| 3a - Full cycle: encoding, read path, background workers | DONE, not committed | - |
| 3b - Other read paths | NEXT | - |
| 4 to 11 | TODO | - |

## Phase 0 - Compressor interface and LZ4 backend (DONE)

Commit `04bd1f1cb`. Files moved to `src/compressor/` in Phase 1.

- `src/compressor/compressor_alg.h`, `src/compressor/compressor_alg.c`,
  `src/compressor/compressor_alg_lz4.c`.
- `src/unit/test_compressor_alg.cpp` (30 tests).
- LZ4 has no trainer, so `compressorApi.train` is NULL for LZ4.

---

## Facts found in the code that change the design

These must be fixed in the design doc, or decided before the matching phase.

1. **Encoding number.** The design says `OBJ_ENCODING_COMPRESSED` is 12. In
   `src/server.h`, 12 is `OBJ_ENCODING_LISTPACK2` and 13 is
   `OBJ_ENCODING_PATH_HASH`. The `encoding` field is 4 bits
   (`src/server.h:888`), so only 14 and 15 are free (3, 4, 5 are marked
   "No longer used" and could be reused). Use **14**.
   Decision: one generic `OBJ_ENCODING_COMPRESSED` for every type and every
   algorithm. `robj->type` gives the object kind. The frame header gives the
   algorithm (`algorithm_id`) and the encoding to rebuild (`original_encoding`).
   No per-type or per-algorithm encodings.
2. **Net-savings guard.** Appendix A shows that the guard must compare
   allocation sizes (`zmalloc_size` / `sdsAllocSize`), not byte lengths. Section
   6.4 does not say that yet. The plan uses allocation sizes.
3. **Where a read decompresses (decided: in `lookupKey()`).** When
   `lookupKey()` in `src/db.c` finds a compressed string, it decompresses it,
   stores the plain value in the keyspace, and returns it. Commands do not
   change, so no command can get a frame by mistake. Modules are covered too
   (`RM_StringDMA()`).
   - New flag `LOOKUP_NODECOMPRESS`: return the value as it is, compressed or
     not. Callers that use it must handle a compressed value.
   - `LOOKUP_NOTOUCH` does not imply `LOOKUP_NODECOMPRESS`. A module can open
     a key with NOTOUCH and still read its bytes.
   - Use `LOOKUP_NODECOMPRESS` in: `DUMP`, `MIGRATE` (`src/cluster.c`),
     `DEBUG DIGEST` / `DEBUG DIGEST-VALUE` (temporary decompress), and
     `TYPE`, `EXISTS`, `TTL`/`PTTL`, `OBJECT`, `MEMORY USAGE` (they do not read
     the bytes). Check each one in Phase 3.
   - Cost: commands like `TOUCH` and `RENAME` also decompress. This is safe.
     The sweeper compresses the key again later.
   - RDB save, AOF rewrite, slot migration export, and the sweeper do not use
     `lookupKey()`. They read the keyspace directly and must handle
     compressed values themselves.
4. **Name clash.** `src/compression.{c,h}` is the RDB / replication stream
   compression. Keep the `compressor_*` prefix for all new files. Check that
   `INFO compression` and the `COMPRESSION` command do not clash with
   `repl-compression` output.
5. **RDB version.** Current `RDB_VERSION` is 81. Phase 8 bumps it.
6. **zstd is not vendored, but it is already an optional system library.**
   `src/Makefile` has `BUILD_ZSTD` (default `no`, static libzstd >= 1.4.7),
   used by `src/compression_zstd.c` under `HAVE_ZSTD`. Phase 7 should reuse
   this, not vendor `deps/zstd`. Open question: the default mode `zstd`
   needs `BUILD_ZSTD=yes`. LZ4 needs a trainer too; the plan uses the zstd
   `ZDICT` trainer for both.
7. **Name clash for the hot-key LRU.** `src/hotkeys.c` already exists. Phase
   4 uses the `compressor_` prefix (`src/compressor/compressor_hotkeys.c`).
8. **Unit tests on macOS.** Fixed upstream by `f213a6750` (#4657). Build them
   with CMake. Pass `-DBUILD_UNIT_GTESTS=ON` on every `cmake ..`, because
   `CMakeLists.txt` does not keep it in the cache:
   `cd .build-debug && cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNIT_GTESTS=ON`
   `&& cmake --build . -j10 --target valkey-unit-gtests`
   Run them with `gtest-parallel` (see "Cross-cutting rules").

---

## Phase 1 - Compression frame (DONE)

Commit "Inline compression: add the compression frame". No keyspace change.

- `src/compressor/compressor_frame.{h,c}`.
  - 10-byte header (the design says 12): `uint32_t uncompressed_len`,
    `uint32_t dict_id` (counts up, never reused), `uint8_t algorithm_id`, then
    one byte with `format_version` (high 4 bits) and `original_encoding`
    (low 4 bits). Multi-byte fields are little endian. `original_encoding` is
    always `OBJ_ENCODING_RAW` for strings in v1.
  - Each field is read and written at a named offset with `memcpy`
    (`frameHeaderEncode()` / `frameHeaderDecode()`).
  - API:
    - `compressorFrameBuild(c, original_encoding, src, srclen, dict)`
    - `compressorFrameParse(frame, &hdr)` returns `COMPRESSOR_ERR_NONE` or a
      `COMPRESSOR_ERR_FRAME_*` code.
    - `compressorFrameDecompress(c, frame, dict)`
    - `compressorFrameSavesMemory(orig_alloc, frame_alloc, min_savings_pct)`
    - `compressorFrameAllocSize(s)`
    - `dict` is `const compressorDict *` and NULL means no dictionary.
  - Limits: `COMPRESSOR_FRAME_MAX_VALUE_LEN` (128 KiB),
    `COMPRESSOR_FRAME_FORMAT_VERSION_MAX` and
    `COMPRESSOR_FRAME_ORIGINAL_ENCODING_MAX` (both 15).
- `src/compressor/compressor_alg.h`:
  - `compressorDict {id, algorithm_id, cdict}` and
    `compressorDictIsValid()`.
  - New error codes: `COMPRESSOR_ERR_FRAME_TOO_SHORT`, `_FRAME_VERSION`,
    `_FRAME_ALGORITHM`, `_FRAME_LENGTH`, `COMPRESSOR_ERR_ALGORITHM_MISMATCH`,
    `COMPRESSOR_ERR_DICTIONARY_MISMATCH`.
- Error rules: a bad argument from our own code asserts. Bad frame data sets
  `c->last_error` and returns NULL, because frames can come from RDB. The
  frame layer never logs; the caller logs and counts.
- All feature files moved to `src/compressor/`. `src/CMakeLists.txt` adds
  `src/` as the include root of `valkey-server`.
- `src/unit/test_compressor_frame.cpp` (19 tests).

## Phase 2 - Configuration (DONE, not committed)

Configs exist and are checked. Nothing compresses yet.

- New `src/compressor/compressor_config.{h,c}`:
  - The limits and defaults of the settings.
  - The fixed v1 values of the "advanced (v2)" settings
    (`COMPRESSOR_MAX_INFLIGHT_REQUESTS`, `COMPRESSOR_MIN_SAVINGS_PCT`, ...).
  - `compressorConfigCheck()`: min value size must not be greater than max.
    It runs at startup (in `loadServerConfigFromString()`) and as the apply
    function of CONFIG SET.
- `src/config.c`, five settings:

  | Name | Range | Default | Change at runtime |
  |---|---|---|---|
  | `compression-mode` | `off`, `lz4` | `off` | No |
  | `compression-threads` | 1-16 | 1 | No |
  | `compression-min-value-size` | 1-131072 | 256 | Yes |
  | `compression-max-value-size` | 1-131072 | 131072 | Yes |
  | `compression-dict-size` | 1024-1048576 | 102400 | No |

  - `compression-mode` rejects every other value, `zstd` too, until Phase 7.
  - `compression-threads` is 1-16 (changed in 3a). 0 is rejected.
  - The max value size limit is `COMPRESSOR_FRAME_MAX_VALUE_LEN`. A bigger
    value is rejected, never clamped.
  - The `compression-dict-size` range is our choice. The design has no range.
- `src/server.h`: `server.compression_*` fields.
- `tests/unit/introspection.tcl`: the three startup-only settings are added
  to the skip list of `CONFIG sanity`.
- `tests/unit/compression-config.tcl` (7 tests).
- Docs in `valkey.conf` come in Phase 11.

## Phase 3a - Full cycle: encoding, read path, background workers (DONE, not committed)

Goal: values are compressed in the background and decompressed on read, so
we can run benchmarks. This also does most of Phase 5.

- `OBJ_ENCODING_COMPRESSED` (14) and `LOOKUP_NODECOMPRESS` in `src/server.h`.
  `LOOKUP_NOEFFECTS` does not include `LOOKUP_NODECOMPRESS`: a module can open
  a key with NOEFFECTS and still read its bytes.
- `src/compressor/compressor_object.{h,c}`:
  - `compressorGetForMainThread(algorithm_id)`: the main thread's
    compressors for decompressing, one per algorithm, made on first use.
  - `compressorSetCompressedValue(o, frame)`: install a frame.
  - `compressorDecompressStringObject(o)`: decompress in place.
  - `compressorDecompressToSds(o)`: a plain copy; `o` does not change.
  - `compressorStringObjectLen(o)`: the plain length, from the frame header.
  - Any decompress failure calls `serverPanic()`.
- `src/compressor/compressor_workers.{h,c}`, three call sites in
  `src/server.c`:
  - `compressorWorkersInit()` in `InitServerLast()`: starts
    `compression-threads` workers. Each has its own `compressorAlg`. Jobs go
    through two `mutexQueue`s.
  - `compressorCron()` in `serverCron()`: samples up to
    `COMPRESSOR_MAX_INFLIGHT_REQUESTS` keys (db picked by key count, then
    `kvstoreGetFairRandomHashtableIndex()` and
    `kvstoreHashtableSampleEntries()`). Takes RAW strings with refcount 1, in
    the size range, and cold: LFU count at most `COMPRESSOR_LFU_THRESHOLD`,
    or idle at least `COMPRESSOR_MIN_IDLE_SECONDS`. Copies the bytes and
    queues a job. Stops at `COMPRESSOR_MAX_INFLIGHT_REQUESTS` jobs in flight.
  - An in-flight set (`inflight_keys`, by db id and key) keeps a key out of
    two jobs at once.
  - The worker builds the frame and drops it if it is not smaller.
  - Queueing and installing pause while `hasActiveChildProcess()`, to avoid
    copy-on-write in the child.
  - Random numbers come from `genrand64_int64()` with rejection sampling, so
    a key count above 2^31 is covered.
  - `compressorWorkersKill()` in `killThreads()` (`debug.c`) stops the
    workers before the crash-time memory test.
  - `compressorBeforeSleep()` in `beforeSleep()`: installs a frame only if
    `canCompress()` still passes (RAW, refcount 1, size range, cold), the key
    still holds the same bytes (`memcmp`), and the savings check passes.
    Otherwise the frame is dropped, and `errno` says why.
- The robj does not change, only its value buffer and its encoding. So the
  key, the TTL, and the LRU data stay.
- `lookupKey()` calls `compressorLookupValue()`:
  - not a compressed string, or `LOOKUP_NODECOMPRESS`: the value as it is.
  - a write (`LOOKUP_WRITE`), or no child: decompress in place.
  - a read while a child runs: `compressorCreatePlainCopy()` makes a
    temporary plain copy with the same key, TTL, and LRU/LFU. The stored
    value does not change, so no copy-on-write. `compressorAfterCall()` in
    `afterCommand()` releases the copies when `server.execution_nesting` is
    0. `compressorBeforeSleep()` releases them too, as a safety net.
  - `PFCOUNT` writes its cache into the copy during a child. Accepted.
  `objectCommandLookup()` (OBJECT, DEBUG OBJECT) sets it. `MEMORY USAGE` uses
  `dbFind()`, so it does not decompress.
- Module key handles (`moduleInitKey()`): every handle gives the module a
  plain value.
  - A read handle gets its own plain copy (`compressorCreatePlainCopy()`),
    with or without a child. The stored value stays compressed, so a module
    that scans the keyspace does not decompress all of it. The handle owns the
    copy (`owns_value`), and `moduleCloseKey()` frees it.
  - `VM_OpenKey()` for reading passes `LOOKUP_NODECOMPRESS`, so the copy is
    made by `moduleInitKey()`. Scan callbacks and key events use the same rule.
  - A write handle decompresses the stored value in place.
- Hot path checks: `lookupKey()` and `moduleInitKey()` check
  `val->encoding == OBJ_ENCODING_COMPRESSED` inline before any call.
  `afterCommand()` calls `compressorAfterCall()` only when
  `compressorHasPlainCopies()` (a counter of copies on the cleanup list, not
  of copies owned by module handles). `serverCron()` and `beforeSleep()` call
  the compressor only when `compression-mode` is not off, or copies exist.
- `stringObjectLen()`, `compareStringObjectsWithFlags()`, and
  `equalStringObjects()` handle compressed values.
- Plain copy: `getDecodedObject()` (so `DEBUG DIGEST` works), RDB save
  (`rdbSaveStringObject()`), AOF rewrite (`rioWriteBulkObject()`).
- Free, dismiss, `objectComputeSize()`, `strEncoding()`, and defrag handle
  the new encoding.
- `compression-mode` with `forkless-infrastructure-enabled yes` is refused at
  startup. Forkless save reads keys on another thread, and a read changes a
  compressed value in place.
- Tests: `tests/unit/compression-encoding.tcl` (24 tests, LFU servers and
  one LRU server with `RESTORE IDLETIME`), `src/unit/test_compressor_object.cpp`
  (5 tests), two new tests in `tests/unit/moduleapi/scan.tcl`, and a new
  test module `tests/modules/compression.c` with
  `tests/unit/moduleapi/compression.tcl` (4 tests).
- Not yet (Phase 5): the hot-key list (Phase 4), the magic-byte skip table,
  `INFO compression`, and the dictionary rule. Sampling does not skip
  importing hashtables yet (design section 6.1).

## Phase 3b - Other read paths

What 3a did not do. Many of these already work in 3a, because they go
through `lookupKey()` and get a plain value. 3b makes them keep the value
compressed, and checks the code paths that do not use `lookupKey()`.

- Use `LOOKUP_NODECOMPRESS` in the callers listed in "Facts" item 3:
  `DUMP`, `MIGRATE`, `DEBUG DIGEST-VALUE` (plain copy), and `TYPE`,
  `EXISTS`, `TTL`/`PTTL` (they do not read the bytes).
- Slot migration export: check `src/cluster_migrateslots.c`, and how it reads
  values.
- Audit all direct reads of `obj->encoding` / `objectGetEncoding()` /
  `sdsEncodedObject()` for `OBJ_STRING` that do not go through
  `lookupKey()`. Put each one in one of the four classes of design section
  7.1, and list them in the PR description.
- Free path: call the backend `release()` and drop the dictionary user count
  (stub until Phase 6).
- Forkless save: remove the startup refusal. A read of a key that the
  forkless thread holds must not change the value in place.
- `DEBUG COMPRESS-KEY <key>`: compress one key now, for tests.
- Tests: `DUMP`/`RESTORE`, `TYPE`/`EXISTS`/`TTL` keep the value compressed,
  keyspace notifications unchanged, unit test for object free and size with
  a frame.

## Phase 4 - Hot-key LRU

Goal: bounded set of recently read keys.

- New `src/compressor/compressor_hotkeys.{h,c}`: keyed by `(dbid, key sds)`, MRU-to-LRU
  list, last touch time, capacity 100,000, expiry after
  `MIN_IDLE_SECONDS`.
- Hooks: client read helper inserts/refreshes. `DEL`, expiry, eviction,
  `RENAME`, `MOVE`, `FLUSHDB`, `FLUSHALL`, `SWAPDB`, replica full sync
  flush remove or update entries.
- Holds key identity only, never a `robj *`.
- Counters for `INFO compression`: entries, inserts, refreshes, capacity
  evictions, expirations, sweeper skips.
- Unit tests `src/unit/test_compressor_hotkeys.cpp`: capacity eviction order,
  expiry, refresh, removal, memory accounting.

## Phase 5 - Background sweeper, worker pool, in-flight table

Goal: real background compression without dictionaries.

- Worker pool: `compression-threads` threads. Check if `bio`, `mutexqueue`,
  or `threads_mngr` can be reused before writing a new pool. Request queue
  and completion queue. Optional CPU pinning is v2.
- Freshness (decided): compare the bytes at install, no version table, no
  epoch, no hook in `signalModifiedKey()`. The job keeps its input copy until
  install. At install: key exists, value is RAW with refcount 1, same length,
  same bytes (`memcmp`). Locking the key (`blockClientInUseOnKeys()`) was
  rejected because it adds latency to client writes. See design section
  6.3 and 6.3.1.
- Job: owned copy of the value bytes (never a live pointer), `dbid`, key
  name, `dict_id`. The worker drops a frame that is not smaller than the
  input. Input and output live until install: at most 25 MiB.
- `compressionCron()` from `serverCron`:
  - Stop if in-flight count `>= COMPRESSION_MAX_INFLIGHT`.
  - Pick a db weighted by key count, then
    `kvstoreGetFairRandomHashtableIndex()`, then
    `kvstoreHashtableSampleEntries()`. Repeat until the budget is used.
  - No normal lookup, no LRU/LFU touch, no hot-key insert.
  - Coldness gate: LFU mode uses decay and `<= LFU_THRESHOLD`; other modes
    use idle `>= MIN_IDLE_SECONDS`.
  - Eligibility: backend active, `OBJ_STRING`, `OBJ_ENCODING_RAW`,
    `refcount == 1`, size window, not in the hot-key LRU, not already in
    flight, not expired.
  - Magic-byte skip table (design section 6.1), `memcmp` only.
  - Dictionary rule (decided):
    - `zstd`: do not compress until the first dictionary exists. The first
      training starts at `COMPRESSOR_DICT_MIN_TRAINING_KEYS` keys. Reason:
      without a dictionary small values save little, these frames are never
      compressed again with the new dictionary, and training needs plain
      sample values.
    - `lz4`: compress without a dictionary (`dict_id` 0) from the start.
    - Count values skipped while waiting for a dictionary in
      `INFO compression`.
- Drain completions on the main thread (`beforeSleep`, one line:
  `compressorBeforeSleep()`): run the byte compare above, then the net
  savings guard, then install. Otherwise drop and count.
- Behavior during fork child, `loading`, `CLIENT PAUSE WRITE`, and on a
  replica (recommended: sweeper runs on replicas too; it is local memory).
- Keyspace notifications: none for install (it is not a logical change).
- `INFO compression` section: mode, threads, values compressed, bytes saved
  (allocation based), live and lifetime ratio, sampled entries, rejections by
  reason, magic-byte skips, in-flight, cap stops, stale jobs, guard
  discards, errors, hot-key counters.
- Tests:
  - Unit: the install checks (byte compare), magic-byte table.
  - Tcl `tests/unit/compression-sweeper.tcl` with a small idle time through
    `DEBUG` or a test-only config: values become compressed; a write during
    a job is not lost (`DEBUG SLEEP` / `DEBUG` hook to delay workers);
    `FLUSHALL` during a job; hot keys skipped; incompressible and image data
    skipped; `refcount > 1` skipped; memory goes down.

## Phase 6 - Dictionary registry

Goal: shared, frozen dictionaries with user counts. Still only LZ4.

- New `src/compressor/compressor_dict.{h,c}`: registry built on `compressorDict`
  (`{id, algorithm_id, cdict}`, already in `compressor_alg.h`), plus
  `bytes` and `users`, up to `DICT_MAX_VERSIONS`. One active entry.
- Each frame with `dict_id != 0` adds one user at install and removes one at
  free. Remove the entry when it is not active and users reach 0.
- A dictionary in use by an in-flight job is pinned until the job is
  drained.
- Fork safety: the child sees a copy of the registry, so a retire after fork
  can not remove a dictionary the snapshot needs. Check the same for
  `bgiteration` / threaded save paths.
- Unit tests `src/unit/test_compressor_dict.cpp`.

## Phase 7 - zstd backend and training

Goal: default algorithm and trained dictionaries.

- Vendor `deps/zstd` (Makefile and CMake). Get approval for the new
  dependency early, because it may take time.
- `src/compressor/compressor_alg_zstd.c` with `train` (ZDICT), `dict_load`
  (`ZSTD_createCDict` / `ZSTD_createDDict`), compress, decompress.
- Open: does LZ4 also get dictionaries from the zstd ZDICT trainer? If yes,
  LZ4 frames made before the first dictionary stay without one (the sweeper
  only picks plain values).
- Training job: sample up to `DICT_MAX_TRAINING_KEYS` values into a 16 MiB
  buffer on the main thread (bounded per cron call), train on a worker,
  install the new dictionary on the main thread with a new ID.
- Triggers: first reach of `DICT_MIN_TRAINING_KEYS`, drift below
  `DICT_DRIFT_RATIO` of the post-training ratio, optional interval (off in
  v1), `COMPRESSION TRAIN` command.
- If `DICT_MAX_VERSIONS` is reached, skip training and count it.
- `COMPRESSION` command: `TRAIN`, and maybe `STATS` (or only `INFO`). Add
  JSON command file under `src/commands/` and run
  `utils/generate-command-code.py`.
- Remove the `zstd` rejection from Phase 2.
- Tests: unit tests for the zstd backend (same matrix as LZ4); Tcl tests for
  first training, retrain, drain of the old dictionary, max versions.

## Phase 8 - RDB and AOF preamble persistence

Goal: save and load values as-is.

- Bump `RDB_VERSION`. Define:
  - A dictionary section written before the keyspace (new opcode or aux
    field): `algorithm_id`, `format_version`, `dict_id`, bytes. Only
    dictionaries referenced by at least one frame (or all live ones -
    simpler, decide in PR).
  - A new value type for a compressed string frame.
- Save: write the frame bytes as-is. AOF rewrite with preamble follows the
  same code. Command-form AOF stays plain (Phase 3).
- Load: rebuild the registry before any value, validate every frame
  (`frameValidate` + dictionary present), rebuild user counts. Any error
  fails the load.
- `DEBUG RELOAD`, `replicaof` full sync (disk and diskless), `rdb-key-save`
  paths, module `RM_LoadDataTypeFromString` are not affected - check.
- Downgrade: an old server must refuse the new RDB version cleanly. Decide
  and document: a "save as plain" option for downgrade (recommended:
  `DEBUG` or config switch that saves frames as plain strings).
- `valkey-check-rdb` must understand the new opcode and type.
- Tests: Tcl `tests/integration/compression-rdb.tcl` - save/load mixed
  keyspace, corrupt frame, missing dictionary, unknown algorithm, full sync
  keeps encoding, AOF with and without preamble, mixed multipart AOF.
  Add a fixed RDB fixture under `tests/assets/`.

## Phase 9 - Inline compression on import and replica apply

Goal: no memory spike during migration and replica catch-up.

- Slot migration import (atomic slot migration and `RESTORE`-based
  `MIGRATE`): compress each eligible value before install.
- Replica applying the stream: commands that create or replace an eligible
  string compress the result before install. Use the replica's own active
  dictionary.
- Same eligibility and net-savings rules as the sweeper, but no coldness
  gate.
- Tests: cluster test for slot migration with compression on the target
  only; replication test with compression on the replica only;
  `DEBUG DIGEST` equal on both sides.

## Phase 10 - Measurements (design TODOs)

Run before the feature leaves experimental status. One script per TODO,
results added to the design doc.

- Inline migration compression: destination peak memory, throughput,
  per-value latency, event-loop time.
- Inline replication compression: replica peak memory, lag, catch-up time.
- AOF load: startup time and peak memory for both preamble settings.
- Synchronous decompression on egress paths: latency, throughput, peak
  temporary memory.
- Decompress on first read: p50/p99/p99.9 latency of the first read and of later reads.
- Hot-key LRU: working sets below and above 100,000 keys.
- Wire format and replication decisions: plain vs compressed transfer.
- AOF persistence: rewrite time, child memory, copy-on-write.
- Overall target: >= 30% memory saving, < 20% TPS cost.

## Phase 11 - Documentation

- `valkey.conf` entries for the five settings, with the LZ4 vs zstd note.
- `INFO compression` field list.
- `COMPRESSION` command docs.
- Update the design doc with the items in "Facts found in the code" and the
  10-byte frame header (section 6.4).

---

## Cross-cutting rules

- RUN UNIT TESTS WITH `gtest-parallel`, ONE PROCESS PER TEST:
  `python3 deps/gtest-parallel/gtest_parallel.py .build-debug/bin/valkey-unit-gtests`
  Add `--gtest_filter='Compressor*'` to run only our tests. Do not trust a
  run of all tests in one process (`./bin/valkey-unit-gtests` with no
  filter): it crashes at `CmdFlagsTest.TestWriteFirstkeyOnly`, and
  `ObjectTest.metadata_changes_embed_threshold` fails after the other
  `ObjectTest` tests. Both problems existed before our changes.
- Change existing source files as little as possible. When it makes sense,
  put new logic in a new file (for example `src/compressor/compressor_*.c`) and call it
  from existing code with a small hook.
- All feature files live in `src/compressor/` and keep the `compressor_`
  prefix (`src/dict.h` and `src/hotkeys.c` already exist). Include them as
  `#include "compressor/compressor_xxx.h"`, never by bare name. `src/` is the
  include root (`-I.` in `src/Makefile`, `target_include_directories` in
  `src/CMakeLists.txt`); do not add `src/compressor/` to any include path.
- After adding a file to `ENGINE_SERVER_OBJ`, run `make distclean`, because
  `src/.make-settings` caches the object list used by the unit test build.
- No `void` function with an early exit. Return a value (`bool`, a count,
  or a pointer), and set `errno` when it fails. A function with no early
  exit may stay `void`.
  "Free" functions that only return early on NULL (like `free()`) also stay
  `void`: `freeCompressor()`, `lz4StateFree()`, `lz4DictFree()`.
- Do not duplicate code. When the same logic is needed in more than one
  place, add a helper function and call it from each place.
- All allocations through `zmalloc`, so the feature cost is in
  `used_memory`.
- Workers never touch the keyspace or shared server state.
- No locks or atomics in `compressor_alg*`. Only the job queues need them.
- Unit tests in minimal C++ (see `src/unit/README.md`).
- Run `clang-format-18 -i` on changed C/C++ files.
- Keep `compression-mode off` with zero cost on the hot path: one branch at
  most.
