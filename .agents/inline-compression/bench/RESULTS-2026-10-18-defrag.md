# Inline compression benchmark with active defrag, 2026-10-18

Run `results/20261018-171301` (`./compression-bench.sh -s scenarios-defrag.txt -w 1200`), same build and setup as `RESULTS-2026-10-18.md`, but with `--activedefrag yes` (default thresholds) and a 1200 s watch in place of 600 s. All numbers come from the raw files and are checked by `analyze.py`.

## Findings

1. **Defrag brings RSS down when only part of the values is compressed.** At 603 s, `cursor-if100-defrag` has 511,499 values compressed, `used_memory` 1.60 GiB and RSS 1.82 GiB (fragmentation 1.14). In the first run without defrag, `cursor-if100` at 600 s had 487,583 values compressed, `used_memory` 1.63 GiB and RSS 2.52 GiB (fragmentation 1.55).
2. **With all values compressed, RSS is the same with or without defrag:** 1.13 GiB after the watch (-52.8% against RSS after SET), fragmentation 1.06.
3. **After the full read, RSS goes back to the baseline:** 2.29 GiB (`cursor-if100-defrag`) and 2.28 GiB (`cursor-ct4-defrag`), against 2.30 GiB for `baseline-off-defrag`. Without defrag, RSS ended above the baseline: 3.21, 3.14 and 2.87 GiB.
4. **Defrag cost.** `total_active_defrag_time` (time with fragmentation over the limit) during the watch: 221,807 ms (`cursor-if100-defrag`) and 144,792 ms (`cursor-ct4-defrag`), with 644,396 and 619,242 hits; until the end: 332,136 ms and 254,448 ms. Main-thread CPU in the 1200 s watch: 18.00 s and 11.67 s, against 4.65 s for `baseline-off-defrag`. The baseline did no defrag work (0 hits, fragmentation 1.06).
5. **`cursor-if100-defrag` (max 100 in-flight jobs) compresses all values in the longer watch:** t50 593 s, t90 1002 s, t99 1089 s. `cursor-ct4-defrag` reaches t99 in 374 s (367 s in the first run).
6. **SET is not affected:** 913,016, 895,037 and 902,469 rps, p99 2.911, 2.927 and 2.911 ms.
7. **GET latency is noisy between runs.** `baseline-off-defrag` did no defrag work, but its GET p99 with all keys is 1.375 ms, against 0.457 ms for `baseline-off` in the first run. `cursor-ct4-defrag` has p99 1.919 ms with all keys (1.383 ms in the first run), so the first `cursor-ct4` result was not a real gain. Repeated runs are needed for differences below about 1 ms.

<!-- tables:start -->

### Scenarios

Each scenario changes one setting from cursor-base. All runs also pass `--save '' --protected-mode no --maxmemory-policy allkeys-lfu`.

| Scenario | io-threads | compression-mode | compression-threads | max-inflight-requests | sweeper-policy | sweeper-samples | backoff-seconds |
|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 12 | off | - | - | - | - | - |
| `cursor-if100-defrag` | 12 | lz4 | 12 | 100 | cursor | 500 | 60 |
| `cursor-ct4-defrag` | 12 | lz4 | 4 | 1000 | cursor | 500 | 60 |

### Memory

In GiB (1 GiB = 1024^3 bytes, the unit of INFO `used_memory_human`). `used_memory` and `used_memory_rss` after the SET load, after the 1200 s watch, and at the end (after the GET runs). The changes are against `baseline-off-defrag` after SET, computed from the bytes. `mem_fragmentation_ratio` is RSS / used_memory, from INFO. The `compressed_*` columns are after the watch. `activedefrag` was on in: `baseline-off-defrag`, `cursor-if100-defrag`, `cursor-ct4-defrag`.

| Scenario | used_memory after SET | used_memory after watch | used_memory change | RSS after SET | RSS after watch | RSS change | fragmentation after watch | compressed_values | compressed_values_original_bytes | compressed_values_bytes | compression_saved_bytes | used_memory at end | RSS at end | fragmentation at end |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 2.17 GiB | 2.17 GiB | 0.0% | 2.39 GiB | 2.30 GiB | -4.0% | 1.06 | 0 | 0.00 GiB | 0.00 GiB | 0.00 GiB | 2.17 GiB | 2.30 GiB | 1.06 |
| `cursor-if100-defrag` | 2.17 GiB | 1.06 GiB | -50.9% | 2.50 GiB | 1.13 GiB | -52.8% | 1.06 | 1000000 | 1.90 GiB | 1.02 GiB | 0.89 GiB | 2.17 GiB | 2.29 GiB | 1.06 |
| `cursor-ct4-defrag` | 2.17 GiB | 1.06 GiB | -51.0% | 2.52 GiB | 1.13 GiB | -52.8% | 1.06 | 1000000 | 1.90 GiB | 1.01 GiB | 0.88 GiB | 2.16 GiB | 2.28 GiB | 1.06 |

### Sweeper, during the 1200 s watch

t50/t90/t99: seconds after the end of the SET load until `compressed_values` reached 50/90/99% of 1000000 (the best value of the run); "-" means not reached in 1200 s. CPU is in seconds, from INFO. The last column counts the decompressions of all 4 GET runs.

| Scenario | t50 s | t90 s | t99 s | keys_checked | keys_eligible | passes | backoffs | main-thread CPU s | total CPU s | worker compression s | values decompressed by GETs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | - | - | - | 0 | 0 | 0 | 0 | 4.65 | 54.05 | 0.00 | 0 |
| `cursor-if100-defrag` | 593 | 1002 | 1089 | 2033641 | 1000000 | 2 | 0 | 18.00 | 99.45 | 4.55 | 1074529 |
| `cursor-ct4-defrag` | 236 | 317 | 374 | 5384717 | 1001824 | 5 | 2 | 11.67 | 58.44 | 4.24 | 1139334 |

### SET load

5M requests (4,999,680 after sb rounds to 512 connections), 1M keys, `github-events` values. Latency in ms, as sb reports it.

| Scenario | rps | pmin | p50 | p90 | p95 | p99 | p995 | p999 | pmax |
|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 913016 | 0.073 | 0.479 | 0.651 | 0.979 | 2.911 | 3.039 | 3.935 | 79.359 |
| `cursor-if100-defrag` | 895037 | 0.075 | 0.483 | 0.659 | 1.003 | 2.927 | 3.023 | 3.903 | 83.455 |
| `cursor-ct4-defrag` | 902469 | 0.068 | 0.483 | 0.683 | 1.007 | 2.911 | 3.071 | 4.159 | 80.383 |

### GET, `--touch-keys 10` (the first 10% of the keys)

5M requests, `--limit-rps 100000`. Latency in ms, as sb reports it. "compressed before" is `compressed_values` when the run starts; "decompressed" is the number of values this run decompressed.

| Scenario | rps | pmin | p50 | p90 | p95 | p99 | p995 | p999 | pmax | hits | compressed before | decompressed |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 99963 | 0.065 | 0.281 | 0.635 | 0.739 | 0.923 | 0.999 | 1.167 | 5.151 | 4999680 | 0 | 0 |
| `cursor-if100-defrag` | 99943 | 0.065 | 0.373 | 1.103 | 1.303 | 1.687 | 1.935 | 2.495 | 5.599 | 4999680 | 1000000 | 102002 |
| `cursor-ct4-defrag` | 99955 | 0.065 | 0.315 | 1.167 | 1.327 | 1.615 | 1.791 | 2.463 | 6.175 | 4999680 | 1000000 | 101969 |

### GET, `--touch-keys 30` (the first 30% of the keys)

5M requests, `--limit-rps 100000`. Latency in ms, as sb reports it. "compressed before" is `compressed_values` when the run starts; "decompressed" is the number of values this run decompressed.

| Scenario | rps | pmin | p50 | p90 | p95 | p99 | p995 | p999 | pmax | hits | compressed before | decompressed |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 99939 | 0.064 | 0.157 | 0.305 | 0.339 | 0.439 | 0.551 | 0.731 | 3.855 | 4999680 | 0 | 0 |
| `cursor-if100-defrag` | 99951 | 0.065 | 0.177 | 0.361 | 0.505 | 0.967 | 1.239 | 1.943 | 2.831 | 4999680 | 900000 | 211498 |
| `cursor-ct4-defrag` | 99975 | 0.065 | 0.194 | 0.519 | 0.747 | 1.287 | 1.471 | 1.863 | 4.799 | 4999680 | 900000 | 214761 |

### GET, `--touch-keys 50` (the first 50% of the keys)

5M requests, `--limit-rps 100000`. Latency in ms, as sb reports it. "compressed before" is `compressed_values` when the run starts; "decompressed" is the number of values this run decompressed.

| Scenario | rps | pmin | p50 | p90 | p95 | p99 | p995 | p999 | pmax | hits | compressed before | decompressed |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 99967 | 0.065 | 0.331 | 0.783 | 0.935 | 1.239 | 1.343 | 1.575 | 5.279 | 4999680 | 0 | 0 |
| `cursor-if100-defrag` | 99955 | 0.065 | 0.373 | 0.943 | 1.111 | 1.679 | 2.007 | 2.655 | 5.503 | 4999680 | 700000 | 218924 |
| `cursor-ct4-defrag` | 99961 | 0.066 | 0.489 | 1.103 | 1.247 | 1.671 | 1.943 | 2.479 | 6.367 | 4999680 | 700000 | 224827 |

### GET, all keys (no `--touch-keys`)

5M requests, `--limit-rps 100000`. Latency in ms, as sb reports it. "compressed before" is `compressed_values` when the run starts; "decompressed" is the number of values this run decompressed.

| Scenario | rps | pmin | p50 | p90 | p95 | p99 | p995 | p999 | pmax | hits | compressed before | decompressed |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `baseline-off-defrag` | 99965 | 0.065 | 0.449 | 0.947 | 1.103 | 1.375 | 1.463 | 1.679 | 5.151 | 4999680 | 0 | 0 |
| `cursor-if100-defrag` | 99941 | 0.064 | 0.2 | 0.679 | 0.927 | 1.799 | 2.127 | 2.703 | 5.727 | 4999680 | 500000 | 542105 |
| `cursor-ct4-defrag` | 99961 | 0.065 | 0.236 | 0.683 | 0.899 | 1.919 | 2.303 | 2.815 | 5.727 | 4999680 | 500000 | 597777 |

<!-- tables:end -->
