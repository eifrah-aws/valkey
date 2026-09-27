# Inline Compression for Valkey

**Related issue:** [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423)

## Executive summary

Memory is the binding constraint for a meaningful subset of Valkey workloads. In these workloads, dataset size, not CPU or network capacity, drives node scaling, and storing redundant data such as JSON, HTML, and protobuf in its original form increases RAM requirements.

Inline compression would allow operators to reduce this memory footprint without changing client behavior or command semantics. The proposed v1 design stores writes as uncompressed bytes, compresses eligible cold string values asynchronously, and promotes a compressed value back to RAW on its first client read. A bounded hot-key LRU keeps recently read keys out of later compression sweeps. Compression remains disabled by default and observable through operator-facing metrics.

The motivating evidence is strong: **92% of memory-bound ElastiCache snapshots contain values between 256 B and 1 MiB that compress by at least 50% with a trained dictionary.** A unit test on approximately 267 B JSON-like values also reduced values to 13.2% of their original size with a dictionary, compared with 89.5% without one.

**Recommendation:** proceed with a deliberately constrained v1 for string values, while resolving the architecture, persistence, memory-bound, and product-scope decisions listed below. The primary review question is not whether inline compression can save memory, but whether the operational benefit justifies the implementation and stability cost.

## 1. Problem and customer need

Valkey currently stores every value uncompressed. For memory-bound workloads with redundant values, this means customers pay a direct RAM cost and may need to scale to another node even when CPU and network capacity remain available.

Application-side compression is possible, but it has material limitations:

- Every client and language must implement the same policy.
- Existing fleets require coordinated reader and writer changes.
- The server cannot inspect or report the logical value directly.
- Small values often contain too little repetition to compress effectively in isolation. A dictionary trained across the keyspace can compress these values, but individual clients cannot readily train and coordinate such a dictionary.

Inline compression addresses these limitations by making compression a server-managed, operator-controlled capability.

## 2. Goals and success criteria

The proposal has three goals:

1. **Reduce memory use** for memory-bound workloads without changing command semantics or requiring client changes.
2. **Provide operational control and visibility** so an operator can enable, measure, and disable the feature.
3. **Improve compression of small values** through keyspace-trained dictionaries where standalone application-side compression is ineffective.

The following are **targets, not measured results**. Phase 9 of the execution plan will validate them.

| Target | Success criterion |
|---|---|
| Memory reduction on a JSON-like dataset | At least 30% |
| Throughput impact under mixed read/write load | Less than 20% TPS reduction |
| Worst-case per-read decompression size | Bounded by `compression-max-value-size` (128 KiB by default) |

## 3. Proposed v1 scope

### In scope

- String values only
- Operator-controlled and disabled by default
- Background compression of eligible cold values
- Optional dictionary-based compression
- Detection and avoidance of common pre-compressed formats
- Net-savings validation before installing a compressed value
- One-time decompression and RAW promotion on the first client read
- Bounded hot-key LRU exclusion for later compression sweeps

### Out of scope and non-goals

- **Universal workload benefit:** CPU-bound, network-bound, and low-redundancy workloads may not benefit.
- **Guaranteed compression ratio:** results depend on the data; the system reports measured savings but promises no fixed ratio.
- **Replacement of application-side compression:** inline compression is an alternative for users who do not want compression policy in every client.
- **Tiered storage:** tiering moves bytes to a lower-cost medium; compression makes bytes smaller. The capabilities are complementary and may be used together.
- **Persistent compressed formats in v1:** under the current proposal, AOF, full sync, `DUMP`, and `MIGRATE` carry uncompressed bytes. RDB behavior remains a review decision.

## 4. Design overview

### 4.1 Write and background-compression path

Writes initially store uncompressed bytes. On each cron tick, the sweeper inspects at most 100 keys, selects values that are absent from the bounded hot-key LRU and cold by idle time or access frequency, and submits them to a small worker pool. It stops when either 100 requests or 32 MiB of owned snapshots are in flight. A worker compresses a private copy against the active dictionary. The main thread installs the compressed result only if the key has not changed in the interim.

Before copying or dispatching a value, the sweeper compares its leading bytes against signatures for common compressed formats. This low-cost `memcmp` check avoids unnecessary main-thread copying and worker CPU. The signature list is intentionally non-exhaustive because a net-savings guard discards any compressed frame that does not reduce the stored size.

| Format | Signature |
|---|---|
| PNG | `89 50 4E 47 0D 0A 1A 0A` |
| JPEG | `FF D8 FF` |
| GIF | `47 49 46 38` (`GIF8`) |
| WEBP | `52 49 46 46` (`RIFF`), followed by `57 45 42 50` (`WEBP`) at offset 8 |
| gzip | `1F 8B` |
| zip | `50 4B 03 04` |
| zstd | `28 B5 2F FD` |
| xz | `FD 37 7A 58 5A 00` |
| bzip2 | `42 5A 68` (`BZh`) |
| 7z | `37 7A BC AF 27 1C` |
| MP4 | `66 74 79 70` (`ftyp`) at offset 4 |
| base64 PNG | `iVBORw0KGgo` |
| base64 JPEG | `/9j/` |

### 4.2 Stored representation and read path

A compressed value is explicitly marked and includes the metadata needed to select its frame format, algorithm, and dictionary version. A zero dictionary ID represents dictionary-free compression.

On the first client read of a compressed value, Valkey synchronously decompresses it and replaces the stored frame with the RAW value. Every client read inserts or refreshes the key in a bounded hot-key LRU. The sweeper skips these entries and still requires the normal idle/LFU coldness check. Later reads avoid decompression, while values that cool can become eligible for compression again.

The initial v1 bound is 100,000 entries with a 60-second default lifetime. These values require benchmark validation because a list that is too small can allow compression/promotion churn, while a list that is too large adds tracking memory.

### 4.3 Persistence and replication boundary

The v1 design deliberately avoids committing future Valkey versions to a durable compressed frame format. AOF, full sync, `DUMP`, and `MIGRATE` operate on uncompressed bytes. Under the current design, a restart begins with uncompressed values and the sweeper restores compression over time.

Whether RDB should also remain uncompressed is an explicit review decision because it determines both restart behavior and long-term format compatibility.

## 5. Evidence available today

The evidence below is measured and should not be confused with the success targets in Section 2.

| Evidence | Observation | Implication |
|---|---|---|
| Memory-bound ElastiCache snapshots | On 92% of snapshots, values from 256 B to 1 MiB compress by at least 50% with a trained dictionary | A substantial portion of memory-bound workloads may benefit |
| LZ4 unit test on approximately 267 B JSON-like values | 89.5% of original size without a dictionary; 13.2% with a dictionary | Dictionary support is important for small values |
| RDB compress-on-load POC, 5M keys of 512 B, three hosts | Load time increases 22% to 54% depending on storage; `used_memory` falls 24% | Compressing during load is affordable. The cost depends on whether loading is bound by disk or by CPU. See Appendix A |

The second result directly addresses the question raised by `sarthakaggarwal97` in the GitHub issue about the value of dictionary-based compression.

## 6. Review decisions required

The following decisions are ordered by expected architectural or product impact.

| # | Decision | Why it matters | Proposed direction / required outcome |
|---|---|---|---|
| 1 | **Coordinate with data tiering or ship independently?** | Both features target the same release. Tiering requires asynchronous fetch, while the current proposal uses synchronous decompression. | Decide whether interface alignment is required before implementation. If shipping independently, document the rationale in the issue. |
| 2 | **What is the minimum viable design that still provides meaningful savings?** | The worker pool, dictionary retraining, bounded scanning, and queue backpressure add complexity. | Measure a synchronous, single-dictionary, no-retraining variant before defending the full design. |
| 3 | **Is the uncompressed memory peak during load acceptable?** | Loading does not compress; a restarted node or fresh replica remains at the uncompressed baseline until the sweeper catches up. Primary/replica pairs therefore cannot be sized below that baseline. The alternative is now measured: compressing during load costs 22% to 54% more load time and returns the memory immediately (Appendix A). | Confirm this as an accepted v1 limitation, now that the price of removing it is known. |
| 4 | **Should RDB contain compressed values or compress values during load?** | Compressed RDB preserves savings across restart but makes the frame format a permanent compatibility obligation. Plain RDB permits format evolution but incurs a full uncompressed memory peak during load. Measured for compress-during-load: load time rises 22% to 54% and `used_memory` falls 24% (Appendix A). | Choose between durable format compatibility and transient restart/load overhead. The current v1 direction is plain RDB. |
| 5 | **What bounds read-promotion memory?** | One read promotes one compressed value to RAW, bounded by `compression-max-value-size` (128 KiB by default). A wide `MGET`, script, or scan can promote many values and move the keyspace toward the uncompressed baseline until those keys cool and are compressed again. | Measure promotion volume and recompression convergence. Expose hot-key LRU and promotion counters before claiming predictable memory savings. |
| 10 | **Should the net-savings guard measure allocation size or byte length?** | Appendix A shows the saving is set by allocator size classes, not by the compression ratio. A frame 16% smaller in bytes can land in the same bin and free nothing, so the CPU is spent for no gain. | Specify the guard in terms of the allocation the value will occupy, not its length. |
| 6 | **Should compression use a dedicated worker pool?** | Reusing I/O threads or BIO may reduce thread-management complexity, but the suitability and isolation trade-offs are unresolved. | Select the execution model before phase 5. |
| 7 | **Does the benefit justify the operational complexity?** | DarrenJiang13 reported that Alibaba previously abandoned a similar feature because the cloud-provider benefit was limited and stability work increased. | Address this experience directly with workload evidence, operational-cost analysis, and clear adoption criteria. |
| 8 | **How can users evaluate suitability before enabling the feature?** | Operators currently have no dry-run mode or analyzer. | Decide whether v1 requires an estimator, sampling mode, or preflight analyzer. |
| 9 | **Should compression be configurable by database or key prefix?** | The current design filters only by value size and coldness. | Decide whether global configuration is sufficient for v1 or workload-level scoping is required. |

## 7. Current alignment

The overall direction has precedent and support:

- `zuiderkwast` proposed the same core model: store writes uncompressed and compress cold values in the background.
- `hpatro` noted that quicklist already applies a comparable size-threshold approach.
- The unresolved issue is therefore not the basic direction, but the appropriate implementation scope and operational complexity.

## 8. Recommended review outcome

Approve continued work on a constrained, opt-in v1 only if the review reaches agreement on the following points:

1. The minimum viable architecture and threading model
2. Coordination boundaries with data tiering
3. RDB behavior and compatibility commitments
4. Measured read-promotion memory and recompression convergence
5. Acceptance of the uncompressed restart and replica-sizing baseline
6. Evidence that projected memory savings justify the operational and maintenance cost

This outcome preserves the customer value of transparent memory reduction while preventing unresolved persistence, memory, and complexity risks from becoming implicit commitments.

## Appendix A: measured cost of compressing during RDB load

Source: `run-rdb-poc.sh` on branch `valkey-inline-compression-rdb-poc`. 5,000,000
keys, 512-byte JSON-like values, LZ4 with **no dictionary**, three runs of each load
per host. The same generated data on every host, so the byte figures are identical
and only the environment differs.

| | macOS arm64, libc, page cache | Linux, jemalloc, EBS gp3 | Linux, jemalloc, local NVMe |
|---|---|---|---|
| Baseline load | 7.20 s | 16.65 s | 9.42 s |
| Compress on load | 10.38 s | 20.25 s | 14.47 s |
| **Load time change** | **+44.2%** | **+21.6%** | **+53.7%** |
| `used_memory` | 3.27 -> 2.48 GB, -24.2% | 3.24 -> 2.45 GB, -24.4% | 3.24 -> 2.45 GB, -24.4% |
| `used_memory_rss` | -25.1% | -23.8% | -23.8% |
| Value bytes | -14.3% | -14.3% | -14.3% |
| Time inside LZ4 | 3.21 s, 101% of delta | 6.07 s, 169% of delta | 4.75 s, 94% of delta |

**The cost is a range, not a number: +22% to +54%.** The two Linux columns are the
same machine, the same allocator, and the same data, differing only in storage. On
EBS gp3 the baseline reads 2.1 GB at about 136 MB/s, so loading waits on the disk and
part of the compression happens inside that wait. That is why the time inside LZ4,
6.07 s, exceeds the extra load time of 3.60 s. On local NVMe the disk is no longer the
limit, the baseline drops to 9.42 s, which is the real cost of parsing and building
5 million objects, and 94% of the LZ4 time lands in the total. **Plan for the upper
end**, because a well-provisioned node has fast storage and pays the full cost.

**Why the memory saving exceeds the byte saving.** The payload fell 16%, from 511 to
429 bytes, but the allocation fell 30%. jemalloc rounds a request up to a bin, and
the bins near these values are 384, 448, 512, 640, 768. An sds string costs the
payload plus a 5-byte header plus a null terminator, so 517 bytes landed in the 640
bin and wasted 123, while 435 bytes landed in the 448 bin and wasted 13. Compression
recovered 82 bytes of data **and** 110 bytes of rounding waste.

That was luck, and it does not generalise. A 300-byte payload rounds to the 320 bin;
compress it by the same 16% and it still rounds to 320, so the CPU is spent and
nothing is freed. This is the basis of review decision 10.

**Two caveats on reading the table.** These are no-dictionary numbers, so the -14.3%
is the pessimistic ratio; the unit test in section 5 measured 13.2% of original with a
dictionary on similar data, at roughly the same CPU cost per byte. And LZ4 throughput
is not constant even on one machine, 422 MB/s on the EBS run against 539 MB/s on
NVMe, because blocking on I/O between compressions leaves caches colder.
