# RDB compression POC

Throwaway code on branch `valkey-inline-compression-rdb-poc`. Do not merge.

It answers one question from the inline compression design: **what does it cost to
compress each value while an RDB loads, instead of loading plain and letting a
background sweeper compress afterwards?** That is open question 4 in the "Why We
Are Building This" page.

## What it measures

- **Step 1, baseline.** Load an RDB as usual. Report load time and `used_memory`.
- **Step 2.** Load the same RDB, compressing each string value with LZ4 on the way
  in. Report load time, `used_memory`, and the compression ratio.

## What it does not do

There is no frame header, no new object encoding, and nothing marks a value as
compressed. After step 2 the keyspace holds raw LZ4 output, so reads return
garbage. That is deliberate: the POC measures time and memory, nothing else.

No dictionary either. LZ4 has no trainer, so this is the plain no-dictionary case.
That matters when reading the ratio: a trained dictionary shrinks small values far
more, which is the whole reason the design has one.

## Running it

```bash
make -C src -j8                      # build valkey-server and valkey-cli
./utils/rdb-poc/run-poc.sh           # 5M keys, 512-byte values, port 7777
```

Environment overrides: `KEYS`, `VALUE_SIZE`, `PORT`, `WORK`, and `REUSE_RDB=0` to
rebuild the RDB instead of reusing the one in the work directory.

A smaller run for a quick check:

```bash
KEYS=50000 REUSE_RDB=0 WORK=/tmp/poc PORT=7791 ./utils/rdb-poc/run-poc.sh
```

## Results

5,000,000 keys, 512-byte JSON-like values, LZ4 with no dictionary. macOS, libc
malloc, `-O3 -flto`. RDB on disk is 2.1 GB, and the fill took 127 seconds. Three
runs, with the RDB in the page cache for all of them.

| | Baseline | Compress on load | Change |
|---|---|---|---|
| Load time | 7.22 / 7.01 / 6.86 s | 10.66 / 10.51 / 10.36 s | +3.5 s, about +50% |
| `used_memory` | 3.27 GB | 2.48 GB | -849 MB, -24% |
| Value bytes | 2,559,312,928 | 2,192,514,923 | -14.3% |
| Time inside LZ4 | - | 3.22 s | 92% of the extra load time |

Read these four things out of it:

1. **Load takes half again as long.** 7.0 s becomes 10.5 s on this dataset. The
   cost scales with the data, not with the key count alone: 3.2 s of LZ4 for
   2.56 GB of values is about 800 MB/s, or 0.64 us per 512-byte value.
2. **LZ4 is nearly all of the cost.** 3.22 s of the 3.5 s delta is inside
   `LZ4_compress_default`. The rest is allocation churn from freeing the plain
   value and allocating the compressed one.
3. **The memory saving is bigger than the byte saving**, 24% against 14.3%,
   because the compressed values cross into a smaller allocator size class. Do not
   expect that to hold at other value sizes. See "Reading the memory number".
4. **This is the pessimistic ratio.** LZ4 has no dictionary here. On the same
   shape of data, the unit test on the compressor branch measured 89.5% of original
   without a dictionary and 13.2% with one. A dictionary costs the same time per
   byte, so the load-time penalty would stay near 50% while the saving would be far
   larger. Anyone using this number to judge the feature should know that.

## The data shape decides the ratio

`gen-load.py` writes JSON-like records. Field names repeat across values, which is
the redundancy a trained dictionary would exploit. Field contents differ per key,
and the free-text field is drawn from a word list, so there is little repetition
inside a single value. That is what real JSON looks like.

Two shapes to avoid, because both make the POC lie:

- Random bytes do not compress. The result would be zero saving and would say
  nothing about real data.
- A short repeated filler pattern compresses far better than real data. An early
  version of this script repeated a 12-byte pattern about 40 times per value and
  reported 53% of plain. With realistic text the same code reports about 86%.

## One trap found while writing this

Allocating `LZ4_compressBound()` bytes and shrinking afterwards with `sdsResize()`
does **not** return the memory on macOS with libc malloc. `realloc` keeps the
original block, so the value still occupies the worst-case size and `used_memory`
does not move at all, even though the bytes did shrink.

The POC compresses into a reused scratch buffer and then allocates the sds at the
exact size. The design's section 5.4 already says the frame must be shrunk to its
real length; this is a concrete case where the obvious way of doing that fails.

## Reading the memory number

The saving is lumpy, not proportional. Measured on 512-byte values: the value went
from 511 to 429 bytes, a 16% cut, while `MEMORY USAGE` for the key went from 672 to
480, a 29% cut. The compressed size crossed into a smaller allocator size class.
A different value size may cross no boundary and save nothing at all.

## Files

| File | What it is |
|---|---|
| `run-poc.sh` | Driver. Fills the keyspace, saves the RDB, then runs both steps. |
| `gen-load.py` | Generates RESP `SET` commands on stdout for `valkey-cli --pipe`. |

The server change is small and marked. Search for `POC only` in `src/rdb.c`,
`src/rdb.h`, `src/server.c`, `src/server.h`, and `src/config.c`. The switch is
`rdb-poc-compress-on-load`, default `no`.
