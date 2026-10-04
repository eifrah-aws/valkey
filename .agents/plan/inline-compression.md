USE SIMPLE ENGLISH (CEFR B1) IN ALL TEXT: CODE COMMENTS, COMMIT MESSAGES, DOCS, AND THIS PLAN. SHORT SENTENCES. COMMON WORDS.

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
| 2 - Configuration | NEXT | - |
| 3 to 11 | TODO | - |

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
3. **Read promotion hook point.** `DUMP`, `DEBUG DIGEST`, and migration also
   use `lookupKeyRead*`. They must not promote (section 7.4). So promotion can
   not live inside a generic lookup. Decision needed: a new
   `LOOKUP_PROMOTE` flag, or a separate helper called only from client read
   commands. Recommended: a separate helper `getStringValueForRead()` called
   from string read commands, plus a generic fallback in `getDecodedObject()`
   that only decompresses to a temporary object.
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
8. **Unit tests on macOS.** `make -C src test-unit` fails with Apple clang,
   because Homebrew `llc` can not read Apple LTO objects. It works with
   `CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++`
   after `make -C src distclean`.

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

## Phase 2 - Configuration

Goal: configs exist and are validated. Nothing compresses yet.

- `src/config.c`:
  - `compression-mode` startup only (`IMMUTABLE_CONFIG`). Accept only `off`
    and `lz4`; reject anything else, `zstd` included, until Phase 7 adds the
    backend.
  - `compression-threads` 0-16, default 1, startup only.
  - `compression-min-value-size` default 256, runtime.
  - `compression-max-value-size` 1-131072, default 131072, runtime, must be
    `>= min`.
    The upper bound is `COMPRESSOR_FRAME_MAX_VALUE_LEN`, not a second literal.
    A value above it is rejected with an error, never clamped.
  - `compression-dict-size` default 102400, startup only.
  - Open: `mode != off` with `threads == 0`. Recommended: allow it. It
    means no background sweeper; inline compression (Phase 9), reads, and RDB
    load still work on the main thread. Document this in `valkey.conf`.
- `src/server.h`: hardcoded v1 constants for the "advanced (v2)" settings
  (`COMPRESSION_MAX_INFLIGHT 100`, `MIN_SAVINGS_RATIO 10`,
  `MIN_IDLE_SECONDS 60`, `HOT_LRU_MAX 100000`, `LFU_THRESHOLD 5`,
  training constants, `DICT_MAX_VERSIONS 4`).
- Tcl test `tests/unit/compression-config.tcl`: defaults, ranges,
  `CONFIG SET` rejected for startup-only settings, min/max cross check.

## Phase 3 - `OBJ_ENCODING_COMPRESSED` and the read/egress paths

Goal: a compressed value can exist in the keyspace and every path handles it.
Frames are created only by a debug command in this phase.

- `src/server.h`: `#define OBJ_ENCODING_COMPRESSED 14`.
- The main thread's compressors (moved from Phase 2): one `compressorAlg`
  for each algorithm, only for the main thread. The main thread uses them to
  decompress. Each one is created the first time it is needed. So frames from
  RDB can be read even when the mode is `off`. Names:
  `main_thread_compressors[COMPRESSOR_ALG_MAX]` and
  `compressorGetForMainThread(algorithm_id)`.
- `src/object.c`:
  - `strEncoding()` returns `"compressed"`.
  - `decrRefCount()` / free path calls the backend `release()` and drops the
    dictionary user count (registry stub until Phase 6).
  - `MEMORY USAGE` / `objectComputeSize()` counts the frame allocation.
  - `getDecodedObject()` returns a temporary RAW copy for compressed values.
  - `sdsEncodedObject()` must stay false for compressed values. Audit every
    caller.
  - Active defrag (`src/defrag.c`): move the frame like any sds.
- Audit all direct reads of `obj->encoding` / `objectGetEncoding()` for
  `OBJ_STRING`. Put each site in one of the four classes of design section 7.1
  and record the list in the PR description.
- Read promotion helper (see "Facts" item 3): decompress, validate, replace
  the value in the db, release the frame, record the key as hot (no-op until
  Phase 4). On error keep the frame and reply with an error.
- Mutation path: `dbUnshareStringValue()` and every string write command
  (`APPEND`, `SETRANGE`, `SETBIT`, `INCR*`, `GETSET`, `GETDEL`, `GETEX`,
  `SET ... GET`) first get the logical RAW value.
- Temporary-decompress egress paths: `DUMP` (payload must be plain; decide if
  `RESTORE` payload format changes - recommended: no change), `DEBUG DIGEST`,
  `DEBUG DIGEST-VALUE`, slot migration export, any propagation that
  materializes a stored value, command-form AOF rewrite
  (`aof-use-rdb-preamble no`).
- RDB save in this phase: write compressed values as plain (temporary
  decompress). Phase 8 replaces this with as-is save.
- `DEBUG COMPRESS-KEY <key>` (sync, test only) to create frames.
- Tests:
  - Unit: object free / size with a frame.
  - Tcl `tests/unit/compression-encoding.tcl`: `OBJECT ENCODING`, every string
    read and write command on a compressed key, `DUMP`/`RESTORE`, `DEBUG
    DIGEST` equal before and after compression, `MULTI` with read then write,
    multi-key reads (`MGET`), keyspace notifications unchanged, replication
    and AOF output stays plain, `SAVE` + `DEBUG RELOAD` keeps data.

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
- In-flight table keyed by `(dbid, key)` with `{version, refs}`.
  - Version bump in `signalModifiedKey()` and `dbUnshareStringValue()`.
  - Global `keyspace_epoch` bumped by `FLUSHALL`, `FLUSHDB`, `SWAPDB`,
    full-sync load, `DEBUG RELOAD`.
- Job: owned copy of the value bytes (never a live pointer), recorded
  version and epoch, `dict_id`. The worker frees the input before it
  publishes the output.
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
- Drain completions on the main thread (cron or `beforeSleep`): check
  epoch, version, key still present, value still the same `robj`, then net
  savings guard, then install. Otherwise discard and count.
- Behavior during fork child, `loading`, `CLIENT PAUSE WRITE`, and on a
  replica (recommended: sweeper runs on replicas too; it is local memory).
- Keyspace notifications: none for install (it is not a logical change).
- `INFO compression` section: mode, threads, values compressed, bytes saved
  (allocation based), live and lifetime ratio, sampled entries, rejections by
  reason, magic-byte skips, in-flight, cap stops, stale jobs, guard
  discards, errors, hot-key counters.
- Tests:
  - Unit: in-flight table, version and epoch rules, magic-byte table.
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
- LZ4 dictionary bytes come from the zstd ZDICT trainer.
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
- Read promotion: p50/p99/p99.9 first-read and hot-read latency.
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
