# Inline In-memory Compression for Valkey — Design Summary

_Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423)_

_Note on wording: "compression dictionary" always means the trained block of sample bytes used by the compressor. It is never a Valkey `dict` (hashtable)._

---

## 1. The problem

Valkey keeps every value in memory as a plain SDS string. Many `OBJ_STRING` values are JSON, HTML, or protobuf — highly repetitive, and stored raw.

Fleet data from AWS ElastiCache: on 92% of memory-bound snapshots, values in the 256 B – 1 MiB range compress by **50% or more** with a trained compression dictionary. Memory is the binding constraint on those hosts, not CPU.

**Target:** ≥30% memory reduction, under 20% TPS cost, opt-in, invisible to clients.

---

## 2. What we intend to build

We intend to add an opt-in, transparent compression capability for Valkey values, initially focusing on string values. When enabled, Valkey will identify cold values in the background, compress eligible values with the selected algorithm, optionally using a trained compression dictionary, and retain the compressed representation only when it produces a net memory saving. The first client read of a compressed value will transparently decompress it, restore the RAW representation, and mark the key hot so later reads avoid decompression and the sweeper does not immediately recompress it.

The design will keep candidate selection and installation coordinated with the main event loop while moving expensive compression and dictionary-training work out of the request path. It will bound the work performed in each cycle, verify that a key has not changed before replacing its value, and expose enough operational information to measure savings and cost. The detailed execution model may evolve during implementation, but the intended outcome is predictable background work without compromising Valkey's data semantics.

---

## 3. Data entering Valkey

Data enters Valkey through five paths. Client commands, slot migration, incremental replication, and command-form AOF replay carry logical, uncompressed values; RDB load and an RDB-format AOF base file preserve the stored representation. Client writes use background compression, while slot-migration imports and replication-stream updates compress eligible values inline to prevent a large transfer or catch-up operation from accumulating an uncompressed copy of the dataset.

### 3.1 Client write commands

A value supplied by a client command enters as uncompressed bytes and is installed using the existing command semantics. The command path does not compress inline: adding compression to write latency would make foreground cost depend on value size and algorithm choice. After installation, the background sweeper may select the value, compress an owned snapshot, and replace the value only if the key is unchanged and the result produces a net memory saving.

Commands that mutate an existing compressed value first obtain its logical, uncompressed representation. The command then applies the mutation and stores the resulting value as plain data; any later recompression is background work.

### 3.2 Slot migration import

Slot migration receives uncompressed logical values. When compression is enabled, the destination compresses each eligible value inline before its final installation. It uses the destination's algorithm and active dictionary when applicable, applies the same size, format, and net-savings checks as the background path, and installs the value as plain data if it is ineligible or compression produces no saving.

Inline compression is deliberate here: installing every migrated value as plain data and waiting for the sweeper could create an unintended memory spike proportional to the imported slot. Processing one value at a time bounds additional plaintext memory to the value currently being imported and keeps the destination independent of the source node's algorithm and dictionary set.

> **TODO — validate inline migration compression with measurements:** quantify destination peak memory, migration throughput, per-value latency, and event-loop occupancy across value sizes and compression ratios.

### 3.3 RDB load

RDB preserves values **as-is**. A value that was plain when the snapshot was taken is loaded as plain, and a value that was compressed is loaded as the same compressed frame. The loader does not decompress compressed entries or synchronously compress plain entries.

To make compressed entries self-contained, the RDB also stores the frame-format version, algorithm metadata, and every compression dictionary referenced by any frame in the snapshot. The loader reconstructs the decoder and dictionary registry before exposing compressed values, validates every frame's algorithm and dictionary reference, and rebuilds dictionary user counts while loading. An unknown format, unsupported algorithm, missing dictionary, or malformed frame fails the RDB load rather than reaching the keyspace.

Persisting frames creates a durable compatibility obligation and requires an RDB format change: future versions that accept this RDB must continue to decode its frame and dictionary formats. The implementation must define downgrade and unsupported-backend behavior before the format is released.

### 3.4 Replication stream application

Incremental replication carries normal Valkey commands and uncompressed logical values. When compression is enabled on the replica, each command that creates or replaces an eligible value compresses the resulting value inline before final installation. A mutation of an existing compressed value obtains its logical representation, applies the command, and inline-compresses the result when eligible. The replica uses its local algorithm and active dictionary when applicable; it does not need the primary's dictionary for incremental updates.

Inline compression prevents replication catch-up from building an unintended uncompressed copy of the changed dataset while waiting for a later sweep. Values that are ineligible or do not pass the net-savings guard remain plain. Primary and replica may therefore use different physical representations while retaining the same logical dataset.

During full synchronization, the incoming snapshot follows §3.3: compressed and plain values are loaded as-is together with all referenced dictionaries. Commands buffered after the snapshot offset then arrive through the uncompressed incremental stream and follow the inline-compression rule above.

> **TODO — validate inline replication compression with measurements:** quantify replica peak memory, apply throughput, replication lag, event-loop occupancy, and catch-up time with and without inline compression.

### 3.5 AOF load

AOF loading follows the format of each multipart AOF component. When the base file was created with `aof-use-rdb-preamble yes` (the default), it is an RDB-format base and loads exactly as described in §3.3: plain and compressed values are restored as-is, all referenced dictionaries and decoder metadata are loaded before the keyspace becomes visible, and malformed or unsupported compressed state fails the load.

When `aof-use-rdb-preamble no`, the base file contains logical commands and uncompressed values. Incremental AOF files also contain logical commands and uncompressed values regardless of the preamble setting. The AOF loader replays those commands from their uncompressed representation; it does not need compression dictionaries to parse command-form files. Values produced by command replay remain plain until normal background compression selects them. A mixed multipart AOF can therefore restore compressed values from its RDB base and then apply plain command updates from its incremental files.

This distinction creates two recovery profiles. An RDB-format base preserves existing memory savings but carries the same durable frame, algorithm, and dictionary compatibility requirements as RDB. A command-format base is independent of compressed frame formats but can rebuild the dataset at the uncompressed memory baseline until the sweeper catches up.

> **TODO — validate AOF load with measurements:** compare startup time, peak memory, dictionary loading, and time to recover steady-state compression for RDB-preamble and command-format bases, each with incremental files.

---

## 4. Data leaving Valkey

Data leaving Valkey follows one of two rules. RDB and an RDB-format AOF base preserve the stored representation; client-visible and node-to-node logical interfaces plus command-form AOF files emit uncompressed values. A client read promotes a compressed value to RAW and marks its key hot (§4.2). Other outgoing logical interfaces synchronously decompress into a temporary buffer without changing the value stored in the keyspace.

> **TODO — validate synchronous decompression with measurements:** benchmark latency, throughput, event-loop occupancy, and peak temporary memory for each outgoing path before finalizing the v1 execution model. If the numbers are not acceptable, move decompression off the main thread or introduce bounded incremental work.

### 4.1 Slot migration export

Slot migration sends uncompressed logical values. If a selected value is compressed, the source synchronously decompresses it for the migration payload and leaves the stored frame unchanged. The destination consequently requires no knowledge of the source algorithm or dictionaries and processes the value through §3.2.

> **TODO — back the wire-format decision with numbers:** compare migration duration, network bytes, source CPU, main-thread stall time, and destination memory for plaintext transfer versus a compression-aware protocol.

### 4.2 Client reads

Reads return uncompressed logical values. An uncompressed value follows the existing path. On the first read of a compressed value, Valkey synchronously decompresses it, replaces the stored frame with the resulting RAW value, and adds the key to the internal hot-key LRU described in §7.3. Later reads use the RAW value directly. Every read refreshes the key's hot-key entry, and the background sweeper excludes keys that remain in that LRU. This pays decompression once when a cold key becomes active instead of on every read.

The tradeoff is deliberate: the first read gives back that key's memory saving. The value can become eligible for compression again only after its hot-key entry expires or is evicted and the normal coldness gate in §6.1 also passes.

> **TODO — back read promotion with numbers:** measure p50, p99, and p99.9 first-read latency, steady-state hot-read latency, throughput, event-loop stall time, memory returned by promotions, and recompression attempts across value sizes and multi-value commands.

### 4.3 RDB save and full synchronization

RDB save writes each value **as-is**: plain values remain plain and compressed values remain compressed. It also serializes the frame-format and algorithm metadata plus every compression dictionary referenced by the snapshot. The snapshot must never contain a frame whose dictionary is absent, including when retraining or dictionary retirement overlaps a background save.

A full synchronization uses this RDB representation as its baseline. The replica loads the snapshot as described in §3.3 and then applies the buffered, uncompressed replication commands produced after the snapshot offset. This preserves in-memory savings across full sync without making the incremental replication stream compression-aware.

### 4.4 Replication stream propagation

Incremental replication sends uncompressed logical commands and values. Commands that already carry client-provided bytes can propagate those original plain bytes. Any propagation path that must materialize a value from the keyspace will synchronously decompress a compressed value before encoding the replication command, without replacing the stored frame.

> **TODO — back the replication decision with numbers:** quantify replication bandwidth, primary CPU, main-thread stall time, replica catch-up time, and backlog pressure for uncompressed propagation versus a negotiated compressed stream.

### 4.5 AOF append and rewrite

Incremental AOF persistence always writes logical commands and uncompressed values. Commands that already carry client-provided bytes append those original bytes. If an AOF path must materialize a value from the keyspace, it decompresses a compressed value for command encoding without replacing the stored frame. Incremental files never reference a compression dictionary.

AOF rewrite depends on `aof-use-rdb-preamble`:

- With `aof-use-rdb-preamble yes` (the default), the new base file is RDB format and follows §4.3. It writes values as-is and includes the frame-format and algorithm metadata plus every dictionary referenced by a compressed frame.
- With `aof-use-rdb-preamble no`, the new base file is a command-form AOF. The rewrite child emits logical commands with uncompressed values. It temporarily decompresses compressed values while encoding those commands and does not change the primary's stored representation.

A multipart AOF may therefore contain a representation-preserving RDB base followed by uncompressed incremental command files. The RDB base carries the durable compatibility requirements of §3.3; the incremental files remain independent of compression algorithms and dictionaries.

> **TODO — validate AOF persistence with measurements:** compare rewrite duration, child peak memory, output size, copy-on-write impact, and load time for both preamble settings with plain, compressed, and mixed keyspaces.

---

## 5. The compression dictionary, and why we hold more than one

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

**Two is the normal case we design for.** New compressions use the active dictionary. Frames made earlier still need the old one, so a small registry keeps it alive until its user count reaches zero. Each registry entry records the algorithm and dictionary ID needed to decode its frames. RDB save persists every referenced entry, and RDB load rebuilds the registry and its user counts before compressed values become visible (§3.3 and §4.3). Each frame counts as one user; freeing a frame calls the matching backend's `release()` hook, which drops the count. No background job hunts down old frames.

Retraining is triggered by drift: when the live compression ratio gets clearly worse than the ratio right after the last training. Also on first reaching `compression-dict-min-training-keys` (default 1000), on an optional interval, and manually via `COMPRESSION TRAIN`.

---

## 6. Background compression path

### 6.1 Choosing a value

On each `compressionCron()` call, the sweeper first checks `compression-max-inflight-requests`. An in-flight request remains counted from enqueue until the main thread installs or discards its result, including queued, running, and completed-but-not-drained jobs. If the in-flight count has reached `compression-max-inflight-requests`, 100 by default, the sweeper does not sample.

When capacity exists, the sampling budget per cron call is `compression-max-inflight-requests` returned entries, 100 by default. It is not controlled by another setting. The sweeper chooses a non-empty database with probability proportional to its key count, selects one of that database's hashtables with `kvstoreGetFairRandomHashtableIndex()`, and calls `kvstoreHashtableSampleEntries()` for up to the remaining budget. If the selected hashtable has fewer entries, it repeats the database and hashtable selection until the budget is exhausted or the in-flight request cap is reached. The kvstore helper excludes importing hashtables.

`kvstoreHashtableSampleEntries()` returns no more entries than requested and does not return the same entry twice within one call. Samples can repeat across cron calls, and no cursor or completed-pass state is kept. Selection is therefore probabilistic rather than an eventual-coverage guarantee. `INFO compression` reports sampled entries and eligibility rejection reasons so operators can measure repeated work and convergence as the share of compressed values rises.

The sweeper reads each sampled database entry directly. Sampling must not perform a normal key lookup, refresh LRU/LFU metadata, or add the key to the hot-key LRU. Candidate discovery is not a client access.

A sampled value is cold according to the access metadata selected by the maxmemory policy. In LFU mode, the sweeper applies normal LFU decay and accepts a frequency at or below `compression-lfu-threshold`, 5 by default. In LRU and `noeviction` modes, it accepts an idle time at or above `compression-min-idle-seconds`, 60 seconds by default. The object's 24-bit field stores one of these forms, so the sweeper never applies both tests.

After the coldness test, the sweeper takes a value only if a compression backend is active and the value is `OBJ_STRING`, `OBJ_ENCODING_RAW`, sole-owner (`refcount == 1`), within the size limits, and absent from the internal hot-key LRU (§7.3). The hot-key LRU is an independent first-line exclusion: it blocks recently read keys before object metadata is evaluated, while the policy-specific coldness gate remains a fallback if that bounded LRU evicts an entry. Compressing a hot key would waste worker CPU and make its next read pay synchronous decompression again.

Dictionary-capable modes use the active dictionary when one is available. A backend may also create a frame without a dictionary—for example, an LZ4 configuration that does not use one—in which case `dict_id` is zero. The frame still records the algorithm and format needed to decode it (§6.4).

**Skip data that is already compressed.** Before the snapshot copy, the sweeper compares the first bytes of the value against a small table. A match means the bytes are already compressed, so an attempt would spend a main-thread `memcpy` (§6.2), a worst-case-size allocation, and worker CPU to learn nothing.

| Format | First bytes |
|---|---|
| PNG | `89 50 4E 47 0D 0A 1A 0A` |
| JPEG | `FF D8 FF` |
| GIF | `47 49 46 38`, "GIF8" |
| WEBP | `52 49 46 46`, "RIFF", then `57 45 42 50`, "WEBP", at offset 8 |
| gzip | `1F 8B` |
| zip | `50 4B 03 04` |
| zstd | `28 B5 2F FD` |
| xz | `FD 37 7A 58 5A 00` |
| bzip2 | `42 5A 68`, "BZh" |
| 7z | `37 7A BC AF 27 1C` |
| MP4 | `66 74 79 70`, "ftyp", at offset 4 |
| base64 PNG | `iVBORw0KGgo` |
| base64 JPEG | `/9j/` |

Two rules keep this honest. The list does **not** need to be complete: the net-savings guard (§6.4) still catches every format nobody listed, and a custom container that wraps a JPEG. And the check is a **hint, not a verdict** - it only avoids work, and it must stay a `memcmp` on the first bytes with no allocation, next to the size window and the coldness gate.

`INFO compression` counts values skipped this way, so an operator can see that a keyspace is full of images instead of guessing why the ratio is flat.

The sweeper does not use a wall-clock or CPU-percentage budget. Candidate work per cron call is bounded by `compression-max-inflight-requests`, 100 returned entries by default. At the default and `server.hz = 10`, it evaluates at most 1,000 sampled entries per second, and less when the request cap is full.

For each eligible value, the main thread reserves one in-flight request slot before copying and enqueueing it. The reservation is released only after the main thread installs or discards the result. This keeps queued, running, and completed-but-not-drained work within `compression-max-inflight-requests`, 100 by default.

A separate in-flight byte cap is unnecessary because `compression-max-value-size` is a finite hard limit. It defaults to 128 KiB, cannot be disabled, and cannot be raised above 128 KiB. Therefore 100 in-flight jobs own at most 12.5 MiB of input snapshots. Output allocation is also bounded: only running workers allocate a worst-case destination, and `compression-threads` is 1 by default and at most 16. A worker frees its input snapshot before publishing a completed output, so queued and completed representations do not accumulate both buffers for the same job.

### 6.2 The worker must not race the main thread

A worker compresses a value's bytes. The main thread may be mutating those same bytes.

**The tempting fix:** hand the worker a live pointer, and require every mutating command to copy-on-write first. That is a *convention*. It depends on every current and future code path obeying it. One missed call site is a use-after-free.

**What we do instead: an owned snapshot.** At enqueue, the main thread copies the bytes into a buffer the job owns. The worker never touches the keyspace.

Two bug classes become impossible rather than merely audited:
- **Use-after-free** — `APPEND` frees the old buffer; the worker isn't reading it.
- **Torn read** — `SETBIT` changes bytes mid-compression; the worker's copy is private.

Cost: one extra memory pass, ~0.1-2 us for typical values. That is **20-40x cheaper than the compression itself**, and paid once per attempt. If a compressed value is later read, promotion (§7.2) ensures that decompression is also paid only once per cold-to-hot transition.

### 6.3 Freshness: the version counter

A snapshot is safe, but it can go stale. The key may change while the job is queued, so installing the result would resurrect old data.

A small in-flight table, keyed by `(dbid, key)`, holds an entry only while work is pending:

```c
typedef struct inflightEntry {
    uint64_t version;   /* bumped on every mutation */
    uint32_t refs;      /* queued compression jobs */
} inflightEntry;
```

Enqueue records the version. At drain: version match → install; mismatch → discard.

The version bumps from two hooks that already exist and are already called by every mutating command: `signalModifiedKey()` and `dbUnshareStringValue()`. No new discipline for command authors. `FLUSHALL`/`FLUSHDB`/`SWAPDB` skip per-key hooks, so a global `keyspace_epoch` covers them the same way.

**Snapshot and version solve different problems.** The copy gives memory safety. The version gives freshness. We need both.

### 6.4 What gets stored: the compression frame

The worker's output is one buffer with a small, self-describing header in front of the compressed bytes:

```c
typedef struct compressionFrame {
    uint32_t uncompressed_len;  /* size to allocate when decompressing */
    uint32_t dict_id;           /* dictionary version; 0 means no dictionary */
    uint16_t algorithm_id;      /* backend required to decode the body */
    uint16_t format_version;    /* compression-frame format */
    unsigned char body[];       /* compressed bytes */
} compressionFrame;
```

The 12-byte header makes a persisted frame independently decodable. `algorithm_id` selects the backend, `format_version` selects the frame interpretation, and `dict_id` selects a dictionary within that backend's registry. A zero `dict_id` explicitly represents dictionary-free compression. Algorithm and format identifiers become compatibility-stable once written to RDB.

Fields deliberately left out:
- **No compressed length.** `sdslen(frame) - 12` is exact and free. A stored copy could disagree with sds.
- **No read counter.** Recent reads are tracked in the bounded hot-key LRU (§7.3), so the immutable frame needs no lifetime counter.
- **No dictionary payload.** Dictionaries are shared objects in the registry and are serialized once per RDB snapshot, not duplicated in each frame (§4.3).

**One allocation, one copy.** The worker allocates header plus worst-case body once, and the backend compresses straight into the space after the header—no intermediate buffer. The header is built as a stack struct and written with a **single 12-byte `memcpy`**. Never use a struct cast: an SDS `buf` sits after a 3- or 5-byte SDS header, so it is not guaranteed to be aligned for these fields. Offsets must come from named constants, with `static_assert` on `offsetof` for each field.

`max_output_size()` is generous, so the frame must be shrunk to its real length after compression. Skipping that leaves unused capacity that the allocator still charges for and makes reported savings exceed real savings.

Finally, a **net-savings guard** compares the allocation occupied by the completed frame with the allocation occupied by the original value. If the frame is not meaningfully smaller, it is discarded and counted. Incompressible data therefore costs one failed attempt rather than permanent overhead.

Frames created locally are trusted internal objects. Frames loaded from RDB are persisted input and must be validated before installation: supported algorithm, known format version, valid lengths, and a resolvable dictionary reference when `dict_id` is nonzero (§3.3).

---

## 7. Decompression details

### 7.1 How we know a value is compressed

A new object encoding, `OBJ_ENCODING_COMPRESSED` (12). One field on the `robj` says whether the bytes need decompressing — no side table, no per-value flag. `OBJECT ENCODING` reports it as `compressed`.

The cost is an audit. Sites that read `obj->encoding` directly must handle the new tag, and they split into four kinds:

- **Client read** → synchronously decompress in v1, install the resulting RAW value in the keyspace, release the frame, and mark the key hot (§7.2 and §7.3).
- **Need plaintext for another egress path** → synchronously decompress into a temporary value without changing the keyspace: slot migration, replication propagation that materializes a stored value, `DUMP`, and `DEBUG DIGEST`.
- **Preserve the stored representation** → do not decompress: RDB save copies the frame and persists its referenced decoder metadata and dictionary (§4.3).
- **Only inspect or release the object** → do not decompress: object free (calls `release()` to drop the dictionary's user count when applicable), `MEMORY USAGE`, and active defragmentation.

### 7.2 The read helper

Client reads go through one function that has the database and key context needed to replace the value. A plain value is returned unchanged. A compressed value is decompressed into a new RAW object, and the main thread atomically replaces the frame before the reply is built. Replacing the object releases the frame and decrements its dictionary user count. The reply then follows the existing RAW-object ownership rules.

The helper also records the key in the hot-key LRU (§7.3). Plain reads refresh an existing entry, so a key that stays active remains excluded from compression. This changes the cost from one decompression per read to one decompression per cold-to-hot transition.

No side-map or `beforeSleep` restoration is needed. Commands run on the main thread, so the replacement cannot race another command. The helper must still finish frame validation and decompression before changing the keyspace; on failure, it leaves the compressed value installed and returns an error.

#### Reply ownership after promotion

The promoted RAW object belongs to the keyspace. Existing reply code may retain it for copy avoidance with `incrRefCount()`, exactly as it does for any other RAW string. If the same command later writes or deletes the key, the reply's reference keeps the old object alive until the socket write completes. This avoids a separate temporary-view lifetime and does not require special cleanup at the end of the command.

Multi-key reads promote each compressed key independently. Peak temporary memory is therefore one decompression output plus reply references already tracked by `io_tracked_reply_len`; the helper must install or release each output before moving to the next key.

### 7.3 Hot-key LRU

Valkey keeps a bounded internal LRU set of recently read keys. Each entry identifies a database and key, records the last touch time, and participates in an MRU-to-LRU order. A client read inserts or refreshes the entry whether the current value is plain or compressed. The sweeper checks this set before copying a candidate and skips every match.

In v1 the set holds at most 100,000 entries. An entry expires after `compression-min-idle-seconds` without a read, which is 60 seconds by default, and the least recently used entry is removed when the capacity is reached. The bound prevents read tracking from growing with the keyspace. Expiry lets a key become compressible again after it cools. The existing idle/LFU test remains required, so early LRU eviction cannot by itself make an active key eligible.

The set tracks key identity, not a `robj *` or value pointer. It must not keep a deleted value alive. `DEL`, expiry, rename, database flush, and database replacement remove or update matching entries. False stale entries are safe because they only delay compression, but they must still age out normally.

`INFO compression` reports the current entry count, insertions, refreshes, capacity evictions, expirations, and sweeper skips. These counters show whether the 100,000-entry bound covers the active working set.

> **TODO — validate the hot-key LRU with measurements:** test working sets below and above 100,000 keys; measure tracking memory, lookup cost, decompressions per promoted key, capacity churn, skipped compression work, and time for a cooled key to become compressed again.

### 7.4 A read promotes a compressed value to RAW

Once a client reads a compressed value, the RAW result replaces the frame and remains installed. Later reads avoid decompression. The key stays out of the sweeper while it is present in the hot-key LRU and must also pass the normal coldness gate after leaving that set.

Other plaintext egress paths do not prove that the key is hot. Slot migration, replication propagation, `DUMP`, and `DEBUG DIGEST` therefore keep using temporary decompression and leave the stored frame unchanged. RDB save and load continue to preserve the stored representation (§3.3 and §4.3).

This policy trades memory for CPU only when a compressed key becomes active. A one-time scan can promote many values and temporarily return the keyspace toward its uncompressed size, but the bounded LRU prevents repeated compression/decompression churn. Values that are not read again age out and are compressed in later sweeps.

The compressed frame remains immutable and self-describing until promotion or a write replaces it (§6.4).

---

## 8. The algorithm is replaceable

The compression library is never called directly. `src/compressor_alg.h` holds three objects:

| Object | What it is |
|---|---|
| `compressorApi` | The compression function table. Every entry calls straight into a backend library. Shared, constant, never allocated. |
| `compressorConfig` | The value size range the operator asked for. |
| `compressorAlg` | What callers hold. Built by `newCompressor()`, released by `freeCompressor()`. Carries the function table, the plain data, and the backend's private scratch. |

```c
typedef struct compressorApi {
    int    (*state_new)(compressorAlg *instance);
    void   (*state_free)(compressorAlg *instance);
    int    (*train)(compressorAlg *instance, const void *samples, const size_t *sizes,
                    unsigned n, void *dict_out, size_t dict_cap);
    void  *(*dict_load)(const void *dict_buf, size_t len);
    void   (*dict_free)(void *cdict);
    size_t (*lib_max_output_size)(size_t input_len);
    size_t (*compress)(compressorAlg *instance, void *cdict, const void *src,
                       size_t srclen, void *dst, size_t dstcap);
    size_t (*decompress)(compressorAlg *instance, void *cdict, const void *body,
                         size_t body_len, void *dst, size_t dstcap);
    void   (*release)(compressorAlg *instance, void *cdict);
} compressorApi;
```

Three shapes here are deliberate.

**The caller allocates the destination**, which is what makes the one-allocation build in §6.4 possible. It asks `instance->max_output_size()` how much room it needs. That function applies the operator's range first, then asks the backend, so the range check exists in one place.

**`cdict` is an argument, not state inside the instance**, because several compression dictionaries are live at once (§5) — a worker may compress with the active one while the main thread decompresses a frame built with a retiring one. A dictionary is read only once `dict_load()` returns, so every thread shares one.

**An instance belongs to one thread and is never shared**, and it never changes after it is built. That is what removes every lock and atomic from this layer. Each worker thread holds one, and the main thread holds one for decompressing on reads. To change a setting, build a new instance and drop the old one.

A failure returns 0, and the code goes into `instance->last_error`. `compressorStrerror()` maps it to text. The codes are shared by all backends, because none of them is specific to one library.

The backend used for **new compression** is selected at startup from `compression-mode` (§9). Persisted frames may outlive the process that created them, so each frame records its algorithm and format version, and each loaded dictionary is associated with that algorithm (§6.4). The process must retain decoder contexts for every algorithm present in the loaded RDB even when new compression is disabled or configured to use another backend.

The second backend (LZ4) is operator-selectable at startup, because `compression-mode` names the algorithm. `zstd` stays the default and the recommended value: dictionary support is what makes small values compress well, and LZ4's dictionary support is weaker. The honest summary for the config docs is that `lz4` trades ratio for speed, and the gap is widest exactly where this feature aims — values of a few hundred bytes. Note that LZ4 is already vendored in `deps/lz4`; zstd is not, so the default adds a vendored dependency.

---

## 9. Configuration

All names are ordinary `CONFIG GET` / `CONFIG SET` settings, except `compression-mode`, `compression-threads`, and `compression-dict-size` — those three are read once at startup, like Valkey's `*_cpulist` settings, and `CONFIG SET` against them is rejected. Background compression is off unless `compression-mode` is set at boot. Decoder support remains available for compressed frames restored from RDB even when the mode is `off`.

**Five primary settings**

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-mode` | `off`, `lz4`, `zstd` | `off` | The main switch. Off, or the algorithm to compress with — the background sweeper runs automatically whenever it's not `off`. | **No — startup only.** |
| `compression-threads` | `0`–`16` | `1` | Worker pool size, fixed at startup. `0` means the feature never starts a pool. | **No — startup only.** |
| `compression-min-value-size` | bytes | `256` | Smallest value worth compressing. | Yes |
| `compression-max-value-size` | `1`-`131072` bytes; must be at least the minimum | `131072` (128 KiB) | Largest value. Hard cap for per-value decompression and compression-job memory. | Yes |
| `compression-dict-size` | bytes | `102400` (100 KiB) | Target size of a trained compression dictionary. | **No — startup only.** |

**The mode names the algorithm, so there is no separate `compression-alg`.** One switch says both *whether* to compress and *how*. An operator cannot express the broken state "compression on, no backend chosen."

- **`off`** — create no new compressed frames and run no compression background work; frames restored from RDB remain readable.
- **`lz4`** — compress with the LZ4 backend, for the life of this process.
- **`zstd`** — compress with the Zstandard backend, for the life of this process.

**The mode used to create new frames is fixed at startup—there are no runtime transitions.** Changing it means restarting with a different value. Because RDB preserves compressed values, startup must also initialize read-only decoder contexts and load every referenced dictionary from the RDB before exposing the keyspace (§3.3). Existing frames keep their recorded algorithm; the configured mode controls only future background compression. Disabling compression stops new work but does not make persisted compressed values unreadable.

**Advanced settings (v2)**

None of these are configurable in v1 — the mechanisms behind them run with the hardcoded defaults shown here. Exposing them via `CONFIG GET`/`CONFIG SET` is v2 work.

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-max-inflight-requests` | requests | `100` | Cap on queued, running, and completed-but-not-drained compression jobs. | No |
| `compression-min-savings-ratio` | percent | `10` | Net-savings guard. Below this, the frame is thrown away. | No |
| `compression-min-idle-seconds` | seconds | `60` | Coldness gate in LRU and noeviction modes, and hot-key LRU entry lifetime in every mode. | No |
| `compression-hot-key-lru-max-entries` | entries | `100000` | Maximum recently read keys excluded from background compression (§7.3). | No |
| `compression-lfu-threshold` | `0`–`255` | `5` | Coldness gate in LFU mode. | No |
| `compression-dict-min-training-keys` | int | `1000` | First training fires at this key count, and needs this many samples. | No |
| `compression-dict-max-training-keys` | int | `10000` | Sample cap for one training scan. | No |
| `compression-training-buffer-size` | bytes | `16777216` (16 MiB) | Sample buffer, allocated in full per scan and freed after it. | No |
| `compression-dict-drift-ratio` | percent | `70` | Retrain when the live ratio degrades past this. | No |
| `compression-dict-refresh-interval` | seconds, `0` = off | `0` | Periodic retrain, on top of drift. | No |
| `compression-dict-max-versions` | `2` or more | `4` | How many compression dictionaries may be live at once (§5). | No |
| `compression_cpulist` | CPU list string | `""` | Pins the worker threads. | No |

**Three controls bound background compression:**
- `compression-max-value-size` is finite and at most 128 KiB, bounding one input snapshot and one compression operation.
- `compression-max-inflight-requests` is 100, bounding job count and input snapshots to at most 12.5 MiB.
- `compression-threads` defaults to 1 and is at most 16, bounding concurrent compression and worst-case output allocations.

`INFO compression` reports savings, live and lifetime ratio, sampled entries, eligibility rejections by reason, in-flight requests, request-cap stops, stale jobs, training state, and errors.

---

## Appendix A: measured cost of compressing plain values during RDB load

The selected RDB design does not recompress plain values during load: it restores both plain and compressed values as-is (§3.3). The measurements below quantify the alternative and support that choice.

Source: `run-rdb-poc.sh` on branch `valkey-inline-compression-rdb-poc`. 5,000,000 keys, 512-byte JSON-like values, LZ4 with **no dictionary**, three runs of each load per host. The same generated data on every host, so the byte figures are identical and only the environment differs.

| | macOS arm64, libc, page cache | Linux, jemalloc, EBS gp3 | Linux, jemalloc, local NVMe |
|---|---|---|---|
| Baseline load | 7.20 s | 16.65 s | 9.42 s |
| Compress on load | 10.38 s | 20.25 s | 14.47 s |
| **Load time change** | **+44.2%** | **+21.6%** | **+53.7%** |
| `used_memory` | 3.27 -> 2.48 GB, -24.2% | 3.24 -> 2.45 GB, -24.4% | 3.24 -> 2.45 GB, -24.4% |
| `used_memory_rss` | -25.1% | -23.8% | -23.8% |
| Value bytes | -14.3% | -14.3% | -14.3% |
| Time inside LZ4 | 3.21 s, 101% of delta | 6.07 s, 169% of delta | 4.75 s, 94% of delta |

**The cost is a range, not a number: +22% to +54%.** The two Linux columns are the same machine, the same allocator, and the same data, differing only in storage. On EBS gp3 the baseline reads 2.1 GB at about 136 MB/s, so loading waits on the disk and part of the compression happens inside that wait. That is why the time inside LZ4, 6.07 s, exceeds the extra load time of 3.60 s. On local NVMe the disk is no longer the limit, the baseline drops to 9.42 s, which is the real cost of parsing and building 5 million objects, and 94% of the LZ4 time lands in the total. Plan for the upper end, because a well-provisioned node has fast storage and pays the full cost.

**Why the memory saving exceeds the byte saving, and why that matters for the net-savings guard (§6.4).** The payload fell 16%, from 511 to 429 bytes, but the allocation fell 30%. jemalloc rounds a request up to a bin, and the bins near these values are 384, 448, 512, 640, 768. An sds string costs the payload plus a 5-byte header plus a null terminator, so 517 bytes landed in the 640 bin and wasted 123, while 435 bytes landed in the 448 bin and wasted 13. Compression recovered 82 bytes of data **and** 110 bytes of rounding waste.

That was luck, and it does not generalise. A 300-byte payload rounds to the 320 bin; compress it by the same 16% and it still rounds to 320, so the CPU is spent and nothing is freed. **So the net-savings guard should compare the allocation the value will occupy, not its byte length.** §6.4 does not say that today.

**Two caveats on reading the table.** These are no-dictionary numbers, so -14.3% is the pessimistic ratio; a trained dictionary reached 13.2% of original on similar data at roughly the same CPU cost per byte. And LZ4 throughput is not constant even on one machine, 422 MB/s on the EBS run against 539 MB/s on NVMe, because blocking on I/O between compressions leaves caches colder.
