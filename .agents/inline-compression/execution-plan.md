# Inline In-memory Compression — Execution Plan

Working plan for implementing `../../design-docs/inline-compression.md` in this
checkout. Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423).

This is an implementation plan, not a design. When a mid-level detail changes,
update `design-docs/inline-compression.md` and this file together. Background and
decision history: `context.md` in this folder.

---

## 0. Facts about this tree that shape the plan

Checked in the Valkey tree; see `context.md` for which worktree owns what:

1. **`src/compression.{c,h}` is already taken.** It holds stream compression for
   the RDB/replication byte stream: `streamCompressor`, `streamDecompressor`,
   `compressionAlgo` (`ALGO_NONE`/`ALGO_LZF`/`ALGO_LZ4`), plus
   `src/compression_lz4.{c,h}` and `src/compression_stream.{c,h}`. All three are
   listed in `src/Makefile:492-494`. That code is a different feature and must
   not be reused or renamed.
   → New code uses the `compressor_alg*` file prefix. The design's `compressor`
   vtable became `compressorApi`, the instance is `compressorAlg`, and the config
   is `compressorConfig`. See `src/compressor_alg.h`.
2. **`OBJ_ENCODING_COMPRESSED` = 12 fits.** `src/server.h:799-810` defines 0-11,
   and `struct serverObject` (`src/server.h:855`) stores `unsigned encoding : 4`,
   so 12-15 are free.
3. **There is no spare room on `robj`.** `static_assert(sizeof(struct
   serverObject) <= 8 + sizeof(void *))` at `src/server.h:866`. Any per-key
   compression state has to live in a side table, as the design assumes.
4. **The two version hooks exist.** `signalModifiedKey()` at `src/db.c:785` and
   `dbUnshareStringValue()` at `src/db.c:605`.
5. **zstd is not vendored.** `deps/` has `lz4` but no `zstd`, so the default
   mode needs a new vendored dependency.
6. **No config name clash.** `src/config.c` has `rdbcompression` and
   `list-compress-depth` only; no `compression-*` name is used.
7. **Tests come in two flavours:** gtest C++ units under `src/unit/`, and TCL
   integration tests under `tests/unit/`.

---

## 1. Read-path decision before coding

`inline-compression.md` §7 now chooses RAW promotion with a bounded hot-key
LRU. A client read of a compressed value decompresses it once, replaces the
frame with the RAW object, and records the key as recently read. Every later
read refreshes that entry. The sweeper skips entries in the hot-key LRU and
still requires the regular idle/LFU coldness check.

This choice needs no side-map, `beforeSleep` restoration, read counter, or
per-read temporary view. The hot-key LRU holds at most 100,000 entries and each
entry expires after `compression-min-idle-seconds`, 60 seconds by default. It
therefore measures recent activity instead of lifetime reads. A key can be
compressed again after it leaves the LRU and passes the coldness gate.

The frame header remains **12 bytes** and immutable: `uncompressed_len`,
`dict_id`, `algorithm_id`, and `format_version`, with no `read_streak`. Keep the
`static_assert` on `offsetof` for each field. An sds `buf` is not guaranteed to
be aligned for these fields, so build the header with a 12-byte `memcpy` and
never a struct cast.

The decompressed RAW object is installed in the keyspace before the reply is
built. Existing reply ownership then applies: copy avoidance may retain the
object with `incrRefCount()`, and a later write or delete releases the keyspace
reference without invalidating the queued reply.

---

## 2. Phases

Each phase should build, pass tests, and be reviewable on its own. Nothing
before phase 7 is reachable by a user, because `compression-mode` defaults to
`off`.

### Phase 1a — the backend interface and the LZ4 backend — DONE

Landed on `valkey-inline-compression-compressor-api`:

- `src/compressor_alg.h` — `compressorApi`, `compressorConfig`, `compressorAlg`,
  with the caller-allocates-destination and `cdict`-as-argument shapes intact.
- `src/compressor_alg.c` — `newCompressor()` and `freeCompressor()`.
- `src/compressor_alg_lz4.c` — the LZ4 backend, block API, dictionary digested
  once with `LZ4_loadDictSlow` and attached per call.
- Registered in `src/Makefile` and `cmake/Modules/SourceFiles.cmake`.
- `src/unit/test_compressor_alg.cpp` — 34 tests, all passing.

### Phase 1b — vendor zstd

**Goal:** the default mode works, and the trainer has a dictionary builder.

- `deps/zstd/` — vendor the library. Update `deps/Makefile`,
  `deps/CMakeLists.txt`, `LICENSES/`, and `REUSE.toml`.
- `src/compressor_alg_zstd.c` — the zstd backend. Add its `case` to
  `newCompressor()` and its table `extern` in `src/compressor_alg.c`.

**Do this before phase 2.** LZ4 has no dictionary trainer, so `train()` is NULL
in the LZ4 table. The trainer needs `ZDICT_trainFromBuffer` from zstd's
`dictBuilder`, whichever backend compresses. For LZ4, pass only the content part
of the trained dictionary, found with `ZDICT_getDictHeaderSize()`.

**Done when:** `src/unit/test_compressor_alg.cpp` covers zstd the same way it
covers LZ4, and a build with zstd absent still links when the mode is `off`, if
we choose to make zstd optional.

### Phase 2 — dictionary registry and the trainer

**Goal:** the process can hold up to `compression-dict-max-versions`
dictionaries, hand out the active one, and retire old ones by user count.

- `src/compressor_dict.{c,h}` — registry entries keyed by `dict_id`,
  refcount per entry, active pointer, retire-at-zero. No per-entry backend
  field (§4).
- Trainer: sample scan plus `train()`, run on a `bio` thread. Add a new job type
  to `src/bio.h` (`BIO_NUM_OPS` grows) and the handler in `src/bio.c`.
- Triggers: first reach of `compression-dict-min-training-keys`, drift ratio,
  optional interval.

**Done when:** unit tests show a retiring dictionary stays alive while frames
reference it and is freed at zero; a training run on a synthetic keyspace
produces a dictionary that beats no-dictionary compression on 256 B-1 KiB
values.

### Phase 3 — the frame, the new encoding, and the read-site audit

**Goal:** a compressed `robj` can exist and every existing reader handles it.
Still nothing creates one.

- `compressionFrame` build and parse helpers: single 12-byte `memcpy`, never a
  struct cast, named offset constants, `static_assert` on `offsetof`, shrink to
  real length after compressing, net-savings guard.
- `OBJ_ENCODING_COMPRESSED 12` in `src/server.h`; `"compressed"` in the
  `OBJECT ENCODING` switch in `src/object.c` (around `src/object.c:1313`).
- A client-read helper (§7.2) that receives database and key context. For a
  compressed value, decompress into a heap RAW object, replace the frame in the
  keyspace before replying, and record the key in the hot-key LRU. Plain reads
  refresh an existing hot-key entry. Use `objectGetVal`/`objectSetVal`/
  `objectSetEncoding` rather than touching fields.
- Existing reply ownership applies after promotion. Copy avoidance may retain
  the installed RAW object; a later write or delete only drops the keyspace
  reference.
- **The audit.** Walk every site that tests `objectGetEncoding()` or
  `OBJ_ENCODING_RAW` and sort it into the four buckets from §7.1:
  - client read -> promote to RAW and mark the key hot;
  - other plaintext egress -> use a temporary value for `cluster.c` (`DUMP`),
    replication materialization, and `debug.c` (`DEBUG DIGEST`);
  - preserve representation -> RDB save;
  - inspect or release only -> object free (calls `release()`), `MEMORY USAGE`,
    `allocator_defrag.c` / active defrag, and `OBJECT ENCODING`.
  `src/object.c` alone has ~10 `OBJ_ENCODING_RAW` sites; treat the audit as a
  tracked checklist, not a grep-and-hope.

**Done when:** a `DEBUG`-only path can compress one key by hand, and the full
TCL suite plus `DEBUG DIGEST` before/after comparison passes with that key
compressed. This is the phase where a missed site shows up as data corruption,
so it deserves the widest test net.

### Phase 4 — in-flight table and version counters

**Goal:** freshness, per §5.3.

- `inflightEntry` table keyed by `(dbid, key)`, entries only while work pends.
- Bump the version from `signalModifiedKey()` (`src/db.c:785`) and
  `dbUnshareStringValue()` (`src/db.c:605`).
- Global `keyspace_epoch` bumped by `FLUSHALL`, `FLUSHDB`, `SWAPDB`.
- Version compare on worker-result install - compare versions, never pointers (§6.3).

**Done when:** unit tests cover install-on-match and discard-on-mismatch; a TCL
test hammers one key with `SETBIT` while a job is pending and shows no stale
install.

### Phase 5 — worker pool and the owned snapshot

**Goal:** off-main-thread compression that cannot race the keyspace.

- `src/compressor_worker.c` — pool of `compression-threads`, job queue,
  `compression-max-inflight-requests` and `compression-inflight-max-bytes`
  caps, `compression_cpulist` pinning.
- Enqueue copies the bytes into a job-owned buffer (§5.2). The worker never
  touches the keyspace.
- Drain on the main thread: version check, then install or discard, then count.

**Done when:** unit tests for queue accounting and the byte cap; a TCL test
under `--accurate` runs a write-heavy load with the pool active and ends with a
clean `DEBUG DIGEST` against an uncompressed run.

### Phase 6 — the sweeper

**Goal:** find cold candidates with bounded main-thread work and queue backpressure.

- `compressionCron()` inspects at most
  `compression-sweep-max-keys-per-tick` keys per cron call, 100 by default, and
  preserves its cursor across calls.
- Enqueue only while queued plus running jobs are below
  `compression-max-inflight-requests`, 100 by default, and owned snapshots stay
  below `compression-inflight-max-bytes`, 32 MiB by default. Stop on either cap
  and resume after completions release capacity.
- Eligibility per §6.1: active dictionary, `OBJ_STRING`, `OBJ_ENCODING_RAW`,
  `refcount == 1`, size window, absent from the hot-key LRU, and cold by idle
  time or LFU.
- A bounded 100,000-entry hot-key LRU. Every client read inserts or refreshes
  the key; entries expire after `compression-min-idle-seconds`, and the
  sweeper skips matches before copying candidate bytes.
- Cursor that survives keyspace changes, resize, and `FLUSHALL`.

**Done when:** unit tests prove that one cron call never inspects more than the
configured key count, no job is accepted above either in-flight cap, and
capacity is restored after completion. A TCL test fills cold data, waits, and
shows `INFO compression` savings rising; another reads a compressed key, shows
that it becomes RAW, and confirms repeated reads do not cause decompression or
recompression churn while its hot-key entry is live.

### Phase 7 — configuration, INFO, commands

- Five primary configs from §8. `compression-mode`, `compression-threads` and
  `compression-dict-size` are read once at startup, and `CONFIG SET` against
  them is rejected — follow the `*_cpulist` pattern in `src/config.c`.
- The 15 advanced settings stay **hardcoded** in v1 (§8 "Advanced settings
  (v2)"). Put each default in one named constant so exposing it later is a
  one-line change.
- `INFO compression`: savings, live and lifetime ratio, keys inspected,
  in-flight requests and bytes, request-cap and byte-cap backpressure stops,
  stale jobs, training state, errors.
- `COMPRESSION TRAIN` for manual retraining, and the subcommand shape in
  `src/commands/`.
- `valkey.conf` documentation for the five primary settings.

**Done when:** `tests/unit/introspection.tcl` style checks confirm the three
startup-only settings reject `CONFIG SET`, and `CONFIG GET compression-*`
returns only the v1 set.

### Phase 8 — persistence and transfer tests

These protect the representation boundaries in §3 and §4.

- RDB save/load preserves plain and compressed values as-is, persists every
  referenced dictionary, and rejects a missing dictionary, unsupported
  algorithm, unknown frame version, or malformed frame.
- `DUMP`/`RESTORE` round-trip across compressed and uncompressed instances.
- Full sync from a compressed primary to a replica with compression off, and
  the reverse; the RDB baseline preserves frames and buffered commands remain
  logical and uncompressed.
- AOF rewrite with `aof-use-rdb-preamble yes`: the RDB base preserves compressed
  frames and dictionaries; incremental AOF files replay uncompressed commands.
- AOF rewrite with `aof-use-rdb-preamble no`: the base contains uncompressed
  logical commands, loads without compression dictionaries, and does not change
  the primary's in-memory representation during rewrite.
- A mixed multipart AOF restores compressed values from its RDB base and then
  applies uncompressed updates from incremental files.
- A corrupted or unsupported compressed frame in an RDB-format AOF base fails
  with the same checks as direct RDB load.
- `MIGRATE` between compressed and uncompressed nodes sends logical values and
  leaves the source representation unchanged.

**Done when:** all cases live in `tests/unit/compression.tcl` and
`tests/integration/`, both AOF preamble settings pass rewrite and restart tests,
and the existing suite passes with `compression-mode` forced on.

### Phase 9 — measurement

Two jobs: prove the targets, and validate the hot-key promotion policy.

- Memory saving on a JSON-like data set, target >=30%.
- TPS cost under mixed read/write load, target under 20%.
- First-read promotion latency by value size, and the effect of the 128 KiB cap.
- Steady-state latency for repeated reads after a value becomes RAW.
- Hot-key LRU lookup cost, memory, expiry, and capacity churn with working sets
  below and above 100,000 keys.
- Sweeper convergence time on a large keyspace, and recovery time after a wide
  read pass promotes many values.

**Done when:** the numbers are in the issue and either confirm the 100,000-entry
bound or justify a measured replacement.

---

## 3. Risk list, worst first

| Risk | Why it hurts | Guard |
|---|---|---|
| A missed `obj->encoding` read site | Silent wrong data returned to a client | Phase 3 checklist, `DEBUG DIGEST` equivalence in every later phase |
| Frame header alignment or offset bug | An sds `buf` is not 4-byte aligned, so a `uint32_t *` cast onto it is undefined behavior | Named offsets, `static_assert` on `offsetof`, single `memcpy`, no struct cast |
| Hot-key LRU is too small | Entries churn out and active keys are compressed, promoted, and compressed again | Keep the idle/LFU gate, report capacity evictions and sweeper skips, and test working sets above 100,000 keys |
| Promotion breaks reply ownership | A later write could free bytes still queued for a copy-avoided reply | Install a normal heap RAW object before replying and rely on the existing object refcount path |
| A wide read pass promotes much of the keyspace | Memory can move toward the uncompressed baseline until untouched values cool and are swept again | Bound read size, track promotion memory, and measure recompression convergence |
| Vendoring zstd | New dependency, licence and build-matrix work | Phase 1 alone; keep the LZ4 path buildable without zstd |
| Name collision with the existing stream compression code | Confusing review, accidental reuse | `compressor_alg*` files, `compressorApi` / `compressorAlg` types |
| Forgetting to shrink the frame after compress | Reported savings exceed real savings | Assert real length <= bound and shrink in the one build helper |

---

## 4. Suggested pull-request split

One PR per phase, in order, except that phase 1 can land in parallel with the
phase 3 audit checklist.

Phases 1-6 land with `compression-mode` still absent from `config.c`, so the
feature is unreachable until phase 7. That keeps every intermediate commit safe
to ship.
