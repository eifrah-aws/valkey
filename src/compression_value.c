/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Building, checking, and decoding compression frames. See compression_value.h
 * for the layout. */

#include "fmacros.h" /* must come before any system header */
#include "compression_value.h"
#include "endianconv.h"
#include "server.h" /* serverAssert */
#include "zmalloc.h"
#include <string.h>

/* The offsets must cover the header exactly, with no gap. */
static_assert(COMPRESSION_VALUE_OFF_DICT_ID == COMPRESSION_VALUE_OFF_UNCOMPRESSED_LEN + 4, "bad dict_id offset");
static_assert(COMPRESSION_VALUE_OFF_ALGORITHM_ID == COMPRESSION_VALUE_OFF_DICT_ID + 4, "bad algorithm_id offset");
static_assert(COMPRESSION_VALUE_OFF_VERSION_ENCODING == COMPRESSION_VALUE_OFF_ALGORITHM_ID + 1, "bad version offset");
static_assert(COMPRESSION_VALUE_HEADER_LEN == COMPRESSION_VALUE_OFF_VERSION_ENCODING + 1, "bad header length");
static_assert(COMPRESSION_VALUE_FORMAT_V1 <= COMPRESSION_VALUE_FORMAT_VERSION_MAX, "format version must fit in 4 bits");

/* Writes hdr into the first COMPRESSION_VALUE_HEADER_LEN bytes of p. */
static void frameHeaderEncode(unsigned char *p, const compressionValueHeader *hdr) {
    uint32_t len = intrev32ifbe(hdr->uncompressed_len);
    uint32_t dict_id = intrev32ifbe(hdr->dict_id);
    memcpy(p + COMPRESSION_VALUE_OFF_UNCOMPRESSED_LEN, &len, sizeof(len));
    memcpy(p + COMPRESSION_VALUE_OFF_DICT_ID, &dict_id, sizeof(dict_id));
    p[COMPRESSION_VALUE_OFF_ALGORITHM_ID] = hdr->algorithm_id;
    p[COMPRESSION_VALUE_OFF_VERSION_ENCODING] = (unsigned char)((hdr->format_version << 4) | hdr->original_encoding);
}

/* Reads the first COMPRESSION_VALUE_HEADER_LEN bytes of p into hdr. */
static void frameHeaderDecode(const unsigned char *p, compressionValueHeader *hdr) {
    uint32_t len, dict_id;
    memcpy(&len, p + COMPRESSION_VALUE_OFF_UNCOMPRESSED_LEN, sizeof(len));
    memcpy(&dict_id, p + COMPRESSION_VALUE_OFF_DICT_ID, sizeof(dict_id));
    hdr->uncompressed_len = intrev32ifbe(len);
    hdr->dict_id = intrev32ifbe(dict_id);
    hdr->algorithm_id = p[COMPRESSION_VALUE_OFF_ALGORITHM_ID];
    hdr->format_version = p[COMPRESSION_VALUE_OFF_VERSION_ENCODING] >> 4;
    hdr->original_encoding = p[COMPRESSION_VALUE_OFF_VERSION_ENCODING] & COMPRESSION_VALUE_ORIGINAL_ENCODING_MAX;
}

static int frameAlgorithmKnown(uint8_t algorithm_id) {
    return algorithm_id == ALGO_LZ4 || algorithm_id == ALGO_ZSTD;
}

/* Returns the digested dictionary to pass to the backend, or NULL. */
static void *frameDictData(const blockCompressorDict *dict) {
    return dict != NULL ? dict->cdict : NULL;
}

sds compressionValueBuild(blockCompressor *c,
                          uint8_t original_encoding,
                          const void *src,
                          size_t srclen,
                          const blockCompressorDict *dict) {
    serverAssert(c != NULL && src != NULL);
    /* The caller picks the dictionary from the registry, so a bad one here is
     * a bug, not bad input. */
    if (dict != NULL) {
        serverAssert(blockCompressorDictIsValid(dict));
        serverAssert(dict->algorithm_id == (uint8_t)c->id);
    }
    serverAssert(original_encoding <= COMPRESSION_VALUE_ORIGINAL_ENCODING_MAX);

    c->last_error = BLOCK_COMPRESSOR_ERR_NONE;
    size_t bound = srclen <= COMPRESSION_VALUE_MAX_LEN ? c->max_output_size(c, srclen) : 0;
    if (bound == 0) {
        c->last_error = BLOCK_COMPRESSOR_ERR_BAD_SIZE;
        return NULL;
    }

    /* One allocation: header plus worst-case body. The backend writes straight
     * after the header, so there is no intermediate buffer. */
    sds frame = sdsnewlen(SDS_NOINIT, COMPRESSION_VALUE_HEADER_LEN + bound);
    size_t written = c->api->compress(c, frameDictData(dict), src, srclen, frame + COMPRESSION_VALUE_HEADER_LEN, bound);
    if (written == 0) {
        sdsfree(frame);
        return NULL;
    }

    compressionValueHeader hdr = {
        .uncompressed_len = (uint32_t)srclen,
        .dict_id = dict != NULL ? dict->id : 0,
        .algorithm_id = (uint8_t)c->id,
        .format_version = COMPRESSION_VALUE_FORMAT_V1,
        .original_encoding = original_encoding,
    };
    frameHeaderEncode((unsigned char *)frame, &hdr);

    /* The bound is generous. Without the shrink the allocator still charges
     * for the unused space, and reported savings would exceed real savings. */
    sdssetlen(frame, COMPRESSION_VALUE_HEADER_LEN + written);
    frame[COMPRESSION_VALUE_HEADER_LEN + written] = '\0';
    return sdsRemoveFreeSpace(frame, 0);
}

int compressionValueParse(const_sds frame, compressionValueHeader *hdr) {
    serverAssert(frame != NULL && hdr != NULL);
    /* A valid frame has at least one body byte. */
    if (sdslen(frame) <= COMPRESSION_VALUE_HEADER_LEN) return BLOCK_COMPRESSOR_ERR_FRAME_TOO_SHORT;

    frameHeaderDecode((const unsigned char *)frame, hdr);

    if (hdr->format_version != COMPRESSION_VALUE_FORMAT_V1) return BLOCK_COMPRESSOR_ERR_FRAME_VERSION;
    if (!frameAlgorithmKnown(hdr->algorithm_id)) return BLOCK_COMPRESSOR_ERR_FRAME_ALGORITHM;
    if (hdr->uncompressed_len == 0 || hdr->uncompressed_len > COMPRESSION_VALUE_MAX_LEN) {
        return BLOCK_COMPRESSOR_ERR_FRAME_LENGTH;
    }
    return BLOCK_COMPRESSOR_ERR_NONE;
}

sds compressionValueDecompress(blockCompressor *c, const_sds frame, const blockCompressorDict *dict) {
    serverAssert(c != NULL);

    compressionValueHeader hdr;
    c->last_error = compressionValueParse(frame, &hdr);
    if (c->last_error != BLOCK_COMPRESSOR_ERR_NONE) return NULL;
    if (hdr.algorithm_id != (uint8_t)c->id) {
        c->last_error = BLOCK_COMPRESSOR_ERR_ALGORITHM_MISMATCH;
        return NULL;
    }
    /* The dictionary comes from a lookup by dict_id, and a frame from RDB is
     * outside input, so a mismatch is an error, not an assert. */
    uint32_t dict_id = dict != NULL ? dict->id : 0;
    if (dict_id != hdr.dict_id || (dict != NULL && dict->algorithm_id != hdr.algorithm_id)) {
        c->last_error = BLOCK_COMPRESSOR_ERR_DICTIONARY_MISMATCH;
        return NULL;
    }

    sds out = sdsnewlen(SDS_NOINIT, hdr.uncompressed_len);
    size_t written = c->api->decompress(c,
                                        frameDictData(dict),
                                        frame + COMPRESSION_VALUE_HEADER_LEN,
                                        sdslen(frame) - COMPRESSION_VALUE_HEADER_LEN,
                                        out,
                                        hdr.uncompressed_len);
    if (written != hdr.uncompressed_len) {
        /* written is 0 when the backend failed, and the backend set
         * last_error. Otherwise the body decoded to the wrong length. */
        if (written != 0) c->last_error = BLOCK_COMPRESSOR_ERR_DECOMPRESS;
        sdsfree(out);
        return NULL;
    }
    return out;
}

int compressionValueSavesMemory(size_t orig_alloc, size_t frame_alloc, unsigned int min_savings_pct) {
    if (min_savings_pct >= 100) return 0;
    /* frame_alloc <= orig_alloc * (100 - pct) / 100, without the division. */
    return (unsigned long long)frame_alloc * 100 <= (unsigned long long)orig_alloc * (100 - min_savings_pct);
}

size_t compressionValueAllocSize(const_sds s) {
    return zmalloc_size(sdsAllocPtr(s));
}
