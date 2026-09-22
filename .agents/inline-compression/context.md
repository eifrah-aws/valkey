# Inline Compression — Working Context

Notes for anyone (human or agent) picking this work up later. Written 2026-09-10.

Feature: inline in-memory value compression for Valkey.
Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423).

**Working directory for all work on this feature:**
`/Users/eifrah/devl/valkey-inline-compression`

This is a git worktree of `/Users/eifrah/devl/valkey`, on branch
`inline-compression`. Run every command from here. Do not `cd` into the main
`valkey` checkout. This file lives at
`.agents/inline-compression/context.md` inside that worktree.

---

## 1. Where the documents live

Only two places hold the design. Keep both in sync; do not create more copies.

| What | Where |
|---|---|
| Design document | `design-docs/inline-compression.md` in this repo |
| Pippin artifact | projectId `o7J4WQhgTEQD`, designId `Fr7Yvs4QI4n0` — https://pippin.amazon.dev/architect/o7J4WQhgTEQD/inline-in-memory-compression-for-valkey-design-summary?artifact=Fr7Yvs4QI4n0 |
| Execution plan | `execution-plan.md` next to this file |

**Retired, do not edit:**

- `/Users/eifrah/work/GeneralTalk/Inline_Compression/merged-design.md` — the older,
  longer "full design". Frozen. It still contains sections that the design
  document has since dropped, so treat it as history only.
- Its Pippin twin: projectId `0eHuRkP0FqRn`, designId `XFOlIJDWemyW`. Frozen.

`design-docs/inline-compression.md` is the file that used to be
`design-summary.md` in the old `Inline_Compression` folder. It was moved and
renamed into this repo.

All paths in the table above are relative to the working directory named at the
top of this file: `/Users/eifrah/devl/valkey-inline-compression`. The old
`/Users/eifrah/work/GeneralTalk/Inline_Compression` folder holds only the retired
document.

---

## 2. Design decisions already settled

These were argued out and closed. Do not reopen them without a new reason.

1. **Nothing compressed leaves the process.** RDB, AOF, full sync, `DUMP`,
   `MIGRATE` all carry plain bytes. No `RDB_VERSION` bump, no dictionary
   serialization, no replica capability negotiation, no untrusted frames. This is
   the most valuable invariant in the design and it has its own test phase.
2. **One switch, and it names the algorithm.** `compression-mode` takes `off`,
   `lz4`, or `zstd`. There is no separate `compression-alg`. An operator cannot
   express "compression on, no backend chosen".
3. **Three settings are startup-only:** `compression-mode`,
   `compression-threads`, `compression-dict-size`. `CONFIG SET` against them is
   rejected, like Valkey's `*_cpulist` settings.
4. **No drain phase, no runtime mode transitions.** A restart is a full reset,
   because nothing compressed is ever persisted. A primary and a replica may even
   run different algorithms.
5. **One backend for the whole process life.** Chosen once at startup. This is
   what deletes: a per-entry backend field in the dictionary registry,
   both-backends-live handling, per-frame algorithm tags, and frame magic.
   `dict_id` picks a dictionary *version* across retrains, never an algorithm.
6. **No separate sweeper on/off switch.** An earlier draft had
   `compression-automatic-sweeper` with `enabled`/`disabled`. It was removed
   because it allowed a silent no-op state (`mode=zstd` plus
   `sweeper=disabled`). The sweeper now runs whenever `compression-mode` is not
   `off`.
7. **The 14 advanced settings are v2.** In v1 they are hardcoded defaults, not
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

The sweeper runs on `compressionCron`, on the cron tick, paced by
`budget_us = (1_000_000 / server.hz) * compression-sweep-max-cpu-pct / 100`,
which is about 25 ms per tick at defaults. It does **not** run in `beforeSleep`.
`beforeSleep` only restores the transient decompressed view, which is an O(1)
operation of roughly 100-200 ns per key touched. This was a point of confusion
once already.

---

## 3. Still open

1. **§6.3 of the design: what to free when a read ends.** Options A (value stays
   compressed), B (value stays plain), and the A-variant (scratch buffer, frame
   never leaves `val_ptr`). The plan of record is the A-variant first, because it
   keeps frames alive across reads while dropping the side-map, the `beforeSleep`
   hook, and the savings cap. The deciding number is *reads per compressed
   lifetime*, which nobody has measured. Phase 9 of the execution plan measures
   it.
2. **`compression-promote-read-threshold` has no single default.** The design
   still says "3-5, not fixed yet". Same phase 9 measurement should settle it.
   Lower urgency now that the setting is v2 and hardcoded in v1.
3. **The stale `compression_alg.h` / `compression_alg.c` files** from an earlier
   experiment: rewrite or delete. Never decided. Note these are *not* the same as
   the `src/compression*.{c,h}` files in this checkout, which belong to a
   different feature (see below).

---

## 4. Facts about this checkout

The execution plan carries the full list with line numbers. The four that matter
most:

1. **`src/compression.{c,h}` is already taken** by stream compression for the
   RDB/replication byte stream (`streamCompressor`, `streamDecompressor`,
   `compressionAlgo` with `ALGO_NONE`/`ALGO_LZF`/`ALGO_LZ4`), together with
   `src/compression_lz4.{c,h}` and `src/compression_stream.{c,h}`. Different
   feature. New code uses the `value_compression*` file prefix and the
   `valueCompress*` symbol prefix, and the design's `compressor` vtable becomes
   `valueCompressor`.
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
  command with `cd /Users/eifrah/devl/valkey-inline-compression && ...`.

---

## 6. Suggested first move when resuming

Read `design-docs/inline-compression.md`, then `execution-plan.md`, then decide
whether §6.3 can be closed with a measurement before writing phase 3 code. If
not, start with phase 1 (vendor zstd plus the `valueCompressor` vtable), which
does not depend on that decision at all.
