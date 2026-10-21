/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_WORKERS_H
#define COMPRESSION_WORKERS_H

/* Background compression of cold string values.
 *
 * The main thread picks values and the worker threads compress them:
 *
 *   1. compressionCron() picks keys, with random samples or with a cursor
 *      scan (compression-sweeper-policy). For each cold plain string that is
 *      not in a job yet, it copies the bytes and queues a job.
 *   2. A worker compresses its copy into a frame.
 *   3. compressionBeforeSleep() installs the frame, but only when the key
 *      still holds the same bytes as the copy and can still be compressed.
 *      Otherwise the frame is dropped.
 *
 * Both steps pause while a child process (BGSAVE, BGREWRITEAOF) runs, so they
 * do not make the kernel copy memory that the child shares.
 *
 * Values that come from the primary (replica apply) or from a slot import do
 * not wait until they are cold. compressionKeyModified() adds their key names
 * to a set of incoming keys, and step 1 queues jobs for them first, with no
 * coldness test. This avoids a memory spike during an import or a catch-up.
 *
 * Workers never touch the keyspace. They only see their own copy. */

#include <stdbool.h>

struct client;
struct serverDb;
struct serverObject;

/* Starts compression-threads workers when compression-mode is not off.
 * Called once at startup, from InitServerLast(). Returns the number of
 * workers started, 0 when compression is off. */
int compressionWorkersInit(void);

/* Stops the workers. Used only on a crash, before the memory test. Returns
 * the number of workers stopped. */
int compressionWorkersKill(void);

/* Picks keys and queues compression jobs. Called from serverCron().
 * Returns the number of jobs queued. */
int compressionCron(void);

/* Jobs queued, running, or done but not installed yet. */
unsigned long compressionJobsInFlight(void);

/* signalModifiedKey() calls this for each key that a command changed. When
 * compression is on and c is the primary or a slot-import client, adds the
 * key name to the incoming keys. Copies no value. Returns true when the key
 * was added, false when it was not (another client, compression off, or the
 * key is already there). */
bool compressionKeyModified(struct client *c, struct serverDb *db, struct serverObject *key);

/* The number of incoming keys that wait for a job. */
unsigned long compressionIncomingPendingKeys(void);

/* True while the cursor sweeper is in its backoff pause. */
bool compressionSweeperIsPaused(void);

/* The apply function of compression-sweeper-backoff-seconds. Ends a backoff
 * pause, so a change of the setting wakes the sweeper. CONFIG SET does not call
 * it when the value does not change. Returns 1, it can not fail. */
int compressionSweeperBackoffApply(const char **err);

/* Installs the frames that the workers finished, and queues jobs for
 * incoming keys. Called from beforeSleep(). Returns the number of frames
 * installed. */
int compressionBeforeSleep(void);

/* DEBUG COMPRESS-KEY <key>: compresses the value of the key now, also when it
 * is not cold, and replies OK or an error with the reason. For tests. Returns
 * true when the value was compressed. */
bool compressionDebugCompressKey(struct client *c);

#endif /* COMPRESSION_WORKERS_H */
