/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSOR_STATS_H
#define COMPRESSOR_STATS_H

/* The numbers behind INFO compression. See design-docs/inline-compression.md,
 * section 9.1, for what each one means.
 *
 * Gauges are values right now. The compressed_* gauges change when a value is
 * compressed, decompressed, or freed. A value can be freed on a background
 * thread (FLUSHALL ASYNC), so these three are atomic.
 *
 * Counters are totals since start. They change only on the main thread.
 * CONFIG RESETSTAT sets them back to 0.
 *
 * The two kinds live in two structs, so the reset clears only the counters
 * and never writes the gauges. A free on a background thread can therefore
 * never be lost during a reset. */

#include "sds.h"

/* Like server.h: C++ unit tests define _Atomic() themselves. */
#ifndef __cplusplus
#include <stdatomic.h>
#endif

typedef struct compressorGauges {
    _Atomic(unsigned long long) compressed_values;
    _Atomic(unsigned long long) compressed_values_original_bytes;
    _Atomic(unsigned long long) compressed_values_bytes;
} compressorGauges;

typedef struct compressorCounters {
    /* The sweeper. */
    unsigned long long keys_checked;
    unsigned long long keys_eligible;
    unsigned long long keys_skipped_already_compressed;
    unsigned long long keys_skipped_not_string;
    unsigned long long keys_skipped_in_use;
    unsigned long long keys_skipped_size;
    unsigned long long keys_skipped_hot;
    unsigned long long keys_skipped_already_queued;
    unsigned long long values_queued;
    unsigned long long values_compressed;
    unsigned long long values_compressed_and_dropped_low_saving;
    unsigned long long values_compressed_and_dropped_changed;
    unsigned long long values_compressed_and_dropped_now_skipped;
    unsigned long long compression_paused_during_save;
    /* The sum over all workers. With more than one worker, it can grow faster
     * than real time. */
    unsigned long long compression_time_us;

    /* Reads. */
    unsigned long long values_decompressed;
    unsigned long long temporary_copies_made;
    unsigned long long decompression_time_us;
} compressorCounters;

typedef struct compressorStats {
    compressorGauges gauges;
    compressorCounters counters;
} compressorStats;

extern compressorStats compressor_stats;

/* Adds or removes one compressed value from the gauges. original_bytes is
 * its plain size, and frame_bytes the memory of its frame. */
void compressorStatsAddValue(size_t original_bytes, size_t frame_bytes);
void compressorStatsRemoveValue(size_t original_bytes, size_t frame_bytes);

/* CONFIG RESETSTAT. Resets the counters, not the gauges. */
void compressorStatsReset(void);

/* Appends the "# Compression" section to info, and returns it. */
sds compressorStatsInfo(sds info);

#endif /* COMPRESSOR_STATS_H */
