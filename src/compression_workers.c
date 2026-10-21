/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Background compression of cold string values. See compression_workers.h. */

#include "server.h"
#include "compression_workers.h"
#include "compression_config.h"
#include "compression_value.h"
#include "compression_object.h"
#include "compression_stats.h"
#include "lrulfu.h"
#include "fifo.h"
#include "mutexqueue.h"
#include "mt19937-64.h"
#include "monotonic.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>

/* One value to compress. The main thread makes it, a worker fills in frame,
 * and the main thread installs or drops it. */
typedef struct compressionJob {
    int dbid;
    sds key;
    sds inflight_name;     /* The job's name in inflight_keys. Owned by that table. */
    sds input;             /* Own copy of the plain value. Kept until install, for the compare. */
    sds frame;             /* Set by the worker. NULL when the value does not get smaller. */
    long long compress_us; /* Set by the worker: time spent compressing. */
    bool incoming;         /* An incoming key: no coldness test at install. */
} compressionJob;

static mutexQueue *pending_jobs; /* Main thread to workers. NULL when compression is off. */
static mutexQueue *done_jobs;    /* Workers to main thread. */

/* The keys of the jobs that are queued, running, or done but not installed
 * yet, so a key is never in two jobs at once. Main thread only. */
static hashtable *inflight_keys;

/* The incoming keys (see compression_workers.h), by inflightName(). The table
 * owns the names and keeps a key there once; incoming_order holds the same
 * names in arrival order. Main thread only. */
static hashtable *incoming_keys;
static fifo *incoming_order;

static pthread_t worker_threads[COMPRESSION_THREADS_MAX];
static int num_workers;

/* The name of a key in inflight_keys: the db id, then the key bytes. */
static sds inflightName(int dbid, const_sds key) {
    sds name = sdsnewlen(&dbid, sizeof(dbid));
    return sdscatlen(name, key, sdslen(key));
}

static unsigned long inflightCount(void) {
    return inflight_keys ? hashtableSize(inflight_keys) : 0;
}

unsigned long compressionJobsInFlight(void) {
    return inflightCount();
}

static void freeJob(compressionJob *job) {
    /* The table owns inflight_name and frees it. */
    hashtableDelete(inflight_keys, job->inflight_name);
    sdsfree(job->key);
    sdsfree(job->input);
    sdsfree(job->frame);
    zfree(job);
}

/* ----------------------------- Worker threads ----------------------------- */

static void *compressionWorkerMain(void *arg) {
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
    blockCompressor *c = blockCompressorCreate(server.compression_mode, NULL);
    serverAssert(c != NULL);

    while (1) {
        compressionJob *job = mutexQueuePop(pending_jobs, true);
        /* For tests: keeps the job in flight longer. */
        if (server.compression_debug_job_delay) debugDelay(server.compression_debug_job_delay);
        monotime start = getMonotonicUs();
        job->frame = compressionValueBuild(c, OBJ_ENCODING_RAW, job->input, sdslen(job->input), NULL);
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

int compressionWorkersInit(void) {
    if (server.compression_mode == ALGO_NONE) return 0;

    pending_jobs = mutexQueueCreate();
    done_jobs = mutexQueueCreate();
    inflight_keys = hashtableCreate(&setHashtableType);
    incoming_keys = hashtableCreate(&setHashtableType);
    incoming_order = fifoCreate();

    pthread_attr_t attr;
    serverInitThreadAttribute(&attr);
    for (int i = 0; i < server.compression_threads; i++) {
        int err = pthread_create(&worker_threads[i], &attr, compressionWorkerMain, NULL);
        if (err) {
            serverLog(LL_WARNING, "Fatal: Can't start compressor threads. Error message: %s", strerror(err));
            exit(1);
        }
        num_workers++;
    }
    pthread_attr_destroy(&attr);
    return num_workers;
}

int compressionWorkersKill(void) {
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
        return freq <= server.compression_lfu_threshold;
    }
    return lru_getIdleSecs(objectGetLRU(o)) >= COMPRESSION_MIN_IDLE_SECONDS;
}

/* Can the value o be compressed, if it is cold? Returns false and sets errno
 * when it can't:
 *
 *   EALREADY  already compressed
 *   EINVAL  not a plain (RAW) string
 *   EBUSY   someone else holds the object, and may read the plain bytes
 *   ERANGE  the size is outside the configured range */
static bool canCompressValue(robj *o) {
    if (compressionIsCompressedString(o)) {
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
    return true;
}

/* Can the value o be compressed now? Used when a value is picked and again
 * when its frame is installed, because the value or the settings can change
 * in between. Returns false and sets errno when it can't: see
 * canCompressValue(), and EAGAIN when the value is not cold. */
static bool canCompress(robj *o) {
    if (!canCompressValue(o)) return false;
    if (!isCold(o)) {
        errno = EAGAIN;
        return false;
    }
    return true;
}

/* Counts a key that canCompress() refused, by the errno it set. */
static void countSkippedKey(int err) {
    switch (err) {
    case EALREADY: compression_stats.counters.compression_total_keys_skipped_already_compressed++; break;
    case EINVAL: compression_stats.counters.compression_total_keys_skipped_not_string++; break;
    case EBUSY: compression_stats.counters.compression_total_keys_skipped_in_use++; break;
    case ERANGE: compression_stats.counters.compression_total_keys_skipped_size++; break;
    case EAGAIN: compression_stats.counters.compression_total_keys_skipped_hot++; break;
    default: break;
    }
}

/* Queues a job for the value o of a key in db dbid. Returns false and sets
 * errno to EEXIST when the key is already in a job. */
static bool queueJob(int dbid, robj *o, bool incoming) {
    sds name = inflightName(dbid, objectGetKey(o));
    if (!hashtableAdd(inflight_keys, name)) {
        sdsfree(name);
        errno = EEXIST;
        return false;
    }

    compressionJob *job = zmalloc(sizeof(*job));
    job->dbid = dbid;
    job->key = sdsdup(objectGetKey(o));
    job->inflight_name = name;
    job->input = sdsdup(objectGetVal(o));
    job->frame = NULL;
    job->compress_us = 0;
    job->incoming = incoming;
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

/* Checks the value o of a key in db dbid, and queues a job when it can be
 * compressed. Returns true when a job was queued. Otherwise returns false,
 * and errno tells why (see canCompress() and queueJob()). */
static bool checkKey(int dbid, robj *o) {
    compression_stats.counters.compression_total_keys_checked++;
    if (!canCompress(o)) {
        countSkippedKey(errno);
        return false;
    }
    compression_stats.counters.compression_total_keys_eligible++;
    if (!queueJob(dbid, o, false)) {
        compression_stats.counters.compression_total_keys_skipped_already_queued++;
        return false;
    }
    compression_stats.counters.compression_total_jobs_queued++;
    return true;
}

static bool inflightIsFull(void) {
    return inflightCount() >= (unsigned long)server.compression_max_inflight_requests;
}

/* The random policy. total is the number of keys in all databases, and must
 * not be 0. Returns the number of jobs queued. */
static int sweepRandom(unsigned long long total) {
    void *samples[COMPRESSION_SWEEPER_SAMPLE_BATCH];
    unsigned int limit = (unsigned int)server.compression_sweeper_samples;
    unsigned int sampled = 0;
    int queued = 0;
    while (sampled < limit && !inflightIsFull()) {
        serverDb *db = pickDb(total);
        if (db == NULL) break;
        int didx = kvstoreGetFairRandomHashtableIndex(db->keys);
        unsigned int want = limit - sampled;
        if (want > COMPRESSION_SWEEPER_SAMPLE_BATCH) want = COMPRESSION_SWEEPER_SAMPLE_BATCH;
        unsigned int n = kvstoreHashtableSampleEntries(db->keys, didx, samples, want);
        if (n == 0) break;
        sampled += n;
        for (unsigned int i = 0; i < n && !inflightIsFull(); i++) {
            if (checkKey(db->id, samples[i])) queued++;
        }
    }
    return queued;
}

/* Where the cursor policy is: the database it scans, the kvstoreScan()
 * cursor in that database, and the number of keys of the bucket at the cursor
 * that an earlier call already handled. They are kept from one cron call to
 * the next. */
static int sweep_dbid = 0;
static unsigned long long sweep_cursor = 0;
static unsigned int sweep_bucket_done = 0;

/* The backoff of the cursor policy: the keys checked and the keys eligible in
 * this pass, whether the pass covers the whole keyspace, and the end of the
 * pause (monotonic us, 0 when not paused). */
static unsigned long long sweep_pass_checked = 0;
static unsigned long long sweep_pass_eligible = 0;
static bool sweep_pass_full = true;
static monotime sweep_paused_until = 0;

typedef struct sweepScanData {
    unsigned int limit; /* compression-sweeper-samples, read once per call */
    int dbid;
    unsigned int checked;
    int queued;
    unsigned int skip; /* Keys of this bucket handled by an earlier call. */
    unsigned int seen; /* Keys of this bucket given to the callback. */
    bool stopped;      /* The callback stopped before the end of the bucket. */
} sweepScanData;

/* The scan gives the callback all keys of a bucket, with its chain, and can
 * not stop in the middle. So the callback skips the keys that an earlier call
 * handled, and stops checking at the key limit or when the in-flight limit is
 * full. One cron call therefore checks at most compression-sweeper-samples
 * keys. For the other keys, the callback only compares two counters. */
static void sweepScanCallback(void *privdata, void *entry, int didx) {
    UNUSED(didx);
    sweepScanData *data = privdata;
    if (data->seen++ < data->skip || data->stopped) return;
    if (data->checked >= data->limit || inflightIsFull()) {
        data->stopped = true;
        sweep_bucket_done = data->seen - 1;
        return;
    }
    data->checked++;
    sweep_pass_checked++;
    bool queued = checkKey(data->dbid, entry);
    /* EEXIST: eligible, but already in a job. */
    if (queued || errno == EEXIST) sweep_pass_eligible++;
    if (queued) data->queued++;
}

/* Ends a pass. Starts the backoff pause when almost no key in the pass was
 * eligible. A pass that checked no key does not pause: during a slot import,
 * the scan skips the importing slots, and the keys must be compressed soon
 * after the import. Such a pass is cheap. */
static void sweepPassDone(void) {
    compression_stats.counters.compression_total_sweeper_passes++;
    bool idle = sweep_pass_full && sweep_pass_checked > 0 &&
                sweep_pass_eligible * 100 < sweep_pass_checked * COMPRESSION_BACKOFF_MIN_ELIGIBLE_PCT;
    if (idle && server.compression_sweeper_backoff_seconds > 0) {
        sweep_paused_until = getMonotonicUs() + (monotime)server.compression_sweeper_backoff_seconds * 1000000;
        compression_stats.counters.compression_total_sweeper_backoffs++;
    }
    sweep_pass_checked = 0;
    sweep_pass_eligible = 0;
    sweep_pass_full = true;
}

bool compressionSweeperIsPaused(void) {
    return server.compression_sweeper_policy == COMPRESSION_SWEEPER_POLICY_CURSOR && sweep_paused_until != 0 &&
           getMonotonicUs() < sweep_paused_until;
}

int compressionSweeperBackoffApply(const char **err) {
    UNUSED(err);
    sweep_paused_until = 0;
    /* A pass that is in progress does not start a pause when it ends. It
     * already scanned a part of the keyspace before the wake, and a key that
     * is new there is checked only in the next pass. At the start of a pass
     * (for example during a pause, which starts when a pass ends) the next
     * pass covers the whole keyspace, so it stays a full pass. */
    bool at_pass_start = sweep_dbid == 0 && sweep_cursor == 0 && sweep_bucket_done == 0;
    sweep_pass_checked = 0;
    sweep_pass_eligible = 0;
    if (!at_pass_start) sweep_pass_full = false;
    return 1;
}

/* Moves the cursor policy to the start of the next database. After the last
 * database, a pass is done, and the next pass starts at database 0. */
static void sweepNextDb(void) {
    sweep_cursor = 0;
    sweep_bucket_done = 0;
    if (++sweep_dbid < server.dbnum) return;
    sweep_dbid = 0;
    sweepPassDone();
}

/* The cursor policy. Each step scans one bucket. kvstoreScan() handles a
 * rehash, FLUSHALL and SWAPDB: at worst a key is checked two times, or waits
 * for the next pass. Returns the number of jobs queued. */
static int sweepCursor(void) {
    if (compressionSweeperIsPaused()) return 0;
    /* The pause is over, or there was none. sweepPassDone() can start a new
     * one in this call, and then the loop stops. */
    sweep_paused_until = 0;
    sweepScanData data = {0};
    data.limit = (unsigned int)server.compression_sweeper_samples;
    for (unsigned int steps = 0; steps < data.limit && data.checked < data.limit &&
                                 !inflightIsFull() && sweep_paused_until == 0;
         steps++) {
        serverDb *db = sweep_dbid < server.dbnum ? server.db[sweep_dbid] : NULL;
        if (db == NULL || kvstoreSize(db->keys) == 0) {
            sweepNextDb();
            continue;
        }
        data.dbid = db->id;
        data.skip = sweep_bucket_done;
        data.seen = 0;
        unsigned long long next = kvstoreScan(db->keys, sweep_cursor, -1, -1, sweepScanCallback, NULL, &data);
        /* When the callback stopped in the middle of the bucket, keep the
         * cursor. The next call scans this bucket again and starts after the
         * keys handled now. The keys come in the same order when the bucket
         * did not change. If it changed, a key may be checked two times, or
         * wait for the next pass. Each step handles at least one new key: the
         * loop checks both limits before the step, and only this step can
         * fill them. */
        if (data.stopped) break;
        sweep_cursor = next;
        sweep_bucket_done = 0;
        if (sweep_cursor == 0) sweepNextDb();
    }
    return data.queued;
}

/* ------------------------------ Incoming keys ------------------------------ */

bool compressionKeyModified(client *c, serverDb *db, robj *key) {
    if (incoming_keys == NULL || c == NULL) return false;
    int type = getClientType(c);
    if (type != CLIENT_TYPE_PRIMARY && type != CLIENT_TYPE_SLOT_IMPORT) return false;
    sds name = inflightName(db->id, objectGetVal(key));
    if (!hashtableAdd(incoming_keys, name)) {
        sdsfree(name);
        return false;
    }
    fifoPush(incoming_order, name);
    return true;
}

unsigned long compressionIncomingPendingKeys(void) {
    return incoming_keys ? hashtableSize(incoming_keys) : 0;
}

/* Queues jobs for incoming keys, oldest first, until compression-sweeper-samples
 * keys are done or compression-max-inflight-requests jobs are in flight. A
 * key that is gone, or that can not be compressed (see canCompressValue()),
 * is dropped from the set.
 *
 * A key that already has a job (from the sweeper, or for an older value)
 * stays in the set. That job may be dropped at install, because the value
 * changed, so the newest value needs its own job. The key goes to the end of
 * the queue and is tried again in a later call, after the job is done. Each
 * key is tried once per call. Returns the number of jobs queued. */
static int queueIncomingKeys(void) {
    unsigned int budget = (unsigned int)server.compression_sweeper_samples;
    int queued = 0;
    fifo *again = NULL;
    sds name;
    while (budget > 0 && !inflightIsFull() && fifoPop(incoming_order, (void **)&name)) {
        budget--;
        int dbid;
        memcpy(&dbid, name, sizeof(dbid));
        sds key = sdsnewlen(name + sizeof(dbid), sdslen(name) - sizeof(dbid));
        serverDb *db = dbid < server.dbnum ? server.db[dbid] : NULL;
        robj *o = db ? dbFind(db, key) : NULL;
        bool keep = false;
        if (o != NULL && canCompressValue(o)) {
            if (queueJob(dbid, o, true)) {
                compression_stats.counters.compression_total_incoming_jobs_queued++;
                queued++;
            } else {
                keep = (errno == EEXIST);
            }
        }
        sdsfree(key);
        if (keep) {
            if (again == NULL) again = fifoCreate();
            fifoPush(again, name);
        } else {
            /* The table owns name and frees it. */
            hashtableDelete(incoming_keys, name);
        }
    }
    if (again != NULL) {
        while (fifoPop(again, (void **)&name)) fifoPush(incoming_order, name);
        fifoRelease(again);
    }
    return queued;
}

int compressionCron(void) {
    if (pending_jobs == NULL || server.loading) return 0;
    /* Installing a frame changes memory that a child process (BGSAVE,
     * BGREWRITEAOF) still shares, and makes the kernel copy those pages. */
    if (hasActiveChildProcess()) {
        compression_stats.counters.compression_total_sweeps_paused_by_child++;
        return 0;
    }

    /* Incoming keys first. The sweeper gets the room that is left. */
    int incoming = queueIncomingKeys();

    unsigned long long total = 0;
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] != NULL) total += kvstoreSize(server.db[i]->keys);
    }
    if (total == 0) return incoming;

    /* Both policies look at no more than compression-sweeper-samples keys per
     * call, and stop when compression-max-inflight-requests jobs are in
     * flight. */
    if (server.compression_sweeper_policy == COMPRESSION_SWEEPER_POLICY_CURSOR) return incoming + sweepCursor();
    return incoming + sweepRandom(total);
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
static bool installJob(compressionJob *job) {
    if (job->frame == NULL) {
        compression_stats.counters.compression_total_values_dropped_low_saving++;
        errno = ERANGE;
        return false;
    }
    serverDb *db = server.db[job->dbid];
    robj *o = db ? dbFind(db, job->key) : NULL;
    if (o == NULL) {
        compression_stats.counters.compression_total_values_dropped_changed++;
        errno = ENOENT;
        return false;
    }
    if (job->incoming ? !canCompressValue(o) : !canCompress(o)) {
        compression_stats.counters.compression_total_values_dropped_not_eligible++;
        return false;
    }

    sds current = objectGetVal(o);
    size_t len = sdslen(current);
    if (len != sdslen(job->input) || memcmp(current, job->input, len) != 0) {
        compression_stats.counters.compression_total_values_dropped_changed++;
        errno = ESTALE;
        return false;
    }

    if (!compressionValueSavesMemory(compressionValueAllocSize(current), compressionValueAllocSize(job->frame),
                                     COMPRESSION_MIN_SAVINGS_PCT)) {
        compression_stats.counters.compression_total_values_dropped_low_saving++;
        errno = ERANGE;
        return false;
    }
    compressionSetCompressedValue(o, job->frame);
    job->frame = NULL;
    compression_stats.counters.compression_total_values_compressed++;
    return true;
}

int compressionBeforeSleep(void) {
    /* Copies made outside a command, for example by a module timer. */
    compressionReleasePlainCopies();

    if (pending_jobs == NULL || server.loading) return 0;
    /* Leave the results and the incoming keys in their queues while a child
     * process runs. See compressionCron(). The compare at install catches any
     * change. */
    if (hasActiveChildProcess()) return 0;

    int installed = 0;
    fifo *done = inflightCount() ? mutexQueuePopAll(done_jobs, false) : NULL;
    if (done != NULL) {
        compressionJob *job;
        while (fifoPop(done, (void **)&job)) {
            compression_stats.counters.compression_total_compression_time_us += job->compress_us;
            if (installJob(job)) installed++;
            freeJob(job);
        }
        fifoRelease(done);
    }
    /* The installs made room in the in-flight limit. Incoming keys use it at
     * once, so an import is not limited to the 10 cron calls per second. */
    if (fifoLength(incoming_order)) queueIncomingKeys();
    return installed;
}

/* --------------------------- DEBUG COMPRESS-KEY --------------------------- */

/* Compresses the value of key in db now, on the main thread. It makes the
 * same checks as the sweeper, except the coldness test, and keeps the
 * net-savings guard. It does not change the sweeper counters, so their sums
 * stay right. It also runs during a child process: it is a test tool. Returns
 * false and sets errno when it can't: ENOTSUP when compression is off, ENOENT
 * when there is no such key, ERANGE when the value does not compress enough,
 * and the codes of canCompressValue(). */
static bool compressKeyNow(serverDb *db, robj *key) {
    if (pending_jobs == NULL) {
        errno = ENOTSUP;
        return false;
    }
    robj *o = dbFind(db, objectGetVal(key));
    if (o == NULL) {
        errno = ENOENT;
        return false;
    }
    if (!canCompressValue(o)) return false;

    sds plain = objectGetVal(o);
    blockCompressor *c = compressionGetForMainThread(server.compression_mode);
    sds frame = c ? compressionValueBuild(c, OBJ_ENCODING_RAW, plain, sdslen(plain), NULL) : NULL;
    if (frame == NULL || !compressionValueSavesMemory(compressionValueAllocSize(plain), compressionValueAllocSize(frame),
                                                      COMPRESSION_MIN_SAVINGS_PCT)) {
        sdsfree(frame);
        errno = ERANGE;
        return false;
    }
    compressionSetCompressedValue(o, frame);
    return true;
}

static const char *compressKeyError(int err) {
    switch (err) {
    case ENOTSUP: return "compression-mode is off";
    case ENOENT: return "no such key";
    case EALREADY: return "the value is already compressed";
    case EINVAL: return "the value is not a plain string";
    case EBUSY: return "the value is in use";
    case ERANGE: return "the value size is outside the configured range, or it does not compress enough";
    default: return "unknown error";
    }
}

bool compressionDebugCompressKey(client *c) {
    bool done = compressKeyNow(c->db, c->argv[2]);
    if (done) {
        addReply(c, shared.ok);
    } else {
        addReplyErrorFormat(c, "Can't compress the key: %s", compressKeyError(errno));
    }
    return done;
}
