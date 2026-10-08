/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The numbers behind INFO compression. See compressor_stats.h. */

#include "server.h"
#include "compressor/compressor_stats.h"
#include "compressor/compressor_object.h"
#include "compressor/compressor_workers.h"

#include <string.h>

compressorStats compressor_stats;

void compressorStatsAddValue(size_t original_bytes, size_t frame_bytes) {
    compressorGauges *g = &compressor_stats.gauges;
    atomic_fetch_add_explicit(&g->compressed_values, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->compressed_values_original_bytes, original_bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->compressed_values_bytes, frame_bytes, memory_order_relaxed);
}

void compressorStatsRemoveValue(size_t original_bytes, size_t frame_bytes) {
    compressorGauges *g = &compressor_stats.gauges;
    atomic_fetch_sub_explicit(&g->compressed_values, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g->compressed_values_original_bytes, original_bytes, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g->compressed_values_bytes, frame_bytes, memory_order_relaxed);
}

void compressorStatsReset(void) {
    /* Only the counters. The gauges describe the data, not the time since a
     * reset, and a background free may update them at any moment. */
    memset(&compressor_stats.counters, 0, sizeof(compressor_stats.counters));
}

sds compressorStatsInfo(sds info) {
    compressorGauges *g = &compressor_stats.gauges;
    compressorCounters *st = &compressor_stats.counters;
    unsigned long long values = atomic_load_explicit(&g->compressed_values, memory_order_relaxed);
    unsigned long long original = atomic_load_explicit(&g->compressed_values_original_bytes, memory_order_relaxed);
    unsigned long long frames = atomic_load_explicit(&g->compressed_values_bytes, memory_order_relaxed);
    unsigned long long dropped = st->compression_total_values_dropped_low_saving +
                                 st->compression_total_values_dropped_changed +
                                 st->compression_total_values_dropped_not_eligible;

    return sdscatprintf(
        info,
        "# Compression\r\n"
        "compression_mode:%s\r\n"
        "compression_threads:%d\r\n"
        "compressed_values:%llu\r\n"
        "compressed_values_original_bytes:%llu\r\n"
        "compressed_values_bytes:%llu\r\n"
        "compression_saved_bytes:%llu\r\n"
        "compression_jobs_in_flight:%lu\r\n"
        "compression_temporary_copies:%lu\r\n"
        "compression_total_keys_checked:%llu\r\n"
        "compression_total_keys_eligible:%llu\r\n"
        "compression_total_keys_skipped_already_compressed:%llu\r\n"
        "compression_total_keys_skipped_not_string:%llu\r\n"
        "compression_total_keys_skipped_in_use:%llu\r\n"
        "compression_total_keys_skipped_size:%llu\r\n"
        "compression_total_keys_skipped_hot:%llu\r\n"
        "compression_total_keys_skipped_already_queued:%llu\r\n"
        "compression_total_jobs_queued:%llu\r\n"
        "compression_total_values_compressed:%llu\r\n"
        "compression_total_values_dropped:%llu\r\n"
        "compression_total_values_dropped_low_saving:%llu\r\n"
        "compression_total_values_dropped_changed:%llu\r\n"
        "compression_total_values_dropped_not_eligible:%llu\r\n"
        "compression_total_sweeps_paused_by_child:%llu\r\n"
        "compression_total_compression_time_us:%llu\r\n"
        "compression_total_values_decompressed:%llu\r\n"
        "compression_total_temporary_copies_made:%llu\r\n"
        "compression_total_decompression_time_us:%llu\r\n",
        compressorAlgIdName(server.compression_mode), server.compression_threads, values, original, frames,
        original > frames ? original - frames : 0, compressorJobsInFlight(), compressor_plain_copies, st->compression_total_keys_checked,
        st->compression_total_keys_eligible, st->compression_total_keys_skipped_already_compressed, st->compression_total_keys_skipped_not_string, st->compression_total_keys_skipped_in_use, st->compression_total_keys_skipped_size,
        st->compression_total_keys_skipped_hot, st->compression_total_keys_skipped_already_queued, st->compression_total_jobs_queued, st->compression_total_values_compressed, dropped,
        st->compression_total_values_dropped_low_saving, st->compression_total_values_dropped_changed,
        st->compression_total_values_dropped_not_eligible, st->compression_total_sweeps_paused_by_child, st->compression_total_compression_time_us,
        st->compression_total_values_decompressed, st->compression_total_temporary_copies_made, st->compression_total_decompression_time_us);
}
