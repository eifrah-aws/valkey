/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the compression frame. See src/compression_value.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compression_value.h"
#include "server.h"
#include "zmalloc.h"
}

/* zmalloc.h defines helper macros (__str/__xstr) that collide with libstdc++
 * internals. Keep them local to C headers in this C++ translation unit. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

#define FRAME_TEST_VALUE_CAP 4096

/* Writes a JSON-like value of about len bytes into buf, which holds
 * FRAME_TEST_VALUE_CAP bytes. Returns the real length. */
static size_t frameTestValue(char *buf, size_t len, int seed) {
    size_t n = 0;
    int i = 0;
    while (n + 64 < len && n + 64 < FRAME_TEST_VALUE_CAP) {
        n += (size_t)snprintf(buf + n, FRAME_TEST_VALUE_CAP - n, "{\"user_id\":%d,\"role\":\"member\",\"active\":true},", seed + i);
        i++;
    }
    return n;
}

/* Changes one header byte of frame in place. */
static void frameTestPoke(sds frame, size_t offset, unsigned char v) {
    ((unsigned char *)frame)[offset] = v;
}

class CompressionValueTest : public ::testing::Test {
  protected:
    blockCompressor *c;
    char value[FRAME_TEST_VALUE_CAP];
    size_t value_len;

    void SetUp() override {
        c = blockCompressorCreate(ALGO_LZ4, NULL);
        ASSERT_TRUE(c != NULL);
        value_len = frameTestValue(value, 1024, 0);
    }

    void TearDown() override {
        blockCompressorFree(c);
    }
};

/* ===== build and decompress ===== */

TEST_F(CompressionValueTest, RoundTripNoDictionary) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    EXPECT_LT(sdslen(frame), value_len);

    /* The frame was shrunk: it uses no more memory than a new string of the
     * same length. sdsavail() is not 0 here, because sds counts the allocator
     * rounding as free space. */
    sds same_len = sdsnewlen(frame, sdslen(frame));
    EXPECT_LE(compressionValueAllocSize(frame), compressionValueAllocSize(same_len));
    sdsfree(same_len);

    compressionValueHeader hdr;
    ASSERT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_NONE);
    EXPECT_EQ(hdr.uncompressed_len, (uint32_t)value_len);
    EXPECT_EQ(hdr.dict_id, (uint32_t)0);
    EXPECT_EQ(hdr.algorithm_id, (uint16_t)ALGO_LZ4);
    EXPECT_EQ(hdr.format_version, (uint8_t)COMPRESSION_VALUE_FORMAT_V1);
    EXPECT_EQ(hdr.original_encoding, (uint8_t)OBJ_ENCODING_RAW);

    sds out = compressionValueDecompress(c, frame, NULL);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(sdslen(out), value_len);
    EXPECT_EQ(memcmp(out, value, value_len), 0);
    EXPECT_EQ(out[value_len], '\0');

    sdsfree(out);
    sdsfree(frame);
}

TEST_F(CompressionValueTest, RoundTripWithDictionary) {
    char dict[FRAME_TEST_VALUE_CAP];
    size_t dict_len = frameTestValue(dict, 2048, 1000);
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    blockCompressorDict d = {7, ALGO_LZ4, cdict};
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, &d);
    ASSERT_TRUE(frame != NULL);

    compressionValueHeader hdr;
    ASSERT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_NONE);
    EXPECT_EQ(hdr.dict_id, (uint32_t)7);

    sds out = compressionValueDecompress(c, frame, &d);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(sdslen(out), value_len);
    EXPECT_EQ(memcmp(out, value, value_len), 0);

    sdsfree(out);
    sdsfree(frame);
    c->api->dict_free(cdict);
}

TEST_F(CompressionValueTest, NoLeak) {
    size_t before = zmalloc_used_memory();
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sds out = compressionValueDecompress(c, frame, NULL);
    ASSERT_TRUE(out != NULL);
    sdsfree(out);
    sdsfree(frame);
    EXPECT_EQ(zmalloc_used_memory(), before);
}

TEST_F(CompressionValueTest, HeaderLayout) {
    sds frame = compressionValueBuild(c, 0x0A, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    const unsigned char *p = (const unsigned char *)frame;

    /* uncompressed_len and dict_id are little endian. */
    uint32_t len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    EXPECT_EQ(len, (uint32_t)value_len);
    EXPECT_EQ(p[4] | p[5] | p[6] | p[7], 0);
    EXPECT_EQ(p[8], (unsigned char)ALGO_LZ4);
    /* format_version in the high 4 bits, original_encoding in the low 4 bits. */
    EXPECT_EQ(p[9], (unsigned char)((COMPRESSION_VALUE_FORMAT_V1 << 4) | 0x0A));

    sdsfree(frame);
}

TEST_F(CompressionValueTest, InnerEncodingRoundTrip) {
    for (int enc = 0; enc <= COMPRESSION_VALUE_ORIGINAL_ENCODING_MAX; enc++) {
        sds frame = compressionValueBuild(c, (uint8_t)enc, value, value_len, NULL);
        ASSERT_TRUE(frame != NULL);
        compressionValueHeader hdr;
        ASSERT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_NONE);
        EXPECT_EQ(hdr.original_encoding, (uint8_t)enc);
        EXPECT_EQ(hdr.format_version, (uint8_t)COMPRESSION_VALUE_FORMAT_V1);
        sdsfree(frame);
    }
}

TEST_F(CompressionValueTest, BuildRejectsSizeOutOfRange) {
    blockCompressorConfig cfg = {256, 2048};
    blockCompressor *ranged = blockCompressorCreate(ALGO_LZ4, &cfg);
    ASSERT_TRUE(ranged != NULL);

    EXPECT_TRUE(compressionValueBuild(ranged, OBJ_ENCODING_RAW, value, 100, NULL) == NULL);
    EXPECT_EQ(ranged->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);
    EXPECT_TRUE(compressionValueBuild(ranged, OBJ_ENCODING_RAW, value, 4000, NULL) == NULL);
    EXPECT_EQ(ranged->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);

    blockCompressorFree(ranged);
}

TEST_F(CompressionValueTest, BuildRejectsAboveHardCap) {
    size_t big_len = COMPRESSION_VALUE_MAX_LEN + 1;
    char *big = (char *)zcalloc(big_len);
    EXPECT_TRUE(compressionValueBuild(c, OBJ_ENCODING_RAW, big, big_len, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_BAD_SIZE);

    /* The cap itself is allowed. */
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, big, big_len - 1, NULL);
    EXPECT_TRUE(frame != NULL);
    sdsfree(frame);
    zfree(big);
}

/* ===== parse ===== */

TEST_F(CompressionValueTest, ParseRejectsShortFrame) {
    compressionValueHeader hdr;
    sds empty = sdsempty();
    EXPECT_EQ(compressionValueParse(empty, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_TOO_SHORT);
    sdsfree(empty);

    /* A header with no body is not a frame. */
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sdssetlen(frame, COMPRESSION_VALUE_HEADER_LEN);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_TOO_SHORT);
    sdsfree(frame);
}

TEST_F(CompressionValueTest, ParseRejectsBadFormatVersion) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressionValueHeader hdr;

    frameTestPoke(frame, 9, (0 << 4) | OBJ_ENCODING_RAW);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_VERSION);
    frameTestPoke(frame, 9, ((COMPRESSION_VALUE_FORMAT_V1 + 1) << 4) | OBJ_ENCODING_RAW);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_VERSION);
    EXPECT_TRUE(compressionValueDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_FRAME_VERSION);

    sdsfree(frame);
}

TEST_F(CompressionValueTest, ParseRejectsUnknownAlgorithm) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressionValueHeader hdr;

    frameTestPoke(frame, 8, ALGO_NONE);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_ALGORITHM);
    frameTestPoke(frame, 8, 99);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_ALGORITHM);

    sdsfree(frame);
}

TEST_F(CompressionValueTest, ParseRejectsBadUncompressedLen) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressionValueHeader hdr;

    /* 0 */
    for (int i = 0; i < 4; i++) frameTestPoke(frame, i, 0);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_LENGTH);

    /* COMPRESSION_VALUE_MAX_LEN + 1 = 0x00020001 */
    frameTestPoke(frame, 0, 0x01);
    frameTestPoke(frame, 2, 0x02);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_FRAME_LENGTH);

    /* COMPRESSION_VALUE_MAX_LEN itself is allowed. */
    frameTestPoke(frame, 0, 0x00);
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_NONE);

    sdsfree(frame);
}

/* ===== decompress mismatches ===== */

TEST_F(CompressionValueTest, DecompressRejectsAlgorithmMismatch) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    /* A valid zstd frame must not reach the LZ4 decoder. */
    frameTestPoke(frame, 8, ALGO_ZSTD);
    compressionValueHeader hdr;
    EXPECT_EQ(compressionValueParse(frame, &hdr), BLOCK_COMPRESSOR_ERR_NONE);
    EXPECT_TRUE(compressionValueDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_ALGORITHM_MISMATCH);
    sdsfree(frame);
}

TEST_F(CompressionValueTest, DecompressRejectsDictionaryMismatch) {
    char dict[FRAME_TEST_VALUE_CAP];
    size_t dict_len = frameTestValue(dict, 2048, 1000);
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    sds plain = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    blockCompressorDict d = {3, ALGO_LZ4, cdict};
    sds with_dict = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, &d);
    ASSERT_TRUE(plain != NULL && with_dict != NULL);

    /* A dictionary for a frame built without one. */
    EXPECT_TRUE(compressionValueDecompress(c, plain, &d) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* No dictionary for a frame built with one. */
    EXPECT_TRUE(compressionValueDecompress(c, with_dict, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* Another dictionary id. */
    blockCompressorDict other_id = {4, ALGO_LZ4, cdict};
    EXPECT_TRUE(compressionValueDecompress(c, with_dict, &other_id) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* The right id, but built for another algorithm. */
    blockCompressorDict other_alg = {3, ALGO_ZSTD, cdict};
    EXPECT_TRUE(compressionValueDecompress(c, with_dict, &other_alg) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH);

    /* The right one still works. */
    sds out = compressionValueDecompress(c, with_dict, &d);
    ASSERT_TRUE(out != NULL);
    EXPECT_EQ(sdslen(out), value_len);
    sdsfree(out);

    sdsfree(plain);
    sdsfree(with_dict);
    c->api->dict_free(cdict);
}

TEST_F(CompressionValueTest, DecompressRejectsTruncatedBody) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sdssetlen(frame, sdslen(frame) - 1);
    EXPECT_TRUE(compressionValueDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DECOMPRESS);
    sdsfree(frame);
}

TEST_F(CompressionValueTest, DecompressRejectsWrongUncompressedLen) {
    sds frame = compressionValueBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    unsigned char orig = (unsigned char)frame[0];

    /* Too small: the body does not fit. */
    frameTestPoke(frame, 0, orig - 1);
    EXPECT_TRUE(compressionValueDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DECOMPRESS);

    /* Too large: the body is shorter than the header says. */
    frameTestPoke(frame, 0, orig + 1);
    EXPECT_TRUE(compressionValueDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, BLOCK_COMPRESSOR_ERR_DECOMPRESS);

    sdsfree(frame);
}

/* ===== net-savings guard ===== */

/* The jemalloc cases from appendix A of the design. */
TEST(CompressionValue, SavesMemoryUsesAllocationSizes) {
    /* 517 bytes in the 640 bin, compressed to 435 bytes in the 448 bin. */
    EXPECT_EQ(compressionValueSavesMemory(640, 448, 10), 1);
    /* 300 bytes in the 320 bin, compressed 16%, still in the 320 bin. */
    EXPECT_EQ(compressionValueSavesMemory(320, 320, 10), 0);
    EXPECT_EQ(compressionValueSavesMemory(320, 320, 0), 1);
}

TEST(CompressionValue, SavesMemoryEdges) {
    EXPECT_EQ(compressionValueSavesMemory(100, 90, 10), 1);
    EXPECT_EQ(compressionValueSavesMemory(100, 91, 10), 0);
    EXPECT_EQ(compressionValueSavesMemory(100, 120, 0), 0);
    EXPECT_EQ(compressionValueSavesMemory(100, 0, 100), 0);
    EXPECT_EQ(compressionValueSavesMemory(100, 0, 200), 0);
}

TEST(CompressionValue, ErrorTextIsSpecific) {
    int codes[] = {BLOCK_COMPRESSOR_ERR_FRAME_TOO_SHORT,
                   BLOCK_COMPRESSOR_ERR_FRAME_VERSION,
                   BLOCK_COMPRESSOR_ERR_FRAME_ALGORITHM,
                   BLOCK_COMPRESSOR_ERR_FRAME_LENGTH,
                   BLOCK_COMPRESSOR_ERR_ALGORITHM_MISMATCH,
                   BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH};
    int n = (int)(sizeof(codes) / sizeof(codes[0]));
    for (int i = 0; i < n; i++) {
        EXPECT_STRNE(blockCompressorStrerror(codes[i]), blockCompressorStrerror(-1));
        for (int j = i + 1; j < n; j++) EXPECT_STRNE(blockCompressorStrerror(codes[i]), blockCompressorStrerror(codes[j]));
    }
}

TEST(CompressionValue, AllocSizeCoversString) {
    sds s = sdsnewlen(NULL, 300);
    EXPECT_GE(compressionValueAllocSize(s), sdslen(s) + 1);
    sdsfree(s);
}
