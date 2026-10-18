# Inline compression for Valkey - overview

Branch `valkey-inline-compression` (fork `eifrah-aws/valkey`). Design: `design-docs/inline-compression.md`. Goal: at least 30% less memory for string values, at most 20% throughput cost, opt-in, invisible to clients.

## Approach

- **Background, not inline.** Writes store plain values as today. A sweeper on the main thread (`compressorCron()`, 10 calls per second) finds cold string values, copies each one into a job, and worker threads compress the copy with LZ4.
- **Cold means not used recently,** read from the object's own LRU/LFU field (LFU counter at most `compression-lfu-threshold`, or idle for 60 s); the sweeper never touches that field.
- **Cursor scan.** The default policy walks all keys with `kvstoreScan()` and keeps its place between calls, so one pass checks each key one time; each call checks at most `compression-sweeper-samples` keys and buckets, and pauses for `compression-sweeper-backoff-seconds` after a pass that finds almost nothing. A `random` policy is kept for comparison.
- **Safe install.** The main thread installs a result only if the key still holds the same bytes as the copy (`memcmp`), the value is still cold, and the frame saves at least 10% of the allocated memory; no key is locked, so no client waits.
- **Frame.** A compressed value is the same `robj` with the new encoding `OBJ_ENCODING_COMPRESSED` and a 10-byte header (length, dictionary id, algorithm, format version).
- **Reads.** `lookupKey()` decompresses in place, so commands do not change; during a child process, or for a module read handle, the read gets a temporary plain copy and the stored frame stays.

## Implemented

1. LZ4 backend behind a replaceable compressor interface, with one compressor instance per thread.
2. Compression frame with a 10-byte self-describing header and frame checks.
3. Settings: mode, threads, value size range, max in-flight jobs, LFU threshold, sweeper policy, samples and backoff.
4. New object encoding and a read path in `lookupKey()`, with temporary copies during a child process and for module read handles.
5. Worker pool, in-flight set and byte compare at install.
6. `cursor` sweeper with resume inside a bucket, backoff, and the `random` policy.
7. `INFO compression` with gauges and counters that add up.
8. Commands that do not read the bytes keep the value compressed (`TYPE`, `TTL`, `EXPIRE`, `RENAME`, `MOVE`, `COPY`, `DUMP`, `SET`, ...).
9. Audit of all direct encoding reads, with fixes for `PFCOUNT`, module DMA, temporary copies in scripts, and existence-only lookups.
10. Slot migration export sends plain bytes from the snapshot child, without changes to the source, with a cluster test.
11. `DEBUG COMPRESS-KEY` to compress one key now, for tests.
12. Unit tests, Tcl and module tests, and a benchmark script with a checker (`bench/`).

## Still to do

1. Set the defaults from the benchmark: keep `cursor` with 500 samples, and raise max in-flight jobs from 100 to 1000.
2. Design a lower read cost: decompress in the I/O threads, or keep the frame on a read of a cold key.
3. Skip values that are already compressed (PNG, JPEG, gzip, ...) by their first bytes.
4. Check the sweeper with `CLIENT PAUSE WRITE` and on replicas, and add the missing sweeper tests.
5. Save and load frames as they are in RDB and the AOF preamble (new RDB type).
6. Compress values on slot-migration import and on replica apply, to avoid a memory spike.
7. Run the full measurements of the design TODOs.
8. Write the user documentation (`valkey.conf`, INFO fields), and recommend `activedefrag yes` with compression, so RSS follows `used_memory`.
9. Add the compression dictionary registry, for better ratios on small values.
10. Add the zstd backend and dictionary training.
11. Support forkless save, after fixing the three problems the audit found.

<div style="page-break-after: always;"></div>

## Benchmark (2026-10-18, build `e89a8c20c`)

**Setup.** Server: 64 cores, 123.52 GiB, `hz 10`, `--io-threads 12`, `allkeys-lfu`. Client: sb, 512 connections, 16 threads. 5M `SET` over 1M keys (`github-events` JSON, 2,044 bytes on average, 1.90 GiB of values), 10 minutes without load, then 5M `GET` at a limit of 100,000 rps over 10%, 30%, 50% and 100% of the keys. Compressed scenarios: `lz4`, 12 compression threads, max 1000 in-flight jobs, unless the name says otherwise. All numbers below come from the raw files and are checked by `bench/analyze.py`; full tables: `bench/RESULTS-2026-10-18.md`.

| Scenario | used_memory / RSS after watch | compressed | t99 | keys checked | SET rps / p99 ms | GET all keys: rps / p50 / p90 / p99 / p999 ms |
|---|---|---|---|---|---|---|
| `baseline-off` | 2.17 / 2.40 GiB | 0 | - | 0 | 892003 / 2.959 | 99959 / 0.168 / 0.315 / 0.457 / 0.727 |
| `cursor-base` (500 samples) | 1.07 / 1.13 GiB | 1000000 | 362 s | 2800451 | 883023 / 3.023 | 99961 / 0.202 / 0.803 / 2.831 / 3.647 |
| `cursor-samples2000` | 1.06 / 1.13 GiB | 1000000 | 209 s | 6310632 | 907712 / 2.927 | 95738 / 0.247 / 0.707 / 10.559 / 11.455 |
| `cursor-if100` (max 100 jobs) | 1.63 / 2.52 GiB | 487583 | not reached | 1070211 | 898738 / 2.943 | 99951 / 0.248 / 0.687 / 1.519 / 2.143 |
| `random-base` (500 samples) | 1.18 / 2.07 GiB | 897046 | not reached | 2988000 | 906067 / 2.911 | 99959 / 0.183 / 0.397 / 2.383 / 3.247 |
| `random-samples2000` | 1.06 / 1.13 GiB | 999895 | 352 s | 11802752 | 923813 / 2.863 | 97285 / 0.214 / 0.647 / 8.895 / 10.751 |

t99: seconds after the load until 99% of the 1,000,000 values are compressed (600 s window).

**Results.**
- **Memory:** -50.8% `used_memory` (2.17 -> 1.07 GiB), and RSS follows (2.47 -> 1.13 GiB) when all values are compressed. With 49% or 90% compressed, RSS stays at 2.52 GiB or 2.07 GiB, because freed values leave holes in pages (active defrag off).
- **Sweeper:** `cursor` compresses 99% in 362 s with 2,800,451 checks; `random` needs 11,802,752 checks for 99% (2000 samples) and reaches only 89.7% with 500 samples. The workers use 4.43 s of CPU for 1M values, and the sweeper 9.01 s of main-thread CPU in 600 s. A limit of 100 in-flight jobs halves the speed.
- **SET** is not affected: 883,023 rps and p99 3.023 ms against 892,003 rps and 2.959 ms.
- **GET** pays for each first read of a compressed key: with all keys read (597,825 decompressions), p99 is 2.831 ms against 0.457 ms. With 2000 samples, keys are compressed again between the runs (883,750 decompressions), p99 is 10.559 ms, and the rate drops to 95,738 rps. A full read brings memory back to 2.17 GiB.
- **Active defrag** (second run, 1200 s watch, `bench/RESULTS-2026-10-18-defrag.md`) fixes RSS with partial compression: `cursor-if100-defrag` at 603 s has 511,499 values compressed, `used_memory` 1.60 GiB and RSS 1.82 GiB, against 2.52 GiB RSS without defrag. After the full read, RSS ends at 2.29 GiB and 2.28 GiB, against 2.30 GiB for the baseline (3.21, 3.14 and 2.87 GiB without defrag). Main-thread CPU in the 1200 s watch is 18.00 s (`cursor-if100-defrag`) and 11.67 s (`cursor-ct4-defrag`), against 4.65 s for the baseline.
- **Baselines are noisy** (GET p99 with all keys: 0.457, 1.055 and 0.943 ms for io-threads 12, 6 and 24, and 1.375 ms for the baseline with defrag, which did no defrag work), so differences below about 1 ms need repeated runs.
