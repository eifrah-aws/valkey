/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Compressing and decompressing string objects on the main thread. See
 * compressor_object.h. */

#include "compressor/compressor_object.h"
#include "compressor/compressor_frame.h"
#include "server.h"
#include "adlist.h"
#include "monotonic.h"
#include "compressor/compressor_stats.h"

#include <errno.h>

/* One compressor for each algorithm, used only by the main thread to
 * decompress. NULL until first use. */
static compressorAlg *main_thread_compressors[COMPRESSOR_ALG_COUNT];

compressorAlg *compressorGetForMainThread(uint8_t algorithm_id) {
    if (algorithm_id == COMPRESSOR_ALG_NONE || algorithm_id >= COMPRESSOR_ALG_COUNT) {
        errno = EINVAL;
        return NULL;
    }
    if (main_thread_compressors[algorithm_id] == NULL) {
        main_thread_compressors[algorithm_id] = newCompressor(algorithm_id, NULL);
        if (main_thread_compressors[algorithm_id] == NULL) {
            errno = ENOTSUP;
            return NULL;
        }
    }
    return main_thread_compressors[algorithm_id];
}

/* Plain copies made for reads while a child process runs. Released by
 * compressorAfterCall() or compressorReleasePlainCopies(). */
static list *plain_copies;
unsigned long compressor_plain_copies;

bool compressorIsCompressedString(const robj *o) {
    return objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_COMPRESSED;
}

void compressorSetCompressedValue(robj *o, sds frame) {
    serverAssert(objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_RAW);
    sds plain = objectGetVal(o);
    compressorStatsAddValue(sdslen(plain), compressorFrameAllocSize(frame));
    objectSetVal(o, frame);
    objectSetEncoding(o, OBJ_ENCODING_COMPRESSED);
    sdsfree(plain);
}

void compressorStringObjectFreed(const robj *o) {
    compressorStatsRemoveValue(compressorStringObjectLen(o), compressorFrameAllocSize(objectGetVal(o)));
}

/* Reads the frame header of the compressed string o. Stops the server if the
 * header is bad. */
static void stringObjectHeader(const robj *o, compressorFrameHeader *hdr) {
    serverAssert(objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_COMPRESSED);
    int err = compressorFrameParse(objectGetVal(o), hdr);
    if (err != COMPRESSOR_ERR_NONE) serverPanic("Bad compressed value: %s", compressorStrerror(err));
}

size_t compressorStringObjectLen(const robj *o) {
    compressorFrameHeader hdr;
    stringObjectHeader(o, &hdr);
    return hdr.uncompressed_len;
}

sds compressorDecompressToSds(const robj *o) {
    monotime start = getMonotonicUs();
    const_sds frame = objectGetVal(o);
    compressorFrameHeader hdr;
    stringObjectHeader(o, &hdr);
    serverAssert(hdr.original_encoding == OBJ_ENCODING_RAW);
    /* Dictionaries come in a later phase, so no frame can need one yet. */
    if (hdr.dict_id != 0) serverPanic("Compressed value needs dictionary %u, which is not loaded", hdr.dict_id);

    compressorAlg *c = compressorGetForMainThread(hdr.algorithm_id);
    if (c == NULL) serverPanic("Compressed value needs algorithm %u, which this build does not have", hdr.algorithm_id);

    sds plain = compressorFrameDecompress(c, frame, NULL);
    compressor_stats.counters.compression_total_decompression_time_us += getMonotonicUs() - start;
    if (plain == NULL) serverPanic("Can't decompress value: %s", compressorStrerror(c->last_error));
    return plain;
}

void compressorDecompressStringObject(robj *o) {
    sds plain = compressorDecompressToSds(o);
    sds frame = objectGetVal(o);
    compressorStatsRemoveValue(sdslen(plain), compressorFrameAllocSize(frame));
    compressor_stats.counters.compression_total_values_decompressed++;
    objectSetVal(o, plain);
    objectSetEncoding(o, OBJ_ENCODING_RAW);
    sdsfree(frame);
}

/* Why a copy, and not a decompress in place:
 *
 * While a child process runs (BGSAVE, BGREWRITEAOF), the parent and the child
 * share memory pages. A decompress in place writes the object and frees the
 * frame, so the kernel must copy those pages for the parent (copy-on-write).
 * A client that reads many compressed keys during a save could make the
 * kernel copy a large part of the dataset. lookupKey() avoids LRU updates
 * during a child for the same reason.
 *
 * So a read during a child gets a temporary plain copy, and the stored value
 * does not change. The copy has the same key, TTL, and LRU/LFU data, because
 * callers read them from the object (for example COPY and TTL).
 *
 * The copy is new memory, not a shared page. It lives until the command ends.
 * If the reply holds the copy (large values are sent by reference), the reply
 * keeps its own reference, and the copy is freed when the reply is sent. */
robj *compressorCreatePlainCopy(robj *val) {
    robj *copy = createObject(OBJ_STRING, compressorDecompressToSds(val));
    copy = objectSetKeyAndExpire(copy, objectGetKey(val), objectGetExpire(val));
    objectSetLRU(copy, objectGetLRU(val));
    compressor_stats.counters.compression_total_temporary_copies_made++;
    return copy;
}

robj *compressorLookupValue(robj *val, int flags) {
    if (!compressorIsCompressedString(val) || (flags & LOOKUP_NODECOMPRESS)) return val;

    if ((flags & LOOKUP_WRITE) || !hasActiveChildProcess()) {
        compressorDecompressStringObject(val);
        return val;
    }

    robj *copy = compressorCreatePlainCopy(val);
    if (plain_copies == NULL) plain_copies = listCreate();
    listAddNodeTail(plain_copies, copy);
    compressor_plain_copies++;
    return copy;
}

int compressorReleasePlainCopies(void) {
    if (plain_copies == NULL || listLength(plain_copies) == 0) return 0;

    int released = 0;
    listIter li;
    listNode *ln;
    listRewind(plain_copies, &li);
    while ((ln = listNext(&li)) != NULL) {
        /* Drops only our reference. A reply that holds the copy keeps it. */
        decrRefCount(listNodeValue(ln));
        listDelNode(plain_copies, ln);
        released++;
    }
    compressor_plain_copies = 0;
    return released;
}

int compressorAfterCall(void) {
    /* Inside a MULTI, a script, or a module call, the outer command may still
     * use the copies. */
    if (server.execution_nesting != 0) return 0;
    return compressorReleasePlainCopies();
}
