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
#include <sys/types.h>

struct serverObject;
struct _rio;

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

/* Returns a copy of the frame of the compressed string o, and adds the copy to
 * INFO compression. The caller stores it in a new compressed string object.
 * Used by COPY, so the copy is not decompressed and compressed again. The new
 * object gets new LRU/LFU data, so the copy starts hot, like a COPY of a plain
 * value. compressorCreatePlainCopy() is different: it keeps the LRU/LFU data. */
sds compressorDupFrame(const struct serverObject *o);

/* Returns a new compressed string object that holds frame, a frame loaded
 * from RDB or a DUMP payload, and adds it to INFO compression. The object
 * takes over frame. Checks the header, and decompresses the body once into a
 * temporary buffer, so a bad frame never reaches the keyspace (a RESTORE
 * payload comes from a client). Returns NULL and sets *reason when the frame
 * is not valid. The caller then still owns frame. Main thread only. */
struct serverObject *compressorCreateStringObjectFromFrame(sds frame, const char **reason);

/* Writes the compressed string o to an RDB as RDB_TYPE_STRING_COMPRESSED
 * data: the frame length, then the frame bytes as they are, without LZF.
 * rdb may be NULL, to count the bytes only. Returns the number of bytes
 * written, or -1 on a write error. */
ssize_t compressorRdbSaveCompressedString(struct _rio *rdb, struct serverObject *o);

/* Reads RDB_TYPE_STRING_COMPRESSED data from an RDB, written by
 * compressorRdbSaveCompressedString(), and returns a new compressed string
 * object (see compressorCreateStringObjectFromFrame()). The value stays
 * compressed. Returns NULL on a read error, or when the frame is not valid;
 * a bad frame is reported as a corrupt RDB. Main thread only. */
struct serverObject *compressorRdbLoadCompressedString(struct _rio *rdb);

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

/* Returns a new plain string object with the value of the compressed string
 * o, and the same key and TTL. o does not change. Forkless save gives this
 * clone to its save thread, in place of o (tryCloneDbEntry()). */
struct serverObject *compressorCreatePlainClone(struct serverObject *o);

/* Returns a new plain copy of the compressed string val, with the same key,
 * TTL, and LRU/LFU data. val does not change. The caller owns the copy and
 * must release it with decrRefCount(). See compressor_object.c for why. */
struct serverObject *compressorCreatePlainCopy(struct serverObject *val);

/* lookupKey() calls this for the value it found. flags are LOOKUP_* flags.
 * Returns the value the caller must use:
 *
 *   - val itself, when it is not a compressed string, or when
 *     LOOKUP_NODECOMPRESS is set.
 *   - val itself, still compressed, for a write (LOOKUP_WRITE) while the
 *     forkless save thread holds val. VM_OpenKey() then refuses the key.
 *   - val itself, decompressed in place, for any other write, or for a read
 *     when no child process runs and nobody else holds val (refcount 1). A
 *     client uses the key, so it stays plain.
 *   - a temporary plain copy, for a read while a child process runs, or while
 *     someone else holds val (for example the forkless save thread). val
 *     stays compressed. The copy goes on the cleanup list, and
 *     compressorAfterCall() releases it when the command ends. */
struct serverObject *compressorLookupValue(struct serverObject *val, int flags);

/* afterCommand() calls this. When the outermost command is done
 * (server.execution_nesting is 0), releases the temporary plain copies.
 * Returns the number released, 0 inside a MULTI, script, or module call. */
int compressorAfterCall(void);

/* The number of temporary plain copies on the cleanup list. call() reads it
 * when a command starts, and gives it to compressorReleasePlainCopiesSince()
 * when the command ends. */
static inline unsigned long compressorPlainCopiesListSize(void) {
    return compressor_plain_copies;
}

/* call() calls this when a command ends, also for a command inside a MULTI, a
 * script, or a module call. Releases the temporary plain copies that the
 * command made: the ones added after mark, the value of
 * compressorPlainCopiesListSize() when the command started. Returns the number
 * released. */
int compressorReleasePlainCopiesSince(unsigned long mark);

/* Releases all temporary plain copies. Returns the number released.
 * compressorBeforeSleep() calls it, for copies made outside a command. */
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
