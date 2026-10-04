/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSOR_FRAME_H
#define COMPRESSOR_FRAME_H

/* The compression frame: what a compressed value looks like in memory and in
 * an RDB file. A frame is one sds string. It holds a 10-byte header followed
 * by the compressed bytes (the body):
 *
 *   offset  size    field
 *   0       4       uncompressed_len  bytes to allocate when decompressing
 *   4       4       dict_id           dictionary version, 0 means no dictionary
 *   8       1       algorithm_id      compressorAlgId needed to decode the body
 *   9       4 bits  format_version    high 4 bits, COMPRESSOR_FRAME_FORMAT_V1
 *   9       4 bits  original_encoding low 4 bits, object encoding before
 *                                     compression
 *
 * The header is small on purpose, because every compressed value pays for it.
 * Multi-byte fields are little endian, because frames are saved to RDB as they
 * are. dict_id is 32 bits so that dictionary ids can count up for the life of
 * the data and are never reused.
 *
 * The header makes a frame decodable on its own. It has no compressed length,
 * because sdslen(frame) - COMPRESSOR_FRAME_HEADER_LEN is exact. It has no
 * dictionary bytes, because dictionaries are shared and live in a registry.
 *
 * original_encoding is the encoding to rebuild after decompression, for example
 * OBJ_ENCODING_RAW for a string. The frame layer stores it but does not check
 * it, because only the caller knows which encodings are valid for its type.
 *
 * original_encoding has 4 bits, the same as the encoding field of an robj.
 *
 * An sds buffer sits after a 3- or 5-byte sds header, so it is not aligned for
 * these fields. The code never casts the buffer to a struct. It reads and
 * writes each field at its named offset with memcpy. */

#include "compressor/compressor_alg.h"
#include "sds.h"
#include <stddef.h>
#include <stdint.h>

#define COMPRESSOR_FRAME_HEADER_LEN 10

/* Field offsets in the header. */
#define COMPRESSOR_FRAME_OFF_UNCOMPRESSED_LEN 0
#define COMPRESSOR_FRAME_OFF_DICT_ID 4
#define COMPRESSOR_FRAME_OFF_ALGORITHM_ID 8
#define COMPRESSOR_FRAME_OFF_VERSION_ENCODING 9

/* format_version and original_encoding have 4 bits each, so 15 is the
 * largest value of each. ORIGINAL_ENCODING_MAX is also the mask that reads
 * original_encoding from its byte. */
#define COMPRESSOR_FRAME_FORMAT_VERSION_MAX 15
#define COMPRESSOR_FRAME_ORIGINAL_ENCODING_MAX 15

/* The only frame format so far. 0 is never valid, so a zeroed header fails. */
#define COMPRESSOR_FRAME_FORMAT_V1 1

/* Hard cap on uncompressed_len, the upper limit of compression-max-value-size.
 * It bounds the memory one decompression can allocate. */
#define COMPRESSOR_FRAME_MAX_VALUE_LEN (128 * 1024)

/* The decoded header, in host byte order. This is not the stored layout. */
typedef struct compressorFrameHeader {
    uint32_t uncompressed_len;
    uint32_t dict_id;
    uint8_t algorithm_id;
    uint8_t format_version;
    uint8_t original_encoding;
} compressorFrameHeader;

/* Compresses srclen bytes from src into a new frame, using c. dict is the
 * dictionary to compress with, or NULL for none. When set, it must be built for
 * the same algorithm as c. original_encoding must be at most
 * COMPRESSOR_FRAME_ORIGINAL_ENCODING_MAX.
 *
 * The frame is shrunk to its real size, so its allocation is what the value
 * really costs. Returns NULL when the value must not be compressed (size out
 * of range for c or above COMPRESSOR_FRAME_MAX_VALUE_LEN) or when compression
 * fails. c->last_error tells which. */
sds compressorFrameBuild(compressorAlg *c,
                         uint8_t original_encoding,
                         const void *src,
                         size_t srclen,
                         const compressorDict *dict);

/* Reads and checks the header of frame. Returns COMPRESSOR_ERR_NONE and fills
 * *hdr, or a COMPRESSOR_ERR_FRAME_* code that says what is wrong: no body,
 * unknown format version, unknown algorithm id, or uncompressed_len 0 or above
 * COMPRESSOR_FRAME_MAX_VALUE_LEN. Pass the code to compressorStrerror().
 *
 * It does not check that the dictionary exists; the dictionary registry does
 * that.
 *
 * Frames loaded from RDB must pass this before they reach the keyspace. */
int compressorFrameParse(const_sds frame, compressorFrameHeader *hdr);

/* Decompresses frame into a new sds string, using c. c must be built for the
 * frame's algorithm. dict must be the dictionary that the frame's dict_id
 * names, or NULL when dict_id is 0.
 *
 * Returns NULL when the header is not valid, when c or dict does not match the
 * header, or when the output length is not uncompressed_len. c->last_error then
 * says why. The frame is never changed.
 *
 * This layer does not log, because it does not know the key and may run on a
 * worker thread. The caller logs and counts the failure. */
sds compressorFrameDecompress(compressorAlg *c, const_sds frame, const compressorDict *dict);

/* The net-savings guard. Returns 1 when frame_alloc is at least
 * min_savings_pct percent smaller than orig_alloc, else 0.
 *
 * Both sizes must be allocation sizes (see compressorFrameAllocSize()), not
 * string lengths. The allocator rounds a request up to a size class, so a
 * shorter string can still use the same amount of memory. */
int compressorFrameSavesMemory(size_t orig_alloc, size_t frame_alloc, unsigned int min_savings_pct);

/* Memory the allocator really uses for the sds string s, header included. */
size_t compressorFrameAllocSize(const_sds s);

#endif /* COMPRESSOR_FRAME_H */
