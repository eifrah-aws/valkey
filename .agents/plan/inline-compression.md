USE SIMPLE ENGLISH (CEFR B1) IN ALL TEXT: CODE COMMENTS, COMMIT MESSAGES, DOCS, AND THIS PLAN. SHORT SENTENCES. COMMON WORDS.
BEFORE YOU START CODING, EXPLAIN THE PLAN AND GET A CLEAR "YES" FROM THE USER. DO NOT START CODING WITHOUT IT.

# Inline In-memory Compression - Execution Plan

Source design: `design-docs/inline-compression.md` (in this repo; the old copy in `valkey-inline-compression-design` is no longer used)
Issue: valkey-io/valkey #3423
Branch: `valkey-inline-compression`

Each phase below is one PR. Each PR must build with no warnings (Make and
CMake), pass the unit tests with `gtest-parallel` (see "Cross-cutting
rules"), and pass the relevant Tcl tests. Each PR must keep the
feature unreachable or safe when `compression-mode off` (the default).

---

## Status

All commits below are pushed to `origin/valkey-inline-compression` (the
`eifrah-aws/valkey` fork).

| Phase | State | Commit |
|---|---|---|
| 0 - Compressor interface and LZ4 backend | DONE, pushed | `89e80d243` |
| 1 - Compression frame | DONE, pushed | `e37891511` |
| 2 - Configuration | DONE, pushed | `cc4fbb5fa` |
| 3a - Full cycle: encoding, read path, background workers | DONE, pushed | `eba2a8ffd` |
| Design doc moved to `design-docs/` and aligned | DONE, pushed | `f28ce0cb1` |
| Fix the Linux build (`pthread_setname_np`) | DONE, pushed | `dd894985d` |
| INFO compression (part of Phase 5) | DONE, pushed | `14df64e73` |
| 5 - Background sweeper | PARTLY DONE (see Phase 5) | `eba2a8ffd`, `14df64e73` |
| 3b - Other read paths | NEXT | - |
| 4 to 11 | TODO | - |
| Off-loading: decompress in the I/O thread | IDEA, after Phase 11 | - |

## Phase 0 - Compressor interface and LZ4 backend (DONE, pushed)

Commit `89e80d243`. Files moved to `src/compressor/` in Phase 1.

- `src/compressor/compressor_alg.h`, `src/compressor/compressor_alg.c`,
  `src/compressor/compressor_alg_lz4.c`.
- `src/unit/test_compressor_alg.cpp` (34 tests now).
- LZ4 has no trainer, so `compressorApi.train` is NULL for LZ4.

---

## Facts found in the code that change the design

Items 1 and 2 are now fixed in `design-docs/inline-compression.md`. The others
are decided, or are open until the matching phase.

1. **Encoding number.** The design says `OBJ_ENCODING_COMPRESSED` is 12. In
   `src/server.h`, 12 is `OBJ_ENCODING_LISTPACK2` and 13 is
   `OBJ_ENCODING_PATH_HASH`. The `encoding` field is 4 bits
   (`src/server.h:888`), so only 14 and 15 are free (3, 4, 5 are marked
   "No longer used" and could be reused). We use **14** (done in 3a).
   Decision: one generic `OBJ_ENCODING_COMPRESSED` for every type and every
   algorithm. `robj->type` gives the object kind. The frame header gives the
   algorithm (`algorithm_id`) and the encoding to rebuild (`original_encoding`).
   No per-type or per-algorithm encodings.
2. **Net-savings guard.** Appendix A shows that the guard must compare
   allocation sizes (`zmalloc_size` / `sdsAllocSize`), not byte lengths. Done
   in the code (`compressorFrameSavesMemory()`) and in design section 6.4.
3. **Where a read decompresses (decided: in `lookupKey()`).** When
   `lookupKey()` in `src/db.c` finds a compressed string, it decompresses it,
   stores the plain value in the keyspace, and returns it. During a child
   process, a read gets a temporary plain copy instead (see 3a). Commands do not
   change, so no command can get a frame by mistake. Modules are covered too
   (`RM_StringDMA()`).
   - New flag `LOOKUP_NODECOMPRESS`: return the value as it is, compressed or
     not. Callers that use it must handle a compressed value.
   - `LOOKUP_NOTOUCH` does not imply `LOOKUP_NODECOMPRESS`. A module can open
     a key with NOTOUCH and still read its bytes.
   - Done in 3a: `OBJECT` and `DEBUG OBJECT` use `LOOKUP_NODECOMPRESS`
     (`objectCommandLookup()`). `MEMORY USAGE` and `DEBUG DIGEST` do not use
     `lookupKey()`, so they do not change the value.
   - Left for 3b: `DUMP`, `MIGRATE` (`src/cluster.c`), `DEBUG DIGEST-VALUE`
     (they need a plain copy), and `TYPE`, `EXISTS`, `TTL`/`PTTL` (they do
     not read the bytes).
   - Cost: commands like `TOUCH` and `RENAME` also decompress. This is safe.
     The sweeper compresses the key again later.
   - RDB save, AOF rewrite, slot migration export, and the sweeper do not use
     `lookupKey()`. They read the keyspace directly and must handle
     compressed values themselves.
4. **Name clash.** `src/compression.{c,h}` is the RDB / replication stream
   compression. Keep the `compressor_*` prefix for all new files. `INFO
   compression` does not clash with any other INFO section (checked). Check
   the `COMPRESSION` command name in Phase 7.
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

## Phase 1 - Compression frame (DONE, pushed)

Commit `e37891511`. No keyspace change.

- `src/compressor/compressor_frame.{h,c}`.
  - 10-byte header (the first design said 12): `uint32_t uncompressed_len`,
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

## Phase 2 - Configuration (DONE, pushed)

Commit `cc4fbb5fa`.

Configs exist and are checked. Nothing compresses yet.

- New `src/compressor/compressor_config.{h,c}`:
  - The limits and defaults of the settings.
  - The fixed v1 values of the "advanced (v2)" settings
    (`COMPRESSOR_MAX_INFLIGHT_REQUESTS`, `COMPRESSOR_MIN_SAVINGS_PCT`, ...).
  - `compressorConfigCheck()`: min value size must not be greater than max,
    and (since 3a) `compression-mode` can not be used with
    `forkless-infrastructure-enabled yes`. It runs at startup (in
    `loadServerConfigFromString()`) and as the apply function of CONFIG SET.
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

## Phase 3a - Full cycle: encoding, read path, background workers (DONE, pushed)

Commit `eba2a8ffd`. Linux build fix in `dd894985d`.

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
- `objectCommandLookup()` (OBJECT, DEBUG OBJECT) sets `LOOKUP_NODECOMPRESS`.
  `MEMORY USAGE` uses `dbFind()`, so it does not decompress.
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
  (4 tests), two new tests in `tests/unit/moduleapi/scan.tcl`, and a new
  test module `tests/modules/compression.c` with
  `tests/unit/moduleapi/compression.tcl` (4 tests).
- Not yet: the hot-key list (Phase 4), the magic-byte skip table and the
  dictionary rule (Phase 5). Sampling does not skip importing hashtables yet
  (design section 6.1). `INFO compression` came later, in `14df64e73`.

## INFO compression (DONE, pushed; part of Phase 5)

Commit `14df64e73`.

- `src/compressor/compressor_stats.{h,c}`: the 27 fields of design doc
  section 9.1. `compression` is a non-default INFO section.
- `canCompress()` sets `EALREADY` for a compressed value, so
  `keys_skipped_already_compressed` counts it on its own (review finding).
- `freeStringObject()` updates the gauges only when the object still holds
  its frame. `objectSetKeyAndExpire()` moves the frame to a new object
  (review finding).
- Gauges (`compressed_values*`) are relaxed atomics, because `FLUSHALL
  ASYNC` frees values on a background thread. `freeStringObject()` calls
  `compressorStringObjectFreed()`.
- `CONFIG RESETSTAT` resets the counters, not the gauges. Gauges and counters
  are two structs, so the reset never writes the gauges (review finding).
- Phase 8 (RDB load of frames) must add loaded frames to the gauges.
- Tests: `tests/unit/compression-info.tcl` (11 tests), including checks that
  the counters add up.
  `src/unit/test_compressor_stats.cpp` (4 tests).

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

Status: PARTLY DONE.

- Done in 3a (`eba2a8ffd`): the worker pool (two `mutexQueue`s), the sampler
  in `compressorCron()`, the coldness test, the in-flight set, the byte
  compare at install, the pause during a child, and the 64-bit random number.
  Locking the key was rejected, because it adds latency to client writes
  (design section 6.3.1).
- Done in `14df64e73`: `INFO compression`.

Left:

- Magic-byte skip table (design section 6.1). A `memcmp` on the first bytes
  only. Count the skips in `INFO compression`.
- Dictionary rule (decided):
  - `zstd`: do not compress until the first dictionary exists. The first
    training starts at `COMPRESSOR_DICT_MIN_TRAINING_KEYS` keys. Reason:
    without a dictionary small values save little, these frames are never
    compressed again with the new dictionary, and training needs plain
    sample values.
  - `lz4`: compress without a dictionary (`dict_id` 0) from the start (as
    today).
  - Count values skipped while waiting for a dictionary in
    `INFO compression`.
- Skip importing hashtables when sampling (slot import).
- Check the behavior during `CLIENT PAUSE WRITE`, and on a replica
  (recommended: the sweeper runs on replicas too; it is local memory).
- Keyspace notifications: none for install (it is not a logical change).
  Add a test.
- Tests still missing: a write while its job runs (the frame is dropped);
  `FLUSHALL` while a job runs; values in an already-compressed format are
  skipped (after the skip table).

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

- Use zstd through `BUILD_ZSTD` (see "Facts" item 6), not a new folder in
  `deps/`. Open question: the default mode `zstd` then needs
  `BUILD_ZSTD=yes`.
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
- Load: rebuild the registry before any value, check every frame
  (`compressorFrameParse()` and dictionary present), rebuild user counts.
  Any error fails the load.
- Loaded frames must be added to the `INFO compression` gauges
  (`compressorStatsAddValue()`), because they do not go through
  `compressorSetCompressedValue()`.
- Loaded values must keep their LRU/LFU data, so they are not cold at once
  only because they were loaded (check).
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
- `INFO compression` field list: already in design doc section 9.1. Add it
  to the user docs.
- `COMPRESSION` command docs.
- Design doc: moved to `design-docs/inline-compression.md` and aligned with
  the code up to `INFO compression` (encoding 14, 10-byte header,
  allocation-size savings check, byte compare, decompress in `lookupKey()`,
  plain copies during a child and for module read handles, section 9.1).
  Keep it in step with each later phase.

## Off-loading: decompress in the I/O thread (IDEA, after Phase 11)

### The problem

We ran a test. 750,000 keys were compressed. Then a client read every key
with `GET`.

- Before the test: a `GET` takes 0.5 ms at p99.
- During the test: a `GET` takes 2.4 ms at p99. The server does 13% fewer
  requests per second.
- After the test: all keys are plain again. `GET` is fast again.

Why? The main thread does all the work for a command. When the value is
compressed, the main thread must also unpack it. We measured it with
`INFO compression`: `decompression_time_us / values_decompressed` is about
1.5 microseconds per key. At 1.2 million requests per second, the main thread
has only 0.8 microseconds for one command. So 1.5 extra microseconds is a
lot. The commands wait in a line, and the wait time goes up.

Each key pays this cost only one time. After the first read, the value is
plain. But when many keys are compressed, the first reads are slow for a
while.

### The idea

Let the I/O thread unpack the value, not the main thread.

The I/O threads are the threads that send the answer to the client. There
are many of them. They have free time. The main thread has no free time.

Today, when a `GET` returns a big value, the main thread does not copy the
value. It writes a small note in the answer buffer. The note says: "send the
bytes of this object". The I/O thread reads the note and sends the bytes.
This note is called `bulkStrRef` in `src/networking.c`.

Every note in the answer buffer has a small header with a type. Today there
are two types:

- `PLAIN_REPLY`: normal bytes. Send them as they are.
- `BULK_STR_REF`: a note. Send the bytes of the object in the note.

We add a third type:

- `COMPRESSED_STR_REF`: a note. The object holds a compressed value. Unpack
  it first, then send the plain bytes.

### How it works, step by step

1. A client sends `GET key`. The value is compressed.
2. The main thread does not unpack the value. It writes a `COMPRESSED_STR_REF`
   note with a pointer to the object. It also adds 1 to the object's
   reference count, so the object stays alive until the answer is sent.
   This is what `BULK_STR_REF` does today too.
3. The I/O thread reads the note. It sees the type `COMPRESSED_STR_REF`.
4. The I/O thread reads the first 10 bytes of the compressed value (the frame
   header). The header holds the plain size. So the I/O thread can write
   `$<size>\r\n` with no unpacking.
5. The I/O thread unpacks the value into its own buffer. Each I/O thread has
   its own buffer and its own LZ4 unpacker. They do not share anything.
6. The I/O thread sends the plain bytes and `\r\n`.
7. When the answer is sent, the main thread takes 1 from the reference count,
   as today.

The main thread does almost no work for the value. The value in the database
stays compressed. The memory saving stays.

### How the I/O thread knows the value is compressed

Only from the type in the note. The main thread checks the object encoding
and picks the type. The I/O thread does not look at the object encoding. It
never touches the database. This is the same rule as today: I/O threads
only read what the main thread puts in the answer buffer.

### Why it is safe to read the compressed value from the I/O thread

A compressed value can change in two ways: a write unpacks it in the
database, or the key is deleted. Both happen on the main thread. Both must
wait until no one else holds the object. The answer holds the object (the
reference count is above 1). So the compressed bytes do not change until the
answer is sent.

One thing must change for this. Today a write (`LOOKUP_WRITE`) unpacks the
value in place, even when the reference count is above 1. With this plan,
that is not safe: the I/O thread may still read the old bytes. The rule
becomes: if the reference count is above 1, make a new plain object and put
it in the database. Do not change the shared one. `dbUnshareStringValue()`
already does this kind of copy for other cases.

### Hot keys: unpack in the background after the first read

If a key is read many times, we do not want to unpack it on every read. We
want it plain in the database, like today.

So after the first read, the main thread also gives a small job to a worker
thread: "unpack this key". The worker gets a copy of the compressed bytes
(small, about 500 bytes for a 2 KB JSON value). It unpacks them. Then
`compressorBeforeSleep()` puts the plain value in the database, but only if
the key still holds the same compressed bytes. This is the same check that
`installJob()` does today, in the other direction.

The second read finds a plain value. No unpacking anywhere.

We need a rule for when to give this job. Not every read. For example: on
the second read, or when the LFU counter goes above
`COMPRESSOR_LFU_THRESHOLD`. The sweeper already uses the same test the other
way.

### Limits

- Without I/O threads, the unpacking happens in `writeToClient()`, on the
  main thread. No gain, no loss.
- The I/O thread needs a buffer for the plain bytes. The bytes must live
  until `writev()` is done. One buffer per I/O thread, reused for each write
  call, is enough. If a write is cut in two (short write), the I/O thread
  unpacks again on the next write. This is simple and cheap.
- Only `GET` and commands that return the whole value as one bulk string.
  Commands that read the bytes (`GETRANGE`, `STRLEN` is fine, `APPEND`,
  `INCR`, ...) still unpack on the main thread, as today.

### Work list

- `src/networking.c`: new type `COMPRESSED_STR_REF`. New function like
  `_addBulkStrRefToBufferOrList()` for compressed objects. In
  `addEncodedBufferToReplyIOV()`, handle the new type: read the plain size
  from the frame header, unpack into the thread's buffer, add to `iov`.
- `src/io_threads.c`: one unpacker and one buffer per I/O thread.
- `src/compressor/compressor_object.c`: a write on a shared compressed object
  makes a new plain object instead of changing the shared one.
- `src/compressor/compressor_workers.c`: a second job type, "unpack this
  key", and the install step in `compressorBeforeSleep()`.
- `src/db.c` / `compressorLookupValue()`: the rule for when to queue the
  unpack job.
- `INFO compression`: count answers sent from a compressed value, and unpack
  jobs queued, installed, and dropped.
- Tests: Tcl test with I/O threads on and off. `GET` on a compressed key
  returns the right bytes. The key stays compressed after one read. The key
  is plain after the second read. A write during a pending unpack job is not
  lost. Unit test for the new reply type in `src/unit/test_networking.cpp`.

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
