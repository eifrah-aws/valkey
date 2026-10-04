/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the compression frame. See src/compressor/compressor_frame.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compressor/compressor_frame.h"
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

class CompressorFrameTest : public ::testing::Test {
  protected:
    compressorAlg *c;
    char value[FRAME_TEST_VALUE_CAP];
    size_t value_len;

    void SetUp() override {
        c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
        ASSERT_TRUE(c != NULL);
        value_len = frameTestValue(value, 1024, 0);
    }

    void TearDown() override {
        freeCompressor(c);
    }
};

/* ===== build and decompress ===== */

TEST_F(CompressorFrameTest, RoundTripNoDictionary) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    EXPECT_LT(sdslen(frame), value_len);

    /* The frame was shrunk: it uses no more memory than a new string of the
     * same length. sdsavail() is not 0 here, because sds counts the allocator
     * rounding as free space. */
    sds same_len = sdsnewlen(frame, sdslen(frame));
    EXPECT_LE(compressorFrameAllocSize(frame), compressorFrameAllocSize(same_len));
    sdsfree(same_len);

    compressorFrameHeader hdr;
    ASSERT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_NONE);
    EXPECT_EQ(hdr.uncompressed_len, (uint32_t)value_len);
    EXPECT_EQ(hdr.dict_id, (uint32_t)0);
    EXPECT_EQ(hdr.algorithm_id, (uint16_t)COMPRESSOR_ALG_LZ4);
    EXPECT_EQ(hdr.format_version, (uint8_t)COMPRESSOR_FRAME_FORMAT_V1);
    EXPECT_EQ(hdr.original_encoding, (uint8_t)OBJ_ENCODING_RAW);

    sds out = compressorFrameDecompress(c, frame, NULL);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(sdslen(out), value_len);
    EXPECT_EQ(memcmp(out, value, value_len), 0);
    EXPECT_EQ(out[value_len], '\0');

    sdsfree(out);
    sdsfree(frame);
}

TEST_F(CompressorFrameTest, RoundTripWithDictionary) {
    char dict[FRAME_TEST_VALUE_CAP];
    size_t dict_len = frameTestValue(dict, 2048, 1000);
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    compressorDict d = {7, COMPRESSOR_ALG_LZ4, cdict};
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, &d);
    ASSERT_TRUE(frame != NULL);

    compressorFrameHeader hdr;
    ASSERT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_NONE);
    EXPECT_EQ(hdr.dict_id, (uint32_t)7);

    sds out = compressorFrameDecompress(c, frame, &d);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(sdslen(out), value_len);
    EXPECT_EQ(memcmp(out, value, value_len), 0);

    sdsfree(out);
    sdsfree(frame);
    c->api->dict_free(cdict);
}

TEST_F(CompressorFrameTest, NoLeak) {
    size_t before = zmalloc_used_memory();
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sds out = compressorFrameDecompress(c, frame, NULL);
    ASSERT_TRUE(out != NULL);
    sdsfree(out);
    sdsfree(frame);
    EXPECT_EQ(zmalloc_used_memory(), before);
}

TEST_F(CompressorFrameTest, HeaderLayout) {
    sds frame = compressorFrameBuild(c, 0x0A, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    const unsigned char *p = (const unsigned char *)frame;

    /* uncompressed_len and dict_id are little endian. */
    uint32_t len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    EXPECT_EQ(len, (uint32_t)value_len);
    EXPECT_EQ(p[4] | p[5] | p[6] | p[7], 0);
    EXPECT_EQ(p[8], (unsigned char)COMPRESSOR_ALG_LZ4);
    /* format_version in the high 4 bits, original_encoding in the low 4 bits. */
    EXPECT_EQ(p[9], (unsigned char)((COMPRESSOR_FRAME_FORMAT_V1 << 4) | 0x0A));

    sdsfree(frame);
}

TEST_F(CompressorFrameTest, InnerEncodingRoundTrip) {
    for (int enc = 0; enc <= COMPRESSOR_FRAME_ORIGINAL_ENCODING_MAX; enc++) {
        sds frame = compressorFrameBuild(c, (uint8_t)enc, value, value_len, NULL);
        ASSERT_TRUE(frame != NULL);
        compressorFrameHeader hdr;
        ASSERT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_NONE);
        EXPECT_EQ(hdr.original_encoding, (uint8_t)enc);
        EXPECT_EQ(hdr.format_version, (uint8_t)COMPRESSOR_FRAME_FORMAT_V1);
        sdsfree(frame);
    }
}

TEST_F(CompressorFrameTest, BuildRejectsSizeOutOfRange) {
    compressorConfig cfg = {256, 2048};
    compressorAlg *ranged = newCompressor(COMPRESSOR_ALG_LZ4, &cfg);
    ASSERT_TRUE(ranged != NULL);

    EXPECT_TRUE(compressorFrameBuild(ranged, OBJ_ENCODING_RAW, value, 100, NULL) == NULL);
    EXPECT_EQ(ranged->last_error, COMPRESSOR_ERR_BAD_SIZE);
    EXPECT_TRUE(compressorFrameBuild(ranged, OBJ_ENCODING_RAW, value, 4000, NULL) == NULL);
    EXPECT_EQ(ranged->last_error, COMPRESSOR_ERR_BAD_SIZE);

    freeCompressor(ranged);
}

TEST_F(CompressorFrameTest, BuildRejectsAboveHardCap) {
    size_t big_len = COMPRESSOR_FRAME_MAX_VALUE_LEN + 1;
    char *big = (char *)zcalloc(big_len);
    EXPECT_TRUE(compressorFrameBuild(c, OBJ_ENCODING_RAW, big, big_len, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_BAD_SIZE);

    /* The cap itself is allowed. */
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, big, big_len - 1, NULL);
    EXPECT_TRUE(frame != NULL);
    sdsfree(frame);
    zfree(big);
}

/* ===== parse ===== */

TEST_F(CompressorFrameTest, ParseRejectsShortFrame) {
    compressorFrameHeader hdr;
    sds empty = sdsempty();
    EXPECT_EQ(compressorFrameParse(empty, &hdr), COMPRESSOR_ERR_FRAME_TOO_SHORT);
    sdsfree(empty);

    /* A header with no body is not a frame. */
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sdssetlen(frame, COMPRESSOR_FRAME_HEADER_LEN);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_TOO_SHORT);
    sdsfree(frame);
}

TEST_F(CompressorFrameTest, ParseRejectsBadFormatVersion) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressorFrameHeader hdr;

    frameTestPoke(frame, 9, (0 << 4) | OBJ_ENCODING_RAW);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_VERSION);
    frameTestPoke(frame, 9, ((COMPRESSOR_FRAME_FORMAT_V1 + 1) << 4) | OBJ_ENCODING_RAW);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_VERSION);
    EXPECT_TRUE(compressorFrameDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_FRAME_VERSION);

    sdsfree(frame);
}

TEST_F(CompressorFrameTest, ParseRejectsUnknownAlgorithm) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressorFrameHeader hdr;

    frameTestPoke(frame, 8, COMPRESSOR_ALG_NONE);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_ALGORITHM);
    frameTestPoke(frame, 8, 99);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_ALGORITHM);

    sdsfree(frame);
}

TEST_F(CompressorFrameTest, ParseRejectsBadUncompressedLen) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    compressorFrameHeader hdr;

    /* 0 */
    for (int i = 0; i < 4; i++) frameTestPoke(frame, i, 0);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_LENGTH);

    /* COMPRESSOR_FRAME_MAX_VALUE_LEN + 1 = 0x00020001 */
    frameTestPoke(frame, 0, 0x01);
    frameTestPoke(frame, 2, 0x02);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_FRAME_LENGTH);

    /* COMPRESSOR_FRAME_MAX_VALUE_LEN itself is allowed. */
    frameTestPoke(frame, 0, 0x00);
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_NONE);

    sdsfree(frame);
}

/* ===== decompress mismatches ===== */

TEST_F(CompressorFrameTest, DecompressRejectsAlgorithmMismatch) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    /* A valid zstd frame must not reach the LZ4 decoder. */
    frameTestPoke(frame, 8, COMPRESSOR_ALG_ZSTD);
    compressorFrameHeader hdr;
    EXPECT_EQ(compressorFrameParse(frame, &hdr), COMPRESSOR_ERR_NONE);
    EXPECT_TRUE(compressorFrameDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_ALGORITHM_MISMATCH);
    sdsfree(frame);
}

TEST_F(CompressorFrameTest, DecompressRejectsDictionaryMismatch) {
    char dict[FRAME_TEST_VALUE_CAP];
    size_t dict_len = frameTestValue(dict, 2048, 1000);
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    sds plain = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    compressorDict d = {3, COMPRESSOR_ALG_LZ4, cdict};
    sds with_dict = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, &d);
    ASSERT_TRUE(plain != NULL && with_dict != NULL);

    /* A dictionary for a frame built without one. */
    EXPECT_TRUE(compressorFrameDecompress(c, plain, &d) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* No dictionary for a frame built with one. */
    EXPECT_TRUE(compressorFrameDecompress(c, with_dict, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* Another dictionary id. */
    compressorDict other_id = {4, COMPRESSOR_ALG_LZ4, cdict};
    EXPECT_TRUE(compressorFrameDecompress(c, with_dict, &other_id) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DICTIONARY_MISMATCH);
    /* The right id, but built for another algorithm. */
    compressorDict other_alg = {3, COMPRESSOR_ALG_ZSTD, cdict};
    EXPECT_TRUE(compressorFrameDecompress(c, with_dict, &other_alg) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DICTIONARY_MISMATCH);

    /* The right one still works. */
    sds out = compressorFrameDecompress(c, with_dict, &d);
    ASSERT_TRUE(out != NULL);
    EXPECT_EQ(sdslen(out), value_len);
    sdsfree(out);

    sdsfree(plain);
    sdsfree(with_dict);
    c->api->dict_free(cdict);
}

TEST_F(CompressorFrameTest, DecompressRejectsTruncatedBody) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    sdssetlen(frame, sdslen(frame) - 1);
    EXPECT_TRUE(compressorFrameDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DECOMPRESS);
    sdsfree(frame);
}

TEST_F(CompressorFrameTest, DecompressRejectsWrongUncompressedLen) {
    sds frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, value, value_len, NULL);
    ASSERT_TRUE(frame != NULL);
    unsigned char orig = (unsigned char)frame[0];

    /* Too small: the body does not fit. */
    frameTestPoke(frame, 0, orig - 1);
    EXPECT_TRUE(compressorFrameDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DECOMPRESS);

    /* Too large: the body is shorter than the header says. */
    frameTestPoke(frame, 0, orig + 1);
    EXPECT_TRUE(compressorFrameDecompress(c, frame, NULL) == NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DECOMPRESS);

    sdsfree(frame);
}

/* ===== net-savings guard ===== */

/* The jemalloc cases from appendix A of the design. */
TEST(CompressorFrame, SavesMemoryUsesAllocationSizes) {
    /* 517 bytes in the 640 bin, compressed to 435 bytes in the 448 bin. */
    EXPECT_EQ(compressorFrameSavesMemory(640, 448, 10), 1);
    /* 300 bytes in the 320 bin, compressed 16%, still in the 320 bin. */
    EXPECT_EQ(compressorFrameSavesMemory(320, 320, 10), 0);
    EXPECT_EQ(compressorFrameSavesMemory(320, 320, 0), 1);
}

TEST(CompressorFrame, SavesMemoryEdges) {
    EXPECT_EQ(compressorFrameSavesMemory(100, 90, 10), 1);
    EXPECT_EQ(compressorFrameSavesMemory(100, 91, 10), 0);
    EXPECT_EQ(compressorFrameSavesMemory(100, 120, 0), 0);
    EXPECT_EQ(compressorFrameSavesMemory(100, 0, 100), 0);
    EXPECT_EQ(compressorFrameSavesMemory(100, 0, 200), 0);
}

TEST(CompressorFrame, ErrorTextIsSpecific) {
    int codes[] = {COMPRESSOR_ERR_FRAME_TOO_SHORT,
                   COMPRESSOR_ERR_FRAME_VERSION,
                   COMPRESSOR_ERR_FRAME_ALGORITHM,
                   COMPRESSOR_ERR_FRAME_LENGTH,
                   COMPRESSOR_ERR_ALGORITHM_MISMATCH,
                   COMPRESSOR_ERR_DICTIONARY_MISMATCH};
    int n = (int)(sizeof(codes) / sizeof(codes[0]));
    for (int i = 0; i < n; i++) {
        EXPECT_STRNE(compressorStrerror(codes[i]), compressorStrerror(-1));
        for (int j = i + 1; j < n; j++) EXPECT_STRNE(compressorStrerror(codes[i]), compressorStrerror(codes[j]));
    }
}

TEST(CompressorFrame, AllocSizeCoversString) {
    sds s = sdsnewlen(NULL, 300);
    EXPECT_GE(compressorFrameAllocSize(s), sdslen(s) + 1);
    sdsfree(s);
}
