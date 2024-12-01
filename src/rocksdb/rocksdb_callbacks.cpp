#include <sstream>
#include <cstdlib>
#include <string>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include <iostream>

extern "C" void send_reply_cstring(void *, const char *);
extern "C" void send_reply_error(void *, const char *);
extern "C" void send_reply_ok(void *);
extern "C" void send_reply_nil(void *);

namespace {
/// Placing the database in /dev/shm enhances performance
thread_local std::string DB_PATH = "/dev/shm/redis-on-rocks";

/// Global write options
thread_local rocksdb::WriteOptions write_opts;

/// The database - static variable as we can safely share it among threads
static rocksdb::DB *pdb = nullptr;

/// Use pinnable slice to avoid the memory copy of the value
thread_local rocksdb::PinnableSlice pinnable_val;

/// Helper pinnable guard to ensure that the pinnable is always reset before usage,
/// and is released when leaving the scope
struct PinnableGuard {
    rocksdb::PinnableSlice &m_value;
    explicit inline PinnableGuard(rocksdb::PinnableSlice &v)
        : m_value(v) {
        m_value.Reset();
    }
    inline ~PinnableGuard() {
        m_value.Reset();
    }
};
} // namespace

/// Implement "SET" based on database
extern "C" void rocksdb_set(void *clnt, const char *argv[], const int argc) {
    auto key = rocksdb::Slice(argv[1]);
    auto value = rocksdb::Slice(argv[2]);

    auto status = pdb->Put(write_opts, pdb->DefaultColumnFamily(), key, value);
    if (status.ok()) {
        send_reply_ok(clnt);
    } else {
        std::stringstream ss;
        ss << "-ROCKSDB failed to put record in the database. " << status.ToString();
        send_reply_error(clnt, ss.str().c_str());
    }
}

/// Implement "SET" based on database
extern "C" void rocksdb_get(void *clnt, const char *argv[], const int argc) {
    auto key = rocksdb::Slice(argv[1]);
    rocksdb::ReadOptions opts;
    PinnableGuard guard{pinnable_val};
    auto status = pdb->Get(opts, pdb->DefaultColumnFamily(), key, &pinnable_val);
    switch (status.code()) {
    case rocksdb::Status::kNotFound:
        send_reply_nil(clnt);
        break;
    case rocksdb::Status::kOk:
        send_reply_cstring(clnt, pinnable_val.data());
        break;
    default: {
        std::stringstream ss;
        ss << "-ROCKSDB failed to get record from the database. " << status.ToString();
        send_reply_error(clnt, ss.str().c_str());
    } break;
    }
}

/// Initialise the database options and open it
extern "C" void rocksdb_initialise(void) {
    if (pdb) {
        // Already opened
        std::cerr << "rocksdb_initialise(): database is already initialised. Ignoring call" << std::endl;
        return;
    }

    rocksdb::Options options;
    options.create_if_missing = true;
    options.max_background_jobs = 4;
    options.compression = rocksdb::CompressionType::kSnappyCompression;

    // Initialise global write options
    write_opts.sync = false;

    // If we are interested in persistency, we can change this into "false" and use manual flushing of the WAL
    write_opts.disableWAL = true;
    rocksdb::Status s = rocksdb::DB::Open(options, DB_PATH, &pdb);
    if (!s.ok()) {
        // Abort
        std::cerr << "Failed to open database. " << s.ToString() << std::endl;
        std::abort();
    }

    // TODO: launch thread for performing background WAL files
}

/// Shutdown the database
extern "C" void rocksdb_shutdown(void) {
    if (!pdb) {
        return;
    }
    // TODO:
    // - Force flush the WAL (sync=true)
    // - Shutdown the WAL flush thread
    // - Close the database
}
