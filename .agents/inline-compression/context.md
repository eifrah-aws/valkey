# Inline Compression — Working Context

Notes for anyone (human or agent) picking this work up later. Written 2026-09-10.

Feature: inline in-memory value compression for Valkey.
Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423).

**The work is split across two branches**, each in its own git worktree of the
Valkey repo. Find them with `git worktree list`.

| Branch | Holds |
|---|---|
| `inline-compression-design` | the design document and these notes |
| `valkey-inline-compression-compressor-api` | the compressor interface, the LZ4 backend, and its tests |

Run every command from the worktree that owns the files you are changing. Do not
work in the main checkout, the one on `unstable`.

**Every path in these notes is relative to the repo root**, which is the top of
whichever worktree you are in. Do not write absolute paths here. Worktrees get
moved and renamed, and absolute paths go stale the moment that happens.

An earlier branch, `inline-compression`, held everything in one place. Its commits
were cherry-picked into the two branches above, so it is no longer needed.

---

## 1. Where the documents live

**Rule: a change to the design lands in all three of these, in the same sitting.
Never one of them alone.** They drift the moment you skip one, and the drift is
silent because nobody reads all three at once.

| # | Source | Where |
|---|---|---|
| 1 | Pippin: "Design Summary" | projectId `o7J4WQhgTEQD`, designId `Fr7Yvs4QI4n0` |
| 2 | Pippin: "Why We Are Building This" | projectId `o7J4WQhgTEQD`, designId `nDi5hlEIYah3` |
| 3 | Markdown | `design-docs/inline-compression.md` for the design, `.agents/inline-compression/why-1pager.md` for the why |

Source 3 is two files because sources 1 and 2 are two documents. Each Pippin
artifact has exactly one markdown twin:

- "Design Summary" pairs with `design-docs/inline-compression.md`.
- "Why We Are Building This" pairs with `.agents/inline-compression/why-1pager.md`.

A fact that appears in both documents, such as the RDB rule, has to be changed in
all four files. Do not create a fifth copy.

Use `pippin_update_artifact` with `updateType: string_replace`, then read the
artifact back and check the stored text. A write that returns a new version number
is not proof the right text landed.

`execution-plan.md` sits next to this file. It is an implementation plan, not a
design, so it has no Pippin twin, but it also goes stale and needs the same care.

**Retired, do not edit:**

- `~/work/GeneralTalk/Inline_Compression/merged-design.md` — the older,
  longer "full design". Frozen. It still contains sections that the design
  document has since dropped, so treat it as history only.
- Its Pippin twin: projectId `0eHuRkP0FqRn`, designId `XFOlIJDWemyW`. Frozen.

`design-docs/inline-compression.md` is the file that used to be
`design-summary.md` in the old `Inline_Compression` folder. It was moved and
renamed into this repo.

The old `~/work/GeneralTalk/Inline_Compression` folder holds only the retired
document. It sits outside the repo, which is the one place these notes cannot use
a relative path.

---

## 2. Design decisions already settled

These were argued out and closed. Do not reopen them without a new reason.

1. **Stored versus logical persistence is format-specific.** RDB and an
   RDB-format AOF base preserve plain and compressed values as-is, including
   frame metadata and referenced dictionaries. Command-form AOF bases,
   incremental AOF files, replication commands, `DUMP`, and `MIGRATE` carry
   logical uncompressed bytes.
2. **One switch, and it names the algorithm.** `compression-mode` takes `off`,
   `lz4`, or `zstd`. There is no separate `compression-alg`. An operator cannot
   express "compression on, no backend chosen".
3. **Three settings are startup-only:** `compression-mode`,
   `compression-threads`, `compression-dict-size`. `CONFIG SET` against them is
   rejected, like Valkey's `*_cpulist` settings.
4. **No drain phase and no runtime mode transitions.** The configured mode
   controls new compression for the process lifetime. Persisted frames survive
   RDB and RDB-format AOF restore, so startup also loads the decoder contexts
   and dictionaries referenced by those frames.
5. **One encoder backend, potentially several decoder backends.** New frames use
   the startup-selected mode. Persisted frames record their algorithm and format,
   so decoder contexts for older algorithms remain available until those frames
   disappear. `dict_id` picks a dictionary version within the recorded backend.
6. **No separate sweeper on/off switch.** An earlier draft had
   `compression-automatic-sweeper` with `enabled`/`disabled`. It was removed
   because it allowed a silent no-op state (`mode=zstd` plus
   `sweeper=disabled`). The sweeper now runs whenever `compression-mode` is not
   `off`.
7. **The 15 advanced settings are v2.** In v1 they are hardcoded defaults, not
   `CONFIG` names. Reason: none of the numbers behind them are backed by measured
   tests yet.
8. **Removed from the design on purpose:** the cost estimate section, the "what
   is honestly still open" section, the one-slide summary, and the "not in v1"
   list. The stated rule was: do not report numbers we cannot back with concrete
   tests.
9. **The worker gets an owned snapshot, not a live pointer.** Copy-on-write by
   convention was rejected: one missed call site is a use-after-free. Snapshot
   gives memory safety, the version counter gives freshness — both are needed.
10. **Version comparison, never pointer comparison.** A same-size in-place write
    such as `SETBIT` can reuse the same allocation, so pointers can be equal
    while content differs.

### Where the sweeper runs

The sweeper runs in `compressionCron` on the cron tick. It does not use a CPU
percentage or wall-clock budget. Each call inspects at most
`compression-sweep-max-keys-per-tick` keys, 100 by default, and preserves its
cursor for the next tick. It enqueues only while both
`compression-max-inflight-requests` (100) and
`compression-inflight-max-bytes` (32 MiB) have room. It stops on either cap and
resumes after workers release capacity. It does **not** run in `beforeSleep`.
Client reads promote compressed values to RAW immediately on the main thread;
there is no transient decompressed view to restore.

---

## 3. Read-path decision

A client read promotes a compressed value to RAW and adds the key to a bounded
100,000-entry hot-key LRU. Every later read refreshes the entry. Entries expire
after `compression-min-idle-seconds`, 60 seconds by default, and the sweeper
also requires the normal idle/LFU coldness check. This avoids decompression on
every hot read and prevents immediate recompression after promotion.

The remaining item to validate is the workload tradeoff: a broad one-time scan
can promote many values and temporarily return memory toward the uncompressed
baseline. Measure hot-key LRU coverage, capacity churn, promotion memory, and
cold-to-hot transitions before changing the 100,000-entry bound.

---

## 4. Facts about this checkout

The execution plan carries the full list with line numbers. The four that matter
most:

1. **`src/compression.{c,h}` is already taken** by stream compression for the
   RDB/replication byte stream (`streamCompressor`, `streamDecompressor`,
   `compressionAlgo` with `ALGO_NONE`/`ALGO_LZF`/`ALGO_LZ4`), together with
   `src/compression_lz4.{c,h}` and `src/compression_stream.{c,h}`. Different
   feature. New code uses the `compressor_alg*` file prefix. The design's
   `compressor` vtable became `compressorApi`, the instance is `compressorAlg`,
   and the config is `compressorConfig`. See `src/compressor_alg.h`.
2. **`OBJ_ENCODING_COMPRESSED 12` fits.** `src/server.h:799-810` defines 0-11 and
   `encoding` is a 4-bit field (`src/server.h:857`).
3. **No spare room on `robj`.** `static_assert(sizeof(struct serverObject) <= 8 +
   sizeof(void *))` at `src/server.h:866`, so side tables are forced.
4. **zstd is not vendored.** `deps/` has `lz4` only, so the default mode adds a
   vendored dependency.

Also: the two version hooks the design relies on already exist —
`signalModifiedKey()` at `src/db.c:785` and `dbUnshareStringValue()` at
`src/db.c:605`. No `compression-*` config name is in use yet.

---

## 5. Conventions to follow

**ALWAYS use English at CEFR B2 level.** This rule has no exceptions. It covers
every piece of text produced for this feature: the design document, this file,
the execution plan, code comments, commit messages, pull request descriptions,
issue comments, the Pippin artifact, and chat replies.

- **Prose:** short sentences, common words, active voice, one idea per sentence.
  Avoid rare or literary words. Avoid idioms and figures of speech.
- **More than 3 parameters means one parameter per line.** Any function,
  prototype, or function-pointer field with 4 or more parameters is written with
  each parameter on its own line, aligned under the opening bracket. Three or
  fewer stay on one line. `src/.clang-format` sets `ColumnLimit: 0`, so
  `clang-format` keeps these breaks and will not join the lines again. Example:

  ```c
  size_t (*compress)(void *ctx,
                     void *cdict,
                     const void *src,
                     size_t srclen,
                     void *dst,
                     size_t dstcap);
  ```
- **Never trade accuracy for simpler wording.** Keep identifiers, paths, line
  numbers, commands and quoted code exact. Never drop a caveat, an error message
  or a test result to shorten text.
- **Do not report numbers we cannot back with tests.** This rule is why several
  sections were deleted from the design.
- **Inclusive language:** primary/replica, allowlist/denylist. No
  master/slave/whitelist/blacklist anywhere in code, comments or docs.
- **Design document scope**, per `design-docs/README.md`: no API details, no
  low-level implementation details, no rejected alternatives, no future-work
  sections. Those belong in code comments, in the issue, or in the execution plan.
- **Editing the Pippin artifact:** use `pippin_update_artifact` with
  `updateType: string_replace` against the exact stored content. Tables are
  stored as literal HTML with `data-id` attributes on `<table>`, `<tr>` and
  `<td>`, not as markdown, so a table edit usually means one call per cell. Each
  successful edit bumps the artifact version.
- **Shell working directory does not persist** between Bash tool calls in this
  environment; it resets after each call. Use absolute paths, or prefix each
  command with `cd <worktree> && ...`.

---

## 6. Suggested first move when resuming

Read `design-docs/inline-compression.md`, then `execution-plan.md`.

Phase 1a is done: `src/compressor_alg.h`, `src/compressor_alg.c`,
`src/compressor_alg_lz4.c`, and `src/unit/test_compressor_alg.cpp` are on the
`valkey-inline-compression-compressor-api` branch.

Next is phase 1b, vendoring zstd. It does not depend on the open §6.3 decision,
and phase 2 needs it, because the trainer needs zstd's dictionary builder even
when LZ4 does the compressing.
