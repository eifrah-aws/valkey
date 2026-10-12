/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSOR_WORKERS_H
#define COMPRESSOR_WORKERS_H

/* Background compression of cold string values.
 *
 * The main thread picks values and the worker threads compress them:
 *
 *   1. compressorCron() picks keys, with random samples or with a cursor
 *      scan (compression-sweeper-policy). For each cold plain string that is
 *      not in a job yet, it copies the bytes and queues a job.
 *   2. A worker compresses its copy into a frame.
 *   3. compressorBeforeSleep() installs the frame, but only when the key
 *      still holds the same bytes as the copy and can still be compressed.
 *      Otherwise the frame is dropped.
 *
 * Both steps pause while a child process (BGSAVE, BGREWRITEAOF) runs, so they
 * do not make the kernel copy memory that the child shares.
 *
 * Workers never touch the keyspace. They only see their own copy. */

#include <stdbool.h>

struct client;

/* Starts compression-threads workers when compression-mode is not off.
 * Called once at startup, from InitServerLast(). Returns the number of
 * workers started, 0 when compression is off. */
int compressorWorkersInit(void);

/* Stops the workers. Used only on a crash, before the memory test. Returns
 * the number of workers stopped. */
int compressorWorkersKill(void);

/* Picks keys and queues compression jobs. Called from serverCron().
 * Returns the number of jobs queued. */
int compressorCron(void);

/* Jobs queued, running, or done but not installed yet. */
unsigned long compressorJobsInFlight(void);

/* Installs the frames that the workers finished. Called from beforeSleep().
 * Returns the number of frames installed. */
int compressorBeforeSleep(void);

/* DEBUG COMPRESS-KEY <key>: compresses the value of the key now, also when it
 * is not cold, and replies OK or an error with the reason. For tests. Returns
 * true when the value was compressed. */
bool compressorDebugCompressKey(struct client *c);

#endif /* COMPRESSOR_WORKERS_H */
