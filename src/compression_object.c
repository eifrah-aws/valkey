/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Compressing and decompressing string objects on the main thread. See
 * compression_object.h. */

#include "fmacros.h" /* must come before any system header */
#include "compression_object.h"
#include "compression_value.h"
#include "server.h"
#include "rdb.h"
#include "adlist.h"
#include "bgiteration.h"
#include "monotonic.h"
#include "compression_stats.h"

#include <errno.h>

/* One compressor for each algorithm, used only by the main thread to
 * decompress. NULL until first use. */
static blockCompressor *main_thread_compressors[ALGO_ZSTD + 1];

blockCompressor *compressionGetForMainThread(uint8_t algorithm_id) {
    if (algorithm_id != ALGO_LZ4 && algorithm_id != ALGO_ZSTD) {
        errno = EINVAL;
        return NULL;
    }
    if (main_thread_compressors[algorithm_id] == NULL) {
        main_thread_compressors[algorithm_id] = blockCompressorCreate(algorithm_id, NULL);
        if (main_thread_compressors[algorithm_id] == NULL) {
            errno = ENOTSUP;
            return NULL;
        }
    }
    return main_thread_compressors[algorithm_id];
}

/* Plain copies made for reads while a child process runs. Released by
 * compressionAfterCall() or compressionReleasePlainCopies(). */
static list *plain_copies;
unsigned long compression_plain_copies;

bool compressionIsCompressedString(const robj *o) {
    return objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_COMPRESSED;
}

void compressionSetCompressedValue(robj *o, sds frame) {
    serverAssert(objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_RAW);
    sds plain = objectGetVal(o);
    compressionStatsAddValue(sdslen(plain), compressionValueAllocSize(frame));
    objectSetVal(o, frame);
    objectSetEncoding(o, OBJ_ENCODING_COMPRESSED);
    sdsfree(plain);
}

sds compressionDupFrame(const robj *o) {
    serverAssert(compressionIsCompressedString(o));
    sds frame = sdsdup(objectGetVal(o));
    compressionStatsAddValue(compressionStringObjectLen(o), compressionValueAllocSize(frame));
    return frame;
}

robj *compressionCreateStringObjectFromFrame(sds frame, const char **reason) {
    compressionValueHeader hdr;
    int err = compressionValueParse(frame, &hdr);
    if (err != BLOCK_COMPRESSOR_ERR_NONE) {
        *reason = blockCompressorStrerror(err);
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
    blockCompressor *c = compressionGetForMainThread(hdr.algorithm_id);
    if (c == NULL) {
        *reason = "this build has no backend for the algorithm of the frame";
        return NULL;
    }
    /* A read stops the server on a frame that does not decompress, so check
     * the body now. compressionValueDecompress() also checks the length. */
    sds plain = compressionValueDecompress(c, frame, NULL);
    if (plain == NULL) {
        *reason = blockCompressorStrerror(c->last_error);
        return NULL;
    }
    sdsfree(plain);

    robj *o = createObject(OBJ_STRING, frame);
    objectSetEncoding(o, OBJ_ENCODING_COMPRESSED);
    compressionStatsAddValue(hdr.uncompressed_len, compressionValueAllocSize(frame));
    return o;
}

ssize_t compressionRdbSaveCompressedString(rio *rdb, robj *o) {
    serverAssert(compressionIsCompressedString(o));
    sds frame = objectGetVal(o);
    ssize_t n, nwritten = 0;
    if ((n = rdbSaveLen(rdb, sdslen(frame))) == -1) return -1;
    nwritten += n;
    if ((n = rdbWriteRaw(rdb, frame, sdslen(frame))) == -1) return -1;
    nwritten += n;
    return nwritten;
}

robj *compressionRdbLoadCompressedString(rio *rdb) {
    sds frame = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
    if (frame == NULL) return NULL;
    const char *reason;
    robj *o = compressionCreateStringObjectFromFrame(frame, &reason);
    if (o == NULL) {
        rdbReportError(1, __LINE__, "Bad compressed string: %s", reason);
        sdsfree(frame);
    }
    return o;
}

/* Reads the frame header of the compressed string o. Stops the server if the
 * header is bad. */
static void stringObjectHeader(const robj *o, compressionValueHeader *hdr) {
    serverAssert(objectGetType(o) == OBJ_STRING && objectGetEncoding(o) == OBJ_ENCODING_COMPRESSED);
    int err = compressionValueParse(objectGetVal(o), hdr);
    if (err != BLOCK_COMPRESSOR_ERR_NONE) serverPanic("Bad compressed value: %s", blockCompressorStrerror(err));
}

/* A frame built with a dictionary must drop its user count of the
 * dictionary, and call the backend's release() hook. Dictionaries come in
 * Phase 10. Until then every frame has dict_id 0, so there is nothing to do.
 * Phase 10 can not call release() from here as it is: a free can run on a
 * background thread (FLUSHALL ASYNC), and each blockCompressor instance belongs
 * to one thread. */
static void releaseFrameDictionary(const compressionValueHeader *hdr) {
    serverAssert(hdr->dict_id == 0);
}

void compressionStringObjectFreed(const robj *o) {
    compressionValueHeader hdr;
    stringObjectHeader(o, &hdr);
    compressionStatsRemoveValue(hdr.uncompressed_len, compressionValueAllocSize(objectGetVal(o)));
    releaseFrameDictionary(&hdr);
}

size_t compressionStringObjectLen(const robj *o) {
    compressionValueHeader hdr;
    stringObjectHeader(o, &hdr);
    return hdr.uncompressed_len;
}

sds compressionDecompressToSds(const robj *o) {
    monotime start = getMonotonicUs();
    const_sds frame = objectGetVal(o);
    compressionValueHeader hdr;
    stringObjectHeader(o, &hdr);
    serverAssert(hdr.original_encoding == OBJ_ENCODING_RAW);
    /* Dictionaries come in a later phase, so no frame can need one yet. */
    if (hdr.dict_id != 0) serverPanic("Compressed value needs dictionary %u, which is not loaded", hdr.dict_id);

    blockCompressor *c = compressionGetForMainThread(hdr.algorithm_id);
    if (c == NULL) serverPanic("Compressed value needs algorithm %u, which this build does not have", hdr.algorithm_id);

    sds plain = compressionValueDecompress(c, frame, NULL);
    compression_stats.counters.compression_total_decompression_time_us += getMonotonicUs() - start;
    if (plain == NULL) serverPanic("Can't decompress value: %s", blockCompressorStrerror(c->last_error));
    return plain;
}

void compressionDecompressStringObject(robj *o) {
    sds plain = compressionDecompressToSds(o);
    sds frame = objectGetVal(o);
    compressionStatsRemoveValue(sdslen(plain), compressionValueAllocSize(frame));
    compression_stats.counters.compression_total_values_decompressed++;
    objectSetVal(o, plain);
    objectSetEncoding(o, OBJ_ENCODING_RAW);
    sdsfree(frame);
}

robj *compressionCreatePlainClone(robj *o) {
    sds plain = compressionDecompressToSds(o);
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
robj *compressionCreatePlainCopy(robj *val) {
    robj *copy = createObject(OBJ_STRING, compressionDecompressToSds(val));
    copy = objectSetKeyAndExpire(copy, objectGetKey(val), objectGetExpire(val));
    objectSetLRU(copy, objectGetLRU(val));
    compression_stats.counters.compression_total_temporary_copies_made++;
    return copy;
}

robj *compressionLookupValue(robj *val, int flags) {
    if (!compressionIsCompressedString(val) || (flags & LOOKUP_NODECOMPRESS)) return val;

    if (flags & LOOKUP_WRITE) {
        /* The forkless save thread may be reading the frame. Write commands
         * wait for it, so only VM_OpenKey() gets here, and it refuses the
         * key after the lookup. */
        if (bgIteration_isEntryInuse(val)) return val;
        compressionDecompressStringObject(val);
        return val;
    }
    /* A decompress in place frees the frame. Someone else that holds val
     * (refcount above 1, for example the forkless save thread) may still read
     * it. A child process shares all memory pages, so then every read gets a
     * copy. */
    if (!hasActiveChildProcess() && objectGetRefcount(val) == 1) {
        compressionDecompressStringObject(val);
        return val;
    }

    robj *copy = compressionCreatePlainCopy(val);
    if (plain_copies == NULL) plain_copies = listCreate();
    listAddNodeTail(plain_copies, copy);
    compression_plain_copies++;
    return copy;
}

int compressionReleasePlainCopiesSince(unsigned long mark) {
    /* The copies of a command are at the end of the list, after the copies of
     * the command that runs it (EXEC, EVAL, a module command). The outer
     * command never uses them, and a reply that holds a copy keeps its own
     * reference. So a script that reads many keys during a child process
     * holds one copy at a time, not all of them. */
    int released = 0;
    while (compression_plain_copies > mark) {
        listNode *ln = listLast(plain_copies);
        /* Drops only our reference. A reply that holds the copy keeps it. */
        decrRefCount(listNodeValue(ln));
        listDelNode(plain_copies, ln);
        compression_plain_copies--;
        released++;
    }
    return released;
}

int compressionReleasePlainCopies(void) {
    return compressionReleasePlainCopiesSince(0);
}

int compressionAfterCall(void) {
    /* Inside a MULTI, a script, or a module call, call() releases the copies
     * of each inner command. */
    if (server.execution_nesting != 0) return 0;
    return compressionReleasePlainCopies();
}
