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
#include "rdb.h"
#include "adlist.h"
#include "bgiteration.h"
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

sds compressorDupFrame(const robj *o) {
    serverAssert(compressorIsCompressedString(o));
    sds frame = sdsdup(objectGetVal(o));
    compressorStatsAddValue(compressorStringObjectLen(o), compressorFrameAllocSize(frame));
    return frame;
}

robj *compressorCreateStringObjectFromFrame(sds frame, const char **reason) {
    compressorFrameHeader hdr;
    int err = compressorFrameParse(frame, &hdr);
    if (err != COMPRESSOR_ERR_NONE) {
        *reason = compressorStrerror(err);
        return NULL;
    }
    if (hdr.original_encoding != OBJ_ENCODING_RAW) {
        *reason = "the frame is not a string";
        return NULL;
    }
    /* Dictionaries come in a later phase. */
    if (hdr.dict_id != 0) {
        *reason = "the frame needs a dictionary";
        return NULL;
    }
    compressorAlg *c = compressorGetForMainThread(hdr.algorithm_id);
    if (c == NULL) {
        *reason = "this build has no backend for the algorithm of the frame";
        return NULL;
    }
    /* A read stops the server on a frame that does not decompress, so check
     * the body now. compressorFrameDecompress() also checks the length. */
    sds plain = compressorFrameDecompress(c, frame, NULL);
    if (plain == NULL) {
        *reason = compressorStrerror(c->last_error);
        return NULL;
    }
    sdsfree(plain);

    robj *o = createObject(OBJ_STRING, frame);
    objectSetEncoding(o, OBJ_ENCODING_COMPRESSED);
    compressorStatsAddValue(hdr.uncompressed_len, compressorFrameAllocSize(frame));
    return o;
}

ssize_t compressorRdbSaveCompressedString(rio *rdb, robj *o) {
    serverAssert(compressorIsCompressedString(o));
    sds frame = objectGetVal(o);
    ssize_t n, nwritten = 0;
    if ((n = rdbSaveLen(rdb, sdslen(frame))) == -1) return -1;
    nwritten += n;
    if ((n = rdbWriteRaw(rdb, frame, sdslen(frame))) == -1) return -1;
    nwritten += n;
    return nwritten;
}

robj *compressorRdbLoadCompressedString(rio *rdb) {
    sds frame = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
    if (frame == NULL) return NULL;
    const char *reason;
    robj *o = compressorCreateStringObjectFromFrame(frame, &reason);
    if (o == NULL) {
        rdbReportError(1, __LINE__, "Bad compressed string: %s", reason);
        sdsfree(frame);
    }
    return o;
}

/* Reads the frame header of the compressed string o. Stops the server if the
 * header is bad. */
static void stringObjectHeader(const robj *o, compressorFrameHeader *hdr) {
    serverAssert(objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_COMPRESSED);
    int err = compressorFrameParse(objectGetVal(o), hdr);
    if (err != COMPRESSOR_ERR_NONE) serverPanic("Bad compressed value: %s", compressorStrerror(err));
}

/* A frame built with a dictionary must drop its user count of the
 * dictionary, and call the backend's release() hook. Dictionaries come in
 * Phase 10. Until then every frame has dict_id 0, so there is nothing to do.
 * Phase 10 can not call release() from here as it is: a free can run on a
 * background thread (FLUSHALL ASYNC), and each compressorAlg instance belongs
 * to one thread. */
static void releaseFrameDictionary(const compressorFrameHeader *hdr) {
    serverAssert(hdr->dict_id == 0);
}

void compressorStringObjectFreed(const robj *o) {
    compressorFrameHeader hdr;
    stringObjectHeader(o, &hdr);
    compressorStatsRemoveValue(hdr.uncompressed_len, compressorFrameAllocSize(objectGetVal(o)));
    releaseFrameDictionary(&hdr);
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

robj *compressorCreatePlainClone(robj *o) {
    sds plain = compressorDecompressToSds(o);
    robj *clone = createStringObjectWithKeyAndExpire(plain, sdslen(plain), objectGetKey(o), objectGetExpire(o));
    sdsfree(plain);
    return clone;
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

    if (flags & LOOKUP_WRITE) {
        /* The forkless save thread may be reading the frame. Write commands
         * wait for it, so only VM_OpenKey() gets here, and it refuses the
         * key after the lookup. */
        if (bgIteration_isEntryInuse(val)) return val;
        compressorDecompressStringObject(val);
        return val;
    }
    /* A decompress in place frees the frame. Someone else that holds val
     * (refcount above 1, for example the forkless save thread) may still read
     * it. A child process shares all memory pages, so then every read gets a
     * copy. */
    if (!hasActiveChildProcess() && objectGetRefcount(val) == 1) {
        compressorDecompressStringObject(val);
        return val;
    }

    robj *copy = compressorCreatePlainCopy(val);
    if (plain_copies == NULL) plain_copies = listCreate();
    listAddNodeTail(plain_copies, copy);
    compressor_plain_copies++;
    return copy;
}

int compressorReleasePlainCopiesSince(unsigned long mark) {
    /* The copies of a command are at the end of the list, after the copies of
     * the command that runs it (EXEC, EVAL, a module command). The outer
     * command never uses them, and a reply that holds a copy keeps its own
     * reference. So a script that reads many keys during a child process
     * holds one copy at a time, not all of them. */
    int released = 0;
    while (compressor_plain_copies > mark) {
        listNode *ln = listLast(plain_copies);
        /* Drops only our reference. A reply that holds the copy keeps it. */
        decrRefCount(listNodeValue(ln));
        listDelNode(plain_copies, ln);
        compressor_plain_copies--;
        released++;
    }
    return released;
}

int compressorReleasePlainCopies(void) {
    return compressorReleasePlainCopiesSince(0);
}

int compressorAfterCall(void) {
    /* Inside a MULTI, a script, or a module call, call() releases the copies
     * of each inner command. */
    if (server.execution_nesting != 0) return 0;
    return compressorReleasePlainCopies();
}
