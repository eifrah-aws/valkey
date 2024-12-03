#include <sstream>
#include <cstdlib>
#include <string>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include <iostream>
#include <thread>

extern "C" void send_reply_cstring(void *, const char *);
extern "C" void send_reply_error(void *, const char *);
extern "C" void send_reply_ok(void *);
extern "C" void send_reply_nil(void *);
extern "C" void log_message(const char *);
extern "C" int rocksdb_enabled(void);

namespace {
/// Placing the database in /dev/shm enhances performance
static std::string DB_PATH = "/dev/shm/valkey-on-rocks";

/// Global write options
static rocksdb::WriteOptions write_opts;

/// The database - static variable as we can safely share it among threads
static rocksdb::DB *pdb = nullptr;

/// Use pinnable slice to avoid the memory copy of the value
static rocksdb::PinnableSlice pinnable_val;

/// Worker thread responsible for flushing the WAL file
static std::thread *wal_flush_thread = nullptr;

static std::atomic_bool shutdown = false;

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

using namespace std::chrono_literals;
void wal_flush_callback(rocksdb::DB *database) {
    log_message("WAL flush thread started");
    while (!shutdown.load()) {
        pdb->FlushWAL(false);
        std::this_thread::sleep_for(100ms);
    }
    log_message("WAL flush thread exited");
}

/// Read the database path from the environment variables
std::optional<std::string> get_database_path() {
    const char *path = ::getenv("ROCKSDB_PATH");
    if (path) {
        return std::string(path);
    }
    return {};
}
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
    if (!rocksdb_enabled()) {
        log_message("rocksdb_initialise(): RocksDB: is not enabled");
        return;
    }

    if (pdb) {
        // Already opened
        log_message("rocksdb_initialise(): database is already initialised. Ignoring call");
        return;
    }

    rocksdb::Options options;
    options.IncreaseParallelism(4);
    options.OptimizeLevelStyleCompaction(64 * 1024 * 1024);
    options.create_if_missing = true;
    options.compression = rocksdb::CompressionType::kNoCompression;
    options.manual_wal_flush = true;

    // Initialise global write options
    write_opts.sync = false;

    // If we are interested in persistency, we can change this into "false" and use manual flushing of the WAL
    write_opts.disableWAL = false;

    const auto dbpath = get_database_path().value_or(DB_PATH);
    rocksdb::Status s = rocksdb::DB::Open(options, dbpath, &pdb);
    if (!s.ok()) {
        // Abort
        std::stringstream ss;
        ss << "Failed to open database. " << s.ToString();
        log_message(ss.str().c_str());
        std::abort();
    }

    // TODO: launch thread for performing background WAL files
    wal_flush_thread = new std::thread(wal_flush_callback, pdb);

    std::stringstream ss;
    ss << "RocksDB successfully initialised at: " << dbpath;
    log_message(ss.str().c_str());
}

/// Shutdown the database
extern "C" void rocksdb_shutdown(void) {
    if (!rocksdb_enabled()) {
        log_message("rocksdb_shutdown(): RocksDB: is not enabled");
        return;
    }

    if (!pdb) {
        return;
    }

    log_message("RocksDB shutdown started...");
    shutdown.store(true);
    pdb->FlushWAL(true);
    wal_flush_thread->join();
    delete wal_flush_thread;
    wal_flush_thread = nullptr;

    pdb->Close();
    delete pdb;
    pdb = nullptr;

    log_message("RocksDB shutdown started...done");
}
