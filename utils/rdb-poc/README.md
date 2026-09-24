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

One command, from the repo root. It builds, fills, saves, and runs both steps.

```bash
./run-rdb-poc.sh
```

A smaller run for a quick check:

```bash
KEYS=40000 REPEATS=2 REUSE_RDB=0 WORK=/tmp/poc ./run-rdb-poc.sh
```

Knobs, all optional:

| Variable | Default | What it does |
|---|---|---|
| `KEYS` | 5000000 | number of keys |
| `VALUE_SIZE` | 512 | bytes per value |
| `REPEATS` | 3 | how many times to run each load |
| `PORT` | 6379 | server port |
| `WORK` | `<repo>/.poc-run` | scratch directory for the RDB and the logs |
| `REUSE_RDB` | 1 | set 0 to rebuild the RDB instead of reusing it |
| `SKIP_BUILD` | 0 | set 1 to skip the build |
| `MALLOC` | unset | passed to make. Unset takes the platform default: jemalloc on Linux, libc on macOS. |

### Two safety rules in the script

The script runs `FLUSHALL`, so pointing it at the wrong server destroys data. Two
checks stop that, and both exist because it happened during development.

1. **It stops every `valkey-server` on the host before starting.** It lists what it
   kills. Then, if the port is still taken by something that is not a
   `valkey-server`, it reports what holds the port and exits. It never moves to
   another port silently.
2. **After every start it proves the server is ours,** before any command that
   writes. It compares `process_id` against the pid it launched, compares `dir`
   against the work directory, and checks that `rdb-poc-compress-on-load` exists,
   which only this build has. Any mismatch aborts.

### Running on Linux with jemalloc

Nothing special to do. `make` picks jemalloc on Linux, and the script reports
`mem_allocator` in the summary so the numbers are comparable across hosts. The
allocator matters a lot here, because part of the memory saving comes from
compressed values landing in a smaller size class, and jemalloc's bins differ from
libc's. Expect the `used_memory` change to differ from the macOS numbers below even
on identical data.

## Results

5,000,000 keys, 512-byte JSON-like values, LZ4 with no dictionary. macOS arm64,
libc malloc, `-O3 -flto`, 12 cores. RDB on disk is 2.1 GB, and the fill took about
2 minutes. Three runs of each load, with the RDB in the page cache throughout.

```
    keys=5000000 value_size=512 allocator=libc runs=3
                                 baseline       compress       change
    load time (mean)                7.20s         10.38s      +44.2%
    used_memory                   3.27 GB        2.48 GB      -24.2%
    used_memory_rss               3.36 GB        2.52 GB      -25.1%
    value bytes                   2.38 GB        2.04 GB      -14.3%
    time inside LZ4                     -           3.21s 101% of delta
```

Read these four things out of it:

1. **Load takes about 44% longer.** 7.20 s becomes 10.38 s on this dataset. The
   cost scales with the bytes, not with the key count alone: 3.21 s of LZ4 for
   2.56 GB of values is about 800 MB/s, or 0.64 us per 512-byte value.
2. **LZ4 is the whole cost.** The time inside `LZ4_compress_default` matches the
   extra load time to within measurement noise, hence the "101% of delta". There is
   no meaningful overhead beyond the compression itself.
3. **The memory saving is bigger than the byte saving**, 24.2% against 14.3%,
   because the compressed values cross into a smaller allocator size class. Do not
   expect that to hold at other value sizes, or under a different allocator. See
   "Reading the memory number".
4. **This is the pessimistic ratio.** LZ4 has no dictionary here. On the same shape
   of data, the unit test on the compressor branch measured 89.5% of original
   without a dictionary and 13.2% with one. A dictionary costs about the same time
   per byte, so the load-time penalty would stay near 44% while the saving would be
   far larger. Anyone using this number to judge the feature should know that.

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
| `../../run-rdb-poc.sh` | The driver, at the repo root. Builds, fills, saves, runs both steps, prints a summary. |
| `gen-load.py` | Generates RESP `SET` commands on stdout for `valkey-cli --pipe`. |

The server change is small and marked. Search for `POC only` in `src/rdb.c`,
`src/rdb.h`, `src/server.c`, `src/server.h`, and `src/config.c`. The switch is
`rdb-poc-compress-on-load`, default `no`.
