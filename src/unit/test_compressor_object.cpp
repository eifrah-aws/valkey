/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for compressed string objects. See src/compressor/compressor_object.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compressor/compressor_frame.h"
#include "compressor/compressor_object.h"
#include "monotonic.h"
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

#define OBJECT_TEST_VALUE_CAP 2048

class CompressorObjectTest : public ::testing::Test {
  protected:
    char value[OBJECT_TEST_VALUE_CAP];
    size_t value_len;

    void SetUp() override {
        /* Decompression measures its time for INFO compression. */
        monotonicInit();
        value_len = makeValue(value, 0);
    }

    /* A JSON-like value of about 1 KiB that compresses well. */
    size_t makeValue(char *buf, int seed) {
        size_t n = 0;
        for (int i = 0; i < 25; i++) {
            n += (size_t)snprintf(buf + n, OBJECT_TEST_VALUE_CAP - n, "{\"user_id\":%d,\"role\":\"member\"},", seed + i);
        }
        return n;
    }

    /* A RAW string object with the value, then compressed in place. */
    robj *createCompressed(const char *buf, size_t len) {
        robj *o = createRawStringObject(buf, len);
        sds frame = compressorFrameBuild(compressorGetForMainThread(COMPRESSOR_ALG_LZ4), OBJ_ENCODING_RAW, buf, len, NULL);
        EXPECT_TRUE(frame != NULL);
        compressorSetCompressedValue(o, frame);
        EXPECT_EQ(objectGetEncoding(o), OBJ_ENCODING_COMPRESSED);
        return o;
    }
};

TEST_F(CompressorObjectTest, SetCompressedThenDecompressInPlace) {
    robj *o = createCompressed(value, value_len);
    EXPECT_LT(sdslen((sds)objectGetVal(o)), value_len);

    compressorDecompressStringObject(o);
    EXPECT_EQ(objectGetEncoding(o), OBJ_ENCODING_RAW);
    ASSERT_EQ(sdslen((sds)objectGetVal(o)), value_len);
    EXPECT_EQ(memcmp(objectGetVal(o), value, value_len), 0);
    decrRefCount(o);
}

TEST_F(CompressorObjectTest, StringObjectLen) {
    robj *o = createCompressed(value, value_len);
    EXPECT_EQ(stringObjectLen(o), value_len);
    decrRefCount(o);
}

TEST_F(CompressorObjectTest, CompareAndEqual) {
    robj *compressed = createCompressed(value, value_len);
    robj *same = createRawStringObject(value, value_len);

    /* Same length, other bytes. */
    char other_buf[OBJECT_TEST_VALUE_CAP];
    memcpy(other_buf, value, value_len);
    other_buf[value_len - 2] = '#';
    robj *other = createRawStringObject(other_buf, value_len);

    EXPECT_EQ(equalStringObjects(compressed, same), 1);
    EXPECT_EQ(equalStringObjects(same, compressed), 1);
    EXPECT_EQ(compareStringObjects(compressed, same), 0);
    EXPECT_EQ(equalStringObjects(compressed, other), 0);
    EXPECT_NE(compareStringObjects(compressed, other), 0);

    robj *compressed2 = createCompressed(value, value_len);
    EXPECT_EQ(equalStringObjects(compressed, compressed2), 1);

    /* The objects stay compressed. */
    EXPECT_EQ(objectGetEncoding(compressed), OBJ_ENCODING_COMPRESSED);
    EXPECT_EQ(objectGetEncoding(compressed2), OBJ_ENCODING_COMPRESSED);

    decrRefCount(compressed);
    decrRefCount(compressed2);
    decrRefCount(same);
    decrRefCount(other);
}

TEST_F(CompressorObjectTest, GetDecodedObjectGivesPlainCopy) {
    robj *o = createCompressed(value, value_len);
    robj *dec = getDecodedObject(o);
    ASSERT_TRUE(dec != o);
    EXPECT_EQ(objectGetEncoding(dec), OBJ_ENCODING_RAW);
    ASSERT_EQ(sdslen((sds)objectGetVal(dec)), value_len);
    EXPECT_EQ(memcmp(objectGetVal(dec), value, value_len), 0);
    EXPECT_EQ(objectGetEncoding(o), OBJ_ENCODING_COMPRESSED);
    decrRefCount(dec);
    decrRefCount(o);
}
