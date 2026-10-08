/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Background compression of cold string values. See compressor_workers.h. */

#include "server.h"
#include "compressor/compressor_workers.h"
#include "compressor/compressor_config.h"
#include "compressor/compressor_frame.h"
#include "compressor/compressor_object.h"
#include "compressor/compressor_stats.h"
#include "lrulfu.h"
#include "mutexqueue.h"
#include "mt19937-64.h"
#include "monotonic.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>

/* One value to compress. The main thread makes it, a worker fills in frame,
 * and the main thread installs or drops it. */
typedef struct compressorJob {
    int dbid;
    sds key;
    sds inflight_name;     /* The job's name in inflight_keys. Owned by that table. */
    sds input;             /* Own copy of the plain value. Kept until install, for the compare. */
    sds frame;             /* Set by the worker. NULL when the value does not get smaller. */
    long long compress_us; /* Set by the worker: time spent compressing. */
} compressorJob;

static mutexQueue *pending_jobs; /* Main thread to workers. NULL when compression is off. */
static mutexQueue *done_jobs;    /* Workers to main thread. */

/* The keys of the jobs that are queued, running, or done but not installed
 * yet, so a key is never in two jobs at once. Main thread only. */
static hashtable *inflight_keys;

static pthread_t worker_threads[COMPRESSOR_THREADS_MAX];
static int num_workers;

/* The name of a key in inflight_keys: the db id, then the key bytes. */
static sds inflightName(int dbid, const_sds key) {
    sds name = sdsnewlen(&dbid, sizeof(dbid));
    return sdscatlen(name, key, sdslen(key));
}

static unsigned long inflightCount(void) {
    return inflight_keys ? hashtableSize(inflight_keys) : 0;
}

unsigned long compressorQueueLength(void) {
    return inflightCount();
}

static void freeJob(compressorJob *job) {
    /* The table owns inflight_name and frees it. */
    hashtableDelete(inflight_keys, job->inflight_name);
    sdsfree(job->key);
    sdsfree(job->input);
    sdsfree(job->frame);
    zfree(job);
}

/* ----------------------------- Worker threads ----------------------------- */

static void *compressorWorkerMain(void *arg) {
    UNUSED(arg);
    valkey_set_thread_title("compressor");
    makeThreadKillable();

    /* Only the main thread must get the watchdog signal. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    int err = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
    if (err) serverLog(LL_WARNING, "Warning: can't mask SIGALRM in compressor thread: %s", strerror(err));

    /* compression-mode can't change at runtime, so the worker's compressor
     * stays valid for the life of the thread. */
    compressorAlg *c = newCompressor(server.compression_mode, NULL);
    serverAssert(c != NULL);

    while (1) {
        compressorJob *job = mutexQueuePop(pending_jobs, true);
        monotime start = getMonotonicUs();
        job->frame = compressorFrameBuild(c, OBJ_ENCODING_RAW, job->input, sdslen(job->input), NULL);
        job->compress_us = (long long)(getMonotonicUs() - start);
        /* A frame that is not smaller can't save memory. Drop it here, so a
         * done job never holds two buffers that are both big. */
        if (job->frame != NULL && sdslen(job->frame) >= sdslen(job->input)) {
            sdsfree(job->frame);
            job->frame = NULL;
        }
        mutexQueueAdd(done_jobs, job);
    }
    return NULL;
}

int compressorWorkersInit(void) {
    if (server.compression_mode == COMPRESSOR_ALG_NONE) return 0;

    pending_jobs = mutexQueueCreate();
    done_jobs = mutexQueueCreate();
    inflight_keys = hashtableCreate(&setHashtableType);

    pthread_attr_t attr;
    serverInitThreadAttribute(&attr);
    for (int i = 0; i < server.compression_threads; i++) {
        int err = pthread_create(&worker_threads[i], &attr, compressorWorkerMain, NULL);
        if (err) {
            serverLog(LL_WARNING, "Fatal: Can't start compressor threads. Error message: %s", strerror(err));
            exit(1);
        }
        num_workers++;
    }
    pthread_attr_destroy(&attr);
    return num_workers;
}

int compressorWorkersKill(void) {
    int killed = 0;
    for (int i = 0; i < num_workers; i++) {
        if (pthread_equal(worker_threads[i], pthread_self())) continue;
        if (pthread_cancel(worker_threads[i]) != 0) continue;
        int err = pthread_join(worker_threads[i], NULL);
        if (err) {
            serverLog(LL_WARNING, "Compressor thread #%d can not be joined: %s", i, strerror(err));
        } else {
            serverLog(LL_WARNING, "Compressor thread #%d terminated", i);
            killed++;
        }
    }
    return killed;
}

/* --------------------------- Picking values (cron) --------------------------- */

/* A value is cold when nobody used it for a while. The 24-bit field holds
 * either LRU or LFU data, so we use the test that matches the policy. Reading
 * the field does not change it. */
static bool isCold(robj *o) {
    if (lrulfu_isUsingLFU()) {
        uint8_t freq;
        lfu_getFrequency(objectGetLRU(o), &freq);
        return freq <= COMPRESSOR_LFU_THRESHOLD;
    }
    return lru_getIdleSecs(objectGetLRU(o)) >= COMPRESSOR_MIN_IDLE_SECONDS;
}

/* Can the value o be compressed now? Used when a value is picked and again
 * when its frame is installed, because the value or the settings can change
 * in between. Returns false and sets errno when it can't:
 *
 *   EALREADY  already compressed
 *   EINVAL  not a plain (RAW) string
 *   EBUSY   someone else holds the object, and may read the plain bytes
 *   ERANGE  the size is outside the configured range
 *   EAGAIN  the value is not cold */
static bool canCompress(robj *o) {
    if (compressorIsCompressedString(o)) {
        errno = EALREADY;
        return false;
    }
    if (objectGetType(o) != OBJ_STRING || objectGetEncoding(o) != OBJ_ENCODING_RAW) {
        errno = EINVAL;
        return false;
    }
    if (objectGetRefcount(o) != 1) {
        errno = EBUSY;
        return false;
    }
    size_t len = sdslen(objectGetVal(o));
    if (len < server.compression_min_value_size || len > server.compression_max_value_size) {
        errno = ERANGE;
        return false;
    }
    if (!isCold(o)) {
        errno = EAGAIN;
        return false;
    }
    return true;
}

/* Counts a key that canCompress() refused, by the errno it set. */
static void countSkippedKey(int err) {
    switch (err) {
    case EALREADY: compressor_stats.counters.keys_skipped_already_compressed++; break;
    case EINVAL: compressor_stats.counters.keys_skipped_not_string++; break;
    case EBUSY: compressor_stats.counters.keys_skipped_in_use++; break;
    case ERANGE: compressor_stats.counters.keys_skipped_size++; break;
    case EAGAIN: compressor_stats.counters.keys_skipped_hot++; break;
    default: break;
    }
}

/* Queues a job for the value o of a key in db dbid. Returns false and sets
 * errno to EEXIST when the key is already in a job. */
static bool queueJob(int dbid, robj *o) {
    sds name = inflightName(dbid, objectGetKey(o));
    if (!hashtableAdd(inflight_keys, name)) {
        sdsfree(name);
        errno = EEXIST;
        return false;
    }

    compressorJob *job = zmalloc(sizeof(*job));
    job->dbid = dbid;
    job->key = sdsdup(objectGetKey(o));
    job->inflight_name = name;
    job->input = sdsdup(objectGetVal(o));
    job->frame = NULL;
    job->compress_us = 0;
    mutexQueueAdd(pending_jobs, job);
    return true;
}

/* A random number in [0, n). n must not be 0. random() has only 31 bits, so
 * it can't cover a large key count. Numbers in the last, incomplete block of
 * the 64-bit range are drawn again, so every result has the same chance. */
static unsigned long long randomBelow(unsigned long long n) {
    unsigned long long limit = ULLONG_MAX - (ULLONG_MAX % n);
    unsigned long long r;
    do {
        r = genrand64_int64();
    } while (r >= limit);
    return r % n;
}

/* Picks a database with a chance that matches its share of all keys.
 * total is the number of keys in all databases, and must not be 0. */
static serverDb *pickDb(unsigned long long total) {
    unsigned long long target = randomBelow(total);
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        unsigned long long size = kvstoreSize(db->keys);
        if (target < size) return db;
        target -= size;
    }
    return NULL;
}

int compressorCron(void) {
    if (pending_jobs == NULL || server.loading) return 0;
    /* Installing a frame changes memory that a child process (BGSAVE,
     * BGREWRITEAOF) still shares, and makes the kernel copy those pages. */
    if (hasActiveChildProcess()) {
        compressor_stats.counters.compression_paused_during_save++;
        return 0;
    }

    unsigned long long total = 0;
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] != NULL) total += kvstoreSize(server.db[i]->keys);
    }
    if (total == 0) return 0;

    /* Look at no more than COMPRESSOR_SAMPLES_PER_CRON keys per call, and stop
     * when compression-max-inflight-requests jobs are in flight. */
    void *samples[COMPRESSOR_SAMPLES_PER_CRON];
    unsigned long max_inflight = (unsigned long)server.compression_max_inflight_requests;
    unsigned int sampled = 0;
    int queued = 0;
    while (sampled < COMPRESSOR_SAMPLES_PER_CRON && inflightCount() < max_inflight) {
        serverDb *db = pickDb(total);
        if (db == NULL) break;
        int didx = kvstoreGetFairRandomHashtableIndex(db->keys);
        unsigned int n =
            kvstoreHashtableSampleEntries(db->keys, didx, samples, COMPRESSOR_SAMPLES_PER_CRON - sampled);
        if (n == 0) break;
        sampled += n;
        for (unsigned int i = 0; i < n && inflightCount() < max_inflight; i++) {
            robj *o = samples[i];
            compressor_stats.counters.keys_checked++;
            if (!canCompress(o)) {
                countSkippedKey(errno);
                continue;
            }
            compressor_stats.counters.keys_eligible++;
            if (!queueJob(db->id, o)) {
                compressor_stats.counters.keys_skipped_already_queued++;
                continue;
            }
            compressor_stats.counters.values_queued++;
            queued++;
        }
    }
    return queued;
}

/* ------------------------ Installing results (beforeSleep) ------------------------ */

/* Installs the frame of job, when the key still holds the same bytes as the
 * job's copy and can still be compressed. Returns true when the frame was
 * installed. Otherwise returns false, sets errno, and the frame is dropped:
 *
 *   ERANGE  the value did not get small enough, or its size is now outside
 *           the configured range
 *   ENOENT  the key is gone
 *   ESTALE  the value changed while the job ran
 *   EALREADY, EINVAL, EBUSY, EAGAIN  see canCompress() */
static bool installJob(compressorJob *job) {
    if (job->frame == NULL) {
        compressor_stats.counters.values_compressed_and_dropped_low_saving++;
        errno = ERANGE;
        return false;
    }
    serverDb *db = server.db[job->dbid];
    robj *o = db ? dbFind(db, job->key) : NULL;
    if (o == NULL) {
        compressor_stats.counters.values_compressed_and_dropped_changed++;
        errno = ENOENT;
        return false;
    }
    if (!canCompress(o)) {
        compressor_stats.counters.values_compressed_and_dropped_now_skipped++;
        return false;
    }

    sds current = objectGetVal(o);
    size_t len = sdslen(current);
    if (len != sdslen(job->input) || memcmp(current, job->input, len) != 0) {
        compressor_stats.counters.values_compressed_and_dropped_changed++;
        errno = ESTALE;
        return false;
    }

    if (!compressorFrameSavesMemory(compressorFrameAllocSize(current), compressorFrameAllocSize(job->frame),
                                    COMPRESSOR_MIN_SAVINGS_PCT)) {
        compressor_stats.counters.values_compressed_and_dropped_low_saving++;
        errno = ERANGE;
        return false;
    }
    compressorSetCompressedValue(o, job->frame);
    job->frame = NULL;
    compressor_stats.counters.values_compressed++;
    return true;
}

int compressorBeforeSleep(void) {
    /* Copies made outside a command, for example by a module timer. */
    compressorReleasePlainCopies();

    if (inflightCount() == 0) return 0;
    /* Leave the results in the queue while a child process runs. See
     * compressorCron(). The compare at install catches any change. */
    if (hasActiveChildProcess()) return 0;
    fifo *done = mutexQueuePopAll(done_jobs, false);
    if (done == NULL) return 0;

    int installed = 0;
    compressorJob *job;
    while (fifoPop(done, (void **)&job)) {
        compressor_stats.counters.compression_time_us += job->compress_us;
        if (installJob(job)) installed++;
        freeJob(job);
    }
    fifoRelease(done);
    return installed;
}
