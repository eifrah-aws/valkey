/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_CONFIG_H
#define COMPRESSION_CONFIG_H

/* Settings for inline compression.
 *
 * The settings that users can change are in config.c, like all other
 * settings. Their values are in the server struct (server.compression_*).
 * This file has the limits of those settings, and the fixed values of the
 * settings that v1 does not let users change. */

#include "compression_value.h"

/* compression-threads */
#define COMPRESSION_THREADS_MIN 1
#define COMPRESSION_THREADS_MAX 16
#define COMPRESSION_THREADS_DEFAULT 1

/* compression-min-value-size and compression-max-value-size. The largest
 * value is the frame limit, so there is only one place that says 128 KiB. */
#define COMPRESSION_VALUE_SIZE_MAX COMPRESSION_VALUE_MAX_LEN
#define COMPRESSION_MIN_VALUE_SIZE_DEFAULT 256
#define COMPRESSION_MAX_VALUE_SIZE_DEFAULT COMPRESSION_VALUE_SIZE_MAX

/* compression-dict-size */
#define COMPRESSION_DICT_SIZE_MIN 1024
#define COMPRESSION_DICT_SIZE_MAX (1024 * 1024)
#define COMPRESSION_DICT_SIZE_DEFAULT (100 * 1024)

/* compression-max-inflight-requests: the most compression jobs at the same
 * time. A job counts from the moment it is queued until the main thread
 * installs or drops the result. When the limit is reached, the sweeper does
 * not pick new values. Each job holds a copy of the plain value, so with
 * values of at most 128 KiB, the default keeps the copies under 125 MiB. In
 * the benchmark, 100 compressed only half of 1M values in 600 s, and 1000
 * compressed all of them in 362 s. */
#define COMPRESSION_MAX_INFLIGHT_REQUESTS_MIN 1
#define COMPRESSION_MAX_INFLIGHT_REQUESTS_MAX 10000
#define COMPRESSION_MAX_INFLIGHT_REQUESTS_DEFAULT 1000

/* compression-lfu-threshold: a value is cold when its LFU counter is at most
 * this number. This test is used with LFU eviction, in place of
 * COMPRESSION_MIN_IDLE_SECONDS. A new key starts at 5 (LFU_INIT_VAL), so with
 * the default of 4 it is hot until the counter decays. With the default
 * lfu-decay-time of 1, this takes at most one minute. With 5 or more, a new
 * key is cold at once. With 0, only keys that were not used for a long time
 * are cold. */
#define COMPRESSION_LFU_THRESHOLD_MIN 0
#define COMPRESSION_LFU_THRESHOLD_MAX 255
#define COMPRESSION_LFU_THRESHOLD_DEFAULT 4

/* compression-sweeper-policy: how the sweeper picks the keys it checks.
 *
 *   random  random samples from all databases. When most keys are already
 *           compressed, most samples find nothing to do, and the last plain
 *           keys are found slowly.
 *   cursor  a scan over all databases with a cursor, one bucket at a time.
 *           One pass checks each key one time. */
#define COMPRESSION_SWEEPER_POLICY_RANDOM 0
#define COMPRESSION_SWEEPER_POLICY_CURSOR 1
#define COMPRESSION_SWEEPER_POLICY_DEFAULT COMPRESSION_SWEEPER_POLICY_CURSOR

/* compression-sweeper-backoff-seconds: with the cursor policy, the sweeper
 * pauses for this many seconds after a pass in which less than
 * COMPRESSION_BACKOFF_MIN_ELIGIBLE_PCT of the checked keys were eligible.
 * Then a new pass starts. 0 turns the backoff off. The default matches
 * COMPRESSION_MIN_IDLE_SECONDS: keys that are hot now can be cold by then.
 * A change of this setting (CONFIG SET to another value) ends a pause at once. */
#define COMPRESSION_SWEEPER_BACKOFF_SECONDS_MIN 0
#define COMPRESSION_SWEEPER_BACKOFF_SECONDS_MAX 3600
#define COMPRESSION_SWEEPER_BACKOFF_SECONDS_DEFAULT 60

/* compression-sweeper-samples: the most keys the sweeper looks at in one
 * compressionCron() call. With the cursor policy, it is also the most buckets
 * the scan visits in one call, so a table with many empty buckets does not
 * make the call long. This keeps the work on the main thread small, also
 * when compression-max-inflight-requests is large. The default comes from
 * the benchmark (design doc section 6.1.1). */
#define COMPRESSION_SWEEPER_SAMPLES_MIN 1
#define COMPRESSION_SWEEPER_SAMPLES_MAX 10000
#define COMPRESSION_SWEEPER_SAMPLES_DEFAULT 500

/* The random policy reads samples in batches of this size, so its array on
 * the stack does not grow with compression-sweeper-samples. */
#define COMPRESSION_SWEEPER_SAMPLE_BATCH 128

/* Fixed values. Users can not change these in v1. A later version may turn
 * them into settings. */

/* The backoff of the cursor sweeper starts when less than this many percent
 * of the keys checked in a pass were eligible. Not 0: a pass over 1M keys
 * that finds a few new cold keys costs as much as one that finds none. The
 * few keys wait for the next pass. */
#define COMPRESSION_BACKOFF_MIN_ELIGIBLE_PCT 1

/* The net-savings guard. A compressed value is kept only when its memory is at
 * least this many percent smaller than the memory of the plain value. If not,
 * the compressed value is dropped and the plain value stays. */
#define COMPRESSION_MIN_SAVINGS_PCT 10

/* A value is cold when nobody used it for this many seconds. Only cold values
 * are compressed. This test is used with LRU eviction and with noeviction. It
 * is also how long a key stays in the hot-key list after its last read. */
#define COMPRESSION_MIN_IDLE_SECONDS 60

/* The most keys in the hot-key list. The list holds keys that clients read
 * recently. The sweeper does not compress these keys. When the list is full,
 * the key that was read longest ago is removed. */
#define COMPRESSION_HOT_KEYS_MAX 100000


/* The first dictionary is trained when the keyspace has this many keys. A
 * training also needs at least this many sample values. */
#define COMPRESSION_DICT_MIN_TRAINING_KEYS 1000

/* The most sample values that one training uses. */
#define COMPRESSION_DICT_MAX_TRAINING_KEYS 10000

/* The size of the buffer that holds the sample values for one training. It is
 * allocated when a training starts and freed when it ends. */
#define COMPRESSION_TRAINING_BUFFER_SIZE (16 * 1024 * 1024)

/* Train a new dictionary when the compression ratio gets worse. The number is
 * a percent of the ratio right after the last training. */
#define COMPRESSION_DICT_DRIFT_PCT 70

/* Train a new dictionary after this many seconds, even when the ratio is still
 * good. 0 turns this off, so only the other reasons start a training. */
#define COMPRESSION_DICT_REFRESH_INTERVAL 0

/* The most dictionaries that can exist at the same time. Old frames still need
 * the old dictionary, so after a new training there are at least two. When the
 * limit is reached, no new training starts. */
#define COMPRESSION_DICT_MAX_VERSIONS 4

/* Checks the settings that depend on each other. Returns 1 when they are
 * fine. Returns 0 and sets *err when they are not. Used at startup and after
 * CONFIG SET. */
int compressionConfigCheck(const char **err);

/* The name of a compression-mode value, for INFO: "off" for ALGO_NONE, else
 * the algorithm name. Never returns NULL. */
const char *compressionModeName(int mode);

#endif /* COMPRESSION_CONFIG_H */
