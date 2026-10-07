/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSOR_OBJECT_H
#define COMPRESSOR_OBJECT_H

/* Compressing and decompressing string objects (robj) on the main thread.
 *
 * A compressed string is an robj with encoding OBJ_ENCODING_COMPRESSED. Its
 * value is a frame (see compressor_frame.h) in place of the plain sds. The robj
 * itself does not change, so the key, the TTL, and the LRU data stay as they
 * are. Only the value buffer and the encoding change. */

#include "compressor/compressor_alg.h"
#include "sds.h"

#include <stdbool.h>

struct serverObject;

/* The compressor that the main thread uses for an algorithm. Each one is made
 * the first time it is needed, so a frame from any algorithm can be read, even
 * when compression-mode is off. Returns NULL and sets errno when it can't:
 * EINVAL for an unknown id, ENOTSUP when this build has no backend for the
 * algorithm. Main thread only. */
compressorAlg *compressorGetForMainThread(uint8_t algorithm_id);

/* True when o is a string whose value is a compressed frame. Other types may
 * use OBJ_ENCODING_COMPRESSED later, so code for strings must check both. */
bool compressorIsCompressedString(const struct serverObject *o);

/* Makes o hold frame, the compressed form of its plain value, and frees the
 * plain value. o must be a RAW string, and o takes over frame. */
void compressorSetCompressedValue(struct serverObject *o, sds frame);

/* Updates INFO compression before a compressed string o is freed. Can run on
 * a background thread (FLUSHALL ASYNC). */
void compressorStringObjectFreed(const struct serverObject *o);

/* The number of temporary plain copies on the cleanup list. Changed only by
 * compressor_object.c. Read through compressorHasPlainCopies(). */
extern unsigned long compressor_plain_copies;

/* True when the cleanup list has copies to release. Cheap, for hot paths. */
static inline bool compressorHasPlainCopies(void) {
    return compressor_plain_copies != 0;
}

/* Returns a new plain copy of the compressed string val, with the same key,
 * TTL, and LRU/LFU data. val does not change. The caller owns the copy and
 * must release it with decrRefCount(). See compressor_object.c for why. */
struct serverObject *compressorCreatePlainCopy(struct serverObject *val);

/* lookupKey() calls this for the value it found. flags are LOOKUP_* flags.
 * Returns the value the caller must use:
 *
 *   - val itself, when it is not a compressed string, or when
 *     LOOKUP_NODECOMPRESS is set.
 *   - val itself, decompressed in place, for a write (LOOKUP_WRITE), or when
 *     no child process runs. A client uses the key, so it stays plain.
 *   - a temporary plain copy, for a read while a child process runs. val
 *     stays compressed. The copy goes on the cleanup list, and
 *     compressorAfterCall() releases it. */
struct serverObject *compressorLookupValue(struct serverObject *val, int flags);

/* afterCommand() calls this. When the outermost command is done
 * (server.execution_nesting is 0), releases the temporary plain copies.
 * Returns the number released, 0 inside a MULTI, script, or module call. */
int compressorAfterCall(void);

/* Releases all temporary plain copies. Returns the number released.
 * compressorBeforeSleep() calls it too, for copies made outside a command. */
int compressorReleasePlainCopies(void);

/* Decompresses the value of o in place. After this o is a RAW string again.
 * o must be a compressed string. Stops the server if the frame can not be
 * decompressed, because frames are checked when they are made. */
void compressorDecompressStringObject(struct serverObject *o);

/* The length of the plain value of the compressed string o, read from the
 * frame header. Does not decompress. */
size_t compressorStringObjectLen(const struct serverObject *o);

/* Returns a new plain sds with the value of the compressed string o, and does
 * not change o. The caller must free it. Stops the server if the frame can not
 * be decompressed. */
sds compressorDecompressToSds(const struct serverObject *o);

#endif /* COMPRESSOR_OBJECT_H */
