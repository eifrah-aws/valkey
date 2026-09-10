# Inline In-memory Compression for Valkey — Design Summary

_A short version for a 30-minute talk. Full detail: `merged-design.md`._
_Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423)_

_Note on wording: "compression dictionary" always means the trained block of sample bytes used by the compressor. It is never a Valkey `dict` (hashtable)._

---

## 1. The problem

Valkey keeps every value in memory as a plain SDS string. Many `OBJ_STRING` values are JSON, HTML, or protobuf — highly repetitive, and stored raw.

Fleet data from AWS ElastiCache: on 92% of memory-bound snapshots, values in the 256 B – 1 MiB range compress by **50% or more** with a trained compression dictionary. Memory is the binding constraint on those hosts, not CPU.

**Target:** ≥30% memory reduction, under 20% TPS cost, opt-in, invisible to clients.

---

## 2. What we build

A background sweeper walks the keyspace and compresses cold string values in place. Reads decompress automatically. Clients see no difference.

| Part | Where it runs |
|---|---|
| Sweeper — picks candidates, paces itself by CPU | Main thread, cron tick |
| Worker pool — does the compression | `compression-threads` threads |
| Trainer — samples the keyspace, builds a compression dictionary | `bio` thread |
| Decompression — on every read | Main thread, synchronous |

Off by default. One switch turns it on, and it also names the algorithm: `compression-mode zstd`.

---

## 3. Scope cut: nothing compressed leaves the process

The most important decision in the design, so it comes early.

RDB, full sync, AOF, `DUMP`, and `MIGRATE` all handle **plain, uncompressed data**. Every value decompresses before it crosses the process boundary.

**What this buys:**
- No `RDB_VERSION` bump. RDB files are byte-identical to a build without the feature.
- No compression dictionary serialization, no AUX fields, no replica capability negotiation.
- No untrusted input. A bad frame is our own bug — assert, don't handle.
- A restart is a full reset. No compressed frame survives one, so a primary and a replica may even run different algorithms.

**What it costs:** full sync streams the uncompressed size, and a fresh replica holds data uncompressed until its own sweeper catches up. Mitigation: `rdbLoad` compresses inline while loading, with a compression dictionary trained locally.

The alternative — compressing RDB and the wire — is a permanent compatibility promise: every future Valkey must decode every frame format we ever ship. Not for v1.

---

## 4. The compression dictionary, and why we hold more than one

Normal compression finds repeats **inside one value**. A 500-byte JSON value has almost none, so there is nothing to save. This is why small values compress badly on their own.

A compression dictionary is a block of sample bytes, trained from real values in the keyspace. The compressor treats it as text that came just before your value, so it can point back into it. `"user_id"` appears once in your value, but exists in the dictionary, so it still shrinks to a few bytes.

**A compression dictionary is frozen once trained.** A compressed frame does not contain words — it contains offsets into the dictionary. Editing the dictionary would move every offset in every existing frame, breaking all of them at once. So it is read-only for its whole life.

That is why retraining creates a **new, separate** compression dictionary with a new ID rather than updating the old one:

| State | Live compression dictionaries |
|---|---|
| Enabled, before first training | 0 |
| Steady state | **1** — the active one |
| After a retraining | **2** — new active, old draining |
| Slow drain plus another retraining | up to 4 (`compression-dict-max-versions`) |

**Two is the normal case we design for.** New compressions use the active dictionary. Frames made earlier still need the old one, so a small registry keeps it alive until its user count reaches zero. Every dictionary in the registry, across every retrain, is built by the same backend — `compression-mode` is fixed for the process's whole life (§2) — so the registry needs no per-entry backend field. Each frame counts as one user; freeing a frame calls the backend's `release()` hook, which drops the count. No background job hunts down old frames.

Retraining is triggered by drift: when the live compression ratio gets clearly worse than the ratio right after the last training. Also on first reaching `compression-dict-min-training-keys` (default 1000), on an optional interval, and manually via `COMPRESSION TRAIN`.

---

## 5. Compression path

### 5.1 Choosing a value

The sweeper takes a value only if **a dictionary is active**, and the value is `OBJ_STRING`, `OBJ_ENCODING_RAW`, sole-owner (`refcount == 1`), within the size limits, and **cold** — idle long enough, or low LFU frequency. Compressing hot keys wastes work that reads immediately undo.

The first condition is not just a convenience. Every frame must reference a dictionary, because `dict_id` is what tells the read path which dictionary *version* to decode with (§5.4) — the backend itself is fixed for the process (§2). A frame built without a dictionary would have nothing to resolve.

Pacing copies the active-expire model exactly, so there is nothing new for operators to learn:

```
budget_us = (1_000_000 / server.hz) * compression-sweep-max-cpu-pct / 100
```

25 ms per tick at defaults.

### 5.2 The worker must not race the main thread

A worker compresses a value's bytes. The main thread may be mutating those same bytes.

**The tempting fix:** hand the worker a live pointer, and require every mutating command to copy-on-write first. That is a *convention*. It depends on every current and future code path obeying it. One missed call site is a use-after-free.

**What we do instead: an owned snapshot.** At enqueue, the main thread copies the bytes into a buffer the job owns. The worker never touches the keyspace.

Two bug classes become impossible rather than merely audited:
- **Use-after-free** — `APPEND` frees the old buffer; the worker isn't reading it.
- **Torn read** — `SETBIT` changes bytes mid-compression; the worker's copy is private.

Cost: one extra memory pass, ~0.1–2 µs for typical values. That is **20–40× cheaper than the compression itself**, and paid once per attempt — while decompression is paid on every read, forever.

### 5.3 Freshness: the version counter

A snapshot is safe, but it can go stale. The key may change while the job is queued, so installing the result would resurrect old data.

A small in-flight table, keyed by `(dbid, key)`, holds an entry only while work is pending:

```c
typedef struct inflightEntry {
    uint64_t version;   /* bumped on every mutation */
    uint32_t refs;      /* queued jobs + open transient views */
} inflightEntry;
```

Enqueue records the version. At drain: version match → install; mismatch → discard.

The version bumps from two hooks that already exist and are already called by every mutating command: `signalModifiedKey()` and `dbUnshareStringValue()`. No new discipline for command authors. `FLUSHALL`/`FLUSHDB`/`SWAPDB` skip per-key hooks, so a global `keyspace_epoch` covers them the same way.

**Snapshot and version solve different problems.** The copy gives memory safety. The version gives freshness. We need both.

### 5.4 What gets stored: the compression frame

The worker's output is one buffer with a small header in front of the compressed bytes:

```c
typedef struct compressionFrame {
    uint32_t uncompressed_len;  /* size to allocate when decompressing */
    uint32_t dict_id;           /* which compression dictionary decodes this */
    uint16_t read_streak;       /* reads since this frame was created (§6.4) */
    unsigned char body[];       /* the compressed bytes */
} compressionFrame;
```

10 bytes of header. `dict_id` is the load-bearing field — it is how a frame says *which* compression dictionary can decode me, and it is why old dictionaries can be retired safely (§4).

Three fields we deliberately left out:
- **No compressed length.** `sdslen(frame) - 10` is exact and free. A stored copy could disagree with sds.
- **No algorithm tag.** `compression-mode` is startup-only (§2), so every frame this process ever creates comes from the same backend. `dict_id` exists to pick the dictionary *version* across retrains (§4), not the algorithm.
- **No length or magic check.** Frames never come from outside the process (§3), so a malformed frame is our own bug. It should assert, not return a recoverable error.

**One allocation, one copy.** The worker allocates header plus worst-case body once, and the backend compresses straight into the space after the header — no intermediate buffer. The header is built as a stack struct and written with a **single 10-byte `memcpy`**. Never a struct cast: an sds `buf` sits after a 3- or 5-byte sds header, so it is not 4-byte aligned, and a `uint32_t *` cast onto it is undefined behavior.

Two traps worth naming out loud, because both are silent:
- `sizeof(compressionFrame)` is **12**, not 10 — the struct pads to its 4-byte alignment. Offsets must come from named constants, with `static_assert` on `offsetof` for each field.
- `compress_bound` is generous, so the frame must be shrunk to its real length after compressing. Skipping that leaves unused capacity the allocator still charges for, and reported savings would exceed real savings.

Finally, a **net-savings guard**: if the frame plus header is not meaningfully smaller than the original, we throw it away and count it. Incompressible data costs us one wasted attempt, not permanent overhead.

---

## 6. Decompression path

### 6.1 How we know a value is compressed

A new object encoding, `OBJ_ENCODING_COMPRESSED` (12). One field on the `robj` says whether the bytes need decompressing — no side table, no per-value flag. `OBJECT ENCODING` reports it as `compressed`.

The cost is an audit. Sites that read `obj->encoding` directly must handle the new tag, and they split into two kinds:

- **Need the plaintext** → must decompress: RDB save, `DUMP`, `DEBUG DIGEST`.
- **Only need to know it is compressed** → no decompression: object free (calls `release()` to drop the compression dictionary's user count), `MEMORY USAGE`, active-defrag.

### 6.2 The read helper

Every read goes through one function:

```c
robj *objectGetUncompressedView(robj *o, sds *scratch, robj *view_out);
```

Not compressed → returns the object untouched, zero cost. Compressed → decompresses into a temp buffer, stashes the frame aside, and returns something that looks like an ordinary RAW string. Callers need no changes.

At `beforeSleep` we put the frame back with a pointer swap — free, no recompression. If the key was written meanwhile, we drop the frame and keep the plain value; the sweeper can compress it again later.

**This check must compare versions, not pointers.** A same-size in-place write (`SETBIT`) can reuse the same allocation: pointer-equal, content different. Only the version catches that.

**Memory is bounded by construction:** temp decompressed bytes never exceed the bytes compression has saved. Past that line, a value falls back to permanent decompression. Peak memory cannot exceed the no-compression baseline.

### 6.3 Open decision: what do we free when the read ends?

When a read finishes, **both copies exist** — plain text in `val_ptr`, frame in the side-map. For that key, memory is briefly worse than with the feature off. One of them has to go, and which one is still open.

| | Option A: value stays compressed in the keyspace | Option B: value stays plain text in the keyspace |
|---|---|---|
| We free | the plain text | the compressed buffer |
| Cost now | ~100–200 ns of bookkeeping | none |
| Cost later | none | rebuild the frame: 3–650 µs of worker CPU, plus a sweep cycle and the idle wait |
| Machinery needed | side-map, `beforeSleep` hook, savings cap, `read_streak` | none of it; header drops to 8 bytes and becomes immutable |

**The case for B:** we only compress cold values, so a frame is probably read once and then left alone for a long time. It deletes a lot of the design. It is the same thing as `compression-promote-read-threshold = 1`.

**The case for A:** the risk in B is cumulative, not per-command. Every read frees a frame permanently, so any pass that reads much of the keyspace — a wide `MGET`, a script over a range, a `SCAN`+`GET` loop, an export or warm-up job — frees every frame it touches. The whole database then has to be recompressed at 25 ms per tick while sitting at baseline memory. If those passes are periodic and faster than recovery, the keyspace never converges.

**A variant of A:** never move the frame out of `val_ptr`. Decompress into a scratch buffer, return a stack `robj`, free the scratch at end of command. Reads still never destroy a frame, and the side-map, the `beforeSleep` hook, and the cap all disappear. The price is no amortization across reads in one iteration, plus an audit rule that no caller keeps the view past its command.

**What settles it:** *reads per compressed lifetime* — how many reads a frame sees before it dies. Nobody has measured this, and the 3–5 default for `K` is a guess too. If it is almost always 1, B wins and the machinery goes away.

### 6.4 Read-hot values need an exit

A value that turns hot again after compression would pay decompress-and-restore on every read, forever. That is what `read_streak` in the frame is for. After `compression-promote-read-threshold` reads (default 3–5), we promote the value to permanent RAW instead of restoring the frame.

Wasted work is bounded at `K` per compressed lifetime. The counter lives in the frame, so it costs nothing for values that are never compressed, and resets for free when a write discards the frame.

---

## 7. The algorithm is replaceable

The compression library is never called directly. One small vtable:

```c
typedef struct compressor {
    const char *name;
    void *(*ctx_new)(void);
    void  (*ctx_free)(void *ctx);
    int    (*train)(void *ctx, const void *samples, const size_t *sizes, unsigned n,
                    void *dict_out, size_t dict_cap);
    void  *(*dict_load)(const void *dict_buf, size_t len);
    void   (*dict_free)(void *cdict);
    size_t (*compress_bound)(size_t srclen);
    size_t (*compress)(void *ctx, void *cdict, const void *src, size_t srclen,
                       void *dst, size_t dstcap);
    size_t (*decompress)(void *ctx, void *cdict, const void *body, size_t body_len,
                         void *dst, size_t dstcap);
    void   (*release)(void *ctx, void *cdict);
    int         (*last_error)(void *ctx);
    const char *(*strerror)(int err);
} compressor;
```

Two shapes here are deliberate. **The caller allocates the destination**, which is what makes the one-allocation build in §5.4 possible. And **`cdict` is an argument, not state inside `ctx`**, because several compression dictionaries are live at once (§4) — a worker may compress with the active one while the main thread decompresses a frame built with a retiring one.

The backend is selected **once, at startup**, from `compression-mode` (§2) — never per dictionary, and never at runtime. Every dictionary this process ever trains, across every retrain, uses that one backend, so a frame's `dict_id` only needs to pick a dictionary version (§4), never an algorithm. That is what deletes frame magic tags and per-frame algorithm detection.

The second backend (LZ4) is operator-selectable at startup, because `compression-mode` names the algorithm. `zstd` stays the default and the recommended value: dictionary support is what makes small values compress well, and LZ4's dictionary support is weaker. The honest summary for the config docs is that `lz4` trades ratio for speed, and the gap is widest exactly where this feature aims — values of a few hundred bytes. Note that LZ4 is already vendored in `deps/lz4`; zstd is not, so the default adds a vendored dependency.

---

## 8. Configuration

All names are ordinary `CONFIG GET` / `CONFIG SET` settings, except `compression-mode`, `compression-threads`, and `compression-dict-size` — those three are read once at startup, like Valkey's `*_cpulist` settings, and `CONFIG SET` against them is rejected. Everything is off unless `compression-mode` is set at boot.

**Five primary settings**

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-mode` | `off`, `lz4`, `zstd` | `off` | The main switch. Off, or the algorithm to compress with — the background sweeper runs automatically whenever it's not `off`. | **No — startup only.** |
| `compression-threads` | `0`–`16` | `1` | Worker pool size, fixed at startup. `0` means the feature never starts a pool. | **No — startup only.** |
| `compression-min-value-size` | bytes | `256` | Smallest value worth compressing. | Yes |
| `compression-max-value-size` | bytes, `0` = no cap | `131072` (128 KiB) | Largest value. Caps worst-case per-read latency. | Yes |
| `compression-dict-size` | bytes | `102400` (100 KiB) | Target size of a trained compression dictionary. | **No — startup only.** |

**The mode names the algorithm, so there is no separate `compression-alg`.** One switch says both *whether* to compress and *how*. An operator cannot express the broken state "compression on, no backend chosen."

- **`off`** — no compression, no background work, for the life of this process.
- **`lz4`** — compress with the LZ4 backend, for the life of this process.
- **`zstd`** — compress with the Zstandard backend, for the life of this process.

**The mode is fixed at startup — there are no runtime transitions.** Changing it means restarting with a different value. That's not a limitation to work around: nothing compressed is ever persisted (§3), so a restart already starts from a clean, plain keyspace — there's nothing to drain or migrate, and no moment where a running process needs to read a frame written by a different algorithm. That single fact is also why one backend, chosen once at startup, is enough for the whole process (§7).

**Advanced settings (v2)**

None of these are configurable in v1 — the mechanisms behind them run with the hardcoded defaults shown here. Exposing them via `CONFIG GET`/`CONFIG SET` is v2 work.

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-inflight-max-bytes` | bytes | `33554432` (32 MiB) | Cap on snapshot bytes queued to workers. | No |
| `compression-promote-read-threshold` | `0`–`65535` | 3–5, not fixed yet; `0` disables | Reads after which a value is promoted to permanent RAW (§6.4). | No |
| `compression-automatic-sweeper-interval` | seconds, `0` = keep cycling | `0` | Pause between sweeper passes. | No |
| `compression-sweep-max-cpu-pct` | `1`–`100` | `25` | Share of each cron tick the sweeper may spend. | No |
| `compression-min-savings-ratio` | percent | `10` | Net-savings guard. Below this, the frame is thrown away. | No |
| `compression-min-idle-seconds` | seconds | `60` | Coldness gate in LRU and noeviction modes. | No |
| `compression-lfu-threshold` | `0`–`255` | `5` | Coldness gate in LFU mode. | No |
| `compression-dict-min-training-keys` | int | `1000` | First training fires at this key count, and needs this many samples. | No |
| `compression-dict-max-training-keys` | int | `10000` | Sample cap for one training scan. | No |
| `compression-training-buffer-size` | bytes | `16777216` (16 MiB) | Sample buffer, allocated in full per scan and freed after it. | No |
| `compression-dict-drift-ratio` | percent | `70` | Retrain when the live ratio degrades past this. | No |
| `compression-dict-refresh-interval` | seconds, `0` = off | `0` | Periodic retrain, on top of drift. | No |
| `compression-dict-max-versions` | `2` or more | `4` | How many compression dictionaries may be live at once (§4). | No |
| `compression_cpulist` | CPU list string | `""` | Pins the worker threads. | No |

**Two bounds that matter in production:**
- `compression-max-value-size` (128 KiB) — caps worst-case per-read latency.
- `compression-inflight-max-bytes` (32 MiB) — caps snapshot memory. Bounding queue *slots* alone would allow 256 MiB of snapshots, which is absurd in a memory-saving feature.

`INFO compression` reports savings, live and lifetime ratio, queue depth, stale jobs, training state, and errors.
