/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the INFO compression numbers. See src/compressor/compressor_stats.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compressor/compressor_stats.h"
#include "server.h"
}

/* zmalloc.h defines helper macros (__str/__xstr) that collide with libstdc++
 * internals. Keep them local to C headers in this C++ translation unit. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

/* Reads one field from the INFO compression text into buf. Returns 1 when the
 * field is there. */
static int statsInfoField(const char *field, char *buf, size_t buflen) {
    sds info = compressorStatsInfo(sdsempty());
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\r\n%s:", field);
    const char *p = strstr(info, pattern);
    int found = 0;
    if (p != NULL) {
        p += strlen(pattern);
        size_t n = strcspn(p, "\r");
        if (n >= buflen) n = buflen - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';
        found = 1;
    }
    sdsfree(info);
    return found;
}

/* In C++ unit tests _Atomic() is a plain type (see wrappers.h), so the test
 * reads the gauges directly. */
static unsigned long long gaugeValue(const unsigned long long *g) {
    return *g;
}

class CompressorStatsTest : public ::testing::Test {
  protected:
    compressorStats saved;

    void SetUp() override {
        memcpy(&saved, &compressor_stats, sizeof(saved));
        memset(&compressor_stats, 0, sizeof(compressor_stats));
    }

    void TearDown() override {
        memcpy(&compressor_stats, &saved, sizeof(saved));
    }
};

TEST_F(CompressorStatsTest, AddAndRemoveUpdateTheGauges) {
    compressorStatsAddValue(1000, 300);
    compressorStatsAddValue(500, 200);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values), 2ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_original_bytes), 1500ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_bytes), 500ULL);

    compressorStatsRemoveValue(1000, 300);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values), 1ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_original_bytes), 500ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_bytes), 200ULL);

    compressorStatsRemoveValue(500, 200);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values), 0ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_original_bytes), 0ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_bytes), 0ULL);
}

TEST_F(CompressorStatsTest, ResetClearsCountersAndKeepsGauges) {
    compressorStatsAddValue(1000, 300);
    compressor_stats.counters.compression_total_keys_checked = 7;
    compressor_stats.counters.compression_total_values_compressed = 3;
    compressor_stats.counters.compression_total_decompression_time_us = 42;

    compressorStatsReset();

    EXPECT_EQ(compressor_stats.counters.compression_total_keys_checked, 0ULL);
    EXPECT_EQ(compressor_stats.counters.compression_total_values_compressed, 0ULL);
    EXPECT_EQ(compressor_stats.counters.compression_total_decompression_time_us, 0ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values), 1ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_original_bytes), 1000ULL);
    EXPECT_EQ(gaugeValue(&compressor_stats.gauges.compressed_values_bytes), 300ULL);
}

TEST_F(CompressorStatsTest, InfoShowsTheValues) {
    compressorStatsAddValue(1000, 300);
    compressor_stats.counters.compression_total_values_dropped_low_saving = 1;
    compressor_stats.counters.compression_total_values_dropped_changed = 2;
    compressor_stats.counters.compression_total_values_dropped_not_eligible = 3;

    char buf[64];
    ASSERT_TRUE(statsInfoField("compressed_values", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "1");
    ASSERT_TRUE(statsInfoField("compression_saved_bytes", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "700");
    /* The total of the three reasons. */
    ASSERT_TRUE(statsInfoField("compression_total_values_dropped", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "6");
    ASSERT_TRUE(statsInfoField("compression_total_decompression_time_us", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "0");
}

TEST_F(CompressorStatsTest, SavedBytesIsNeverNegative) {
    /* The gauges are read one by one, so for a moment the frames can look
     * bigger than the original. The INFO text must show 0, not a huge number. */
    compressorStatsAddValue(100, 300);
    char buf[64];
    ASSERT_TRUE(statsInfoField("compression_saved_bytes", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "0");
}
