#include "valkeymodule.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <array>

#include "rocksdb/db.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"

namespace {

/// Function callback signature
typedef void (*CallbackFuncPtr)(ValkeyModuleClientPtr);

/// Default database location folder
static std::string DB_PATH = "valkey-on-rocks.db";

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

struct CommandContext {
    void *orig_proc = nullptr;
    size_t calls = 0;
};

/// Fixed array that holds pointers to a command context. A function may access this context during execution to
/// collect data
constexpr size_t CONTEXT_SIZE = 500;
static CommandContext *CONTEXT_ARR[CONTEXT_SIZE];
static size_t next_context_idx = 0;

using namespace std::chrono_literals;

#define VALKEY_INSTALL_COMMAND_CONTEXT()                                      \
    /* Cast the pointer back to the wrapper struct */                         \
    static CommandContext *context = nullptr;                                 \
    if (context == nullptr) {                                                 \
        CommandContext *wrapper = reinterpret_cast<CommandContext *>(client); \
        context = wrapper;                                                    \
        return;                                                               \
    }

/// The WAL worker thread. Periodically flush the WAL content to disk
/// in a constant intervals of 100ms. This guarantees that in case of crash
/// we will lose the data of the last 100ms (maximum)
void wal_flush_callback(rocksdb::DB *database) {
    while (!shutdown.load()) {
        pdb->FlushWAL(false);
        std::this_thread::sleep_for(100ms);
    }
}

size_t AllocateContextIndex() {
    if (next_context_idx >= CONTEXT_SIZE) {
        return (size_t)-1;
    }
    return next_context_idx++;
}

/// Helper method that replaces Valkey command represented by "name"
/// by a new pointer `funcptr`. In addition, this function initialises the function context structure.
/// This is done by calling the new callback function (`funcptr`) with the context object.
void InstallCallback(std::string_view name, void *funcptr) {
    size_t context_index = AllocateContextIndex();
    auto context = new CommandContext();
    CONTEXT_ARR[context_index] = context;
    auto orig_func = ValkeyModule_ReplaceCommand(name.data(), funcptr);
    context->orig_proc = orig_func;

    // Call the **new** method once - with the context, this will be stored in the callback
    // thread-local static storage and can be used in later calls
    ((CallbackFuncPtr)funcptr)(context);
}

/// Logging API
void LOG(ValkeyModuleCtx *ctx, const std::stringstream &ss) {
    ValkeyModule_Log(ctx, VALKEYMODULE_LOGLEVEL_NOTICE, "%s", ss.str().c_str());
}

void LOG(ValkeyModuleCtx *ctx, const char *msg) {
    ValkeyModule_Log(ctx, VALKEYMODULE_LOGLEVEL_NOTICE, "%s", msg);
}

/// Initialise the database options and open it
void rocksdb_initialise(ValkeyModuleCtx *ctx, bool enable_wal, size_t block_cache_mb, bool direct_io, std::optional<std::string> dbpath) {
    rocksdb::Options options;

    // Block cache for caching pages from the disk
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = rocksdb::NewLRUCache(block_cache_mb * 1024 * 1024);
    auto factory = rocksdb::NewBlockBasedTableFactory(table_options);
    options.table_factory.reset(factory);

    // BF for better reading
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    options.create_if_missing = true;
    options.compression = rocksdb::CompressionType::kNoCompression;

    // Blobbing
    options.enable_blob_files = true;
    options.min_blob_size = 1024; // 1K and above should be stored separately

    // Direct I/O should affect writes only
    options.use_direct_io_for_flush_and_compaction = direct_io;

    // These parameters are suppose to reduce write-stalls
    options.write_buffer_size = 128 << 20; // 128MB per memtable
    options.max_write_buffer_number = 4;   // 4 memtables pending to be flush
    options.IncreaseParallelism(4);

    // Initialise global write options
    write_opts.sync = false;

    write_opts.disableWAL = !enable_wal;
    options.manual_wal_flush = enable_wal;

    const auto path = dbpath.value_or(DB_PATH);
    rocksdb::Status s = rocksdb::DB::Open(options, path, &pdb);
    if (!s.ok()) {
        // Abort
        std::stringstream ss;
        ss << "Failed to open database. " << s.ToString();
        LOG(ctx, ss);
        std::abort();
    }

    if (enable_wal) {
        LOG(ctx, "RocksDB WAL is used");
        wal_flush_thread = new std::thread(wal_flush_callback, pdb);
    }
    std::stringstream ss;
    ss << "RocksDB successfully initialised at: " << path;
    LOG(ctx, ss);
}

void rocksdb_shutdown(ValkeyModuleCtx *ctx) {
    if (!pdb) {
        return;
    }

    LOG(ctx, "RocksDB shutdown started...");
    shutdown.store(true);

    if (wal_flush_thread) {
        pdb->FlushWAL(true);
        wal_flush_thread->join();
        delete wal_flush_thread;
        wal_flush_thread = nullptr;
    }

    pdb->Close();
    delete pdb;
    pdb = nullptr;

    LOG(ctx, "RocksDB shut-down started...done");
}

} // namespace

extern "C" void on_command_get(ValkeyModuleClientPtr client) {
    VALKEY_INSTALL_COMMAND_CONTEXT();
    context->calls++;

    int count = 0;
    auto argv = ValkeyModule_GetClientCommandArgs(client, &count);

    size_t keylen = 0;
    const char *ckey = ValkeyModule_StringPtrLen((const ValkeyModuleString *)argv[1], &keylen);

    auto key = rocksdb::Slice(ckey, keylen);
    rocksdb::ReadOptions opts;
    PinnableGuard guard{pinnable_val};
    auto status = pdb->Get(opts, pdb->DefaultColumnFamily(), key, &pinnable_val);
    switch (status.code()) {
    case rocksdb::Status::kNotFound:
        ValkeyModule_SendReplyNull(client);
        break;
    case rocksdb::Status::kOk:
        ValkeyModule_SendReplyBulkCString(client, pinnable_val.data(), pinnable_val.size());
        break;
    default: {
        std::stringstream ss;
        ss << "-ROCKSDB failed to get record from the database. " << status.ToString();
        ValkeyModule_SendReplyError(client, ss.str().c_str());
    } break;
    }
}

extern "C" void on_command_set(ValkeyModuleClientPtr *client) {
    VALKEY_INSTALL_COMMAND_CONTEXT();

    // We could choose to call here to original callback
    // ((CallbackFuncPtr)context->orig_proc)(client);

    int count = 0;
    auto argv = ValkeyModule_GetClientCommandArgs(client, &count);

    size_t keylen = 0;
    const char *ckey = ValkeyModule_StringPtrLen((const ValkeyModuleString *)argv[1], &keylen);

    size_t vallen = 0;
    const char *cval = ValkeyModule_StringPtrLen((const ValkeyModuleString *)argv[2], &vallen);

    auto key = rocksdb::Slice(ckey, keylen);
    auto value = rocksdb::Slice(cval, vallen);

    auto status = pdb->Put(write_opts, pdb->DefaultColumnFamily(), key, value);
    if (status.ok()) {
        ValkeyModule_SendReplyOk(client);
    } else {
        std::stringstream ss;
        ss << "-ROCKSDB failed to put record in the database. " << status.ToString();
        ValkeyModule_SendReplyError(client, ss.str().c_str());
    }
}

extern "C" int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    rocksdb_shutdown(ctx);
    LOG(ctx, "RocksDB module shutdown completed");
    return VALKEYMODULE_OK;
}

extern "C" int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, "demo_module", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    // Log the list of parameters passing loading the module.
    std::stringstream ss;
    bool with_wal = false;
    std::optional<std::string> dbpath;
    std::optional<size_t> block_cache_mb;
    bool direct_io = false;
    for (int j = 0; j < argc; j++) {
        std::string_view arg{ValkeyModule_StringPtrLen(argv[j], NULL)};
        if (arg == "--with-wal") {
            with_wal = true;
        } else if (arg == "--db-path") {
            ++j;
            dbpath = ValkeyModule_StringPtrLen(argv[j], NULL);
        } else if (arg == "--block-cache") {
            ++j;
            const char *n = ValkeyModule_StringPtrLen(argv[j], NULL);
            block_cache_mb = std::atol(n);
        } else if (arg == "--direct-io") {
            direct_io = true;
        }
        ss << arg << " ";
    }

    // If not provided, use 64mb of block cache
    ss = {};
    ss << "RocksDB cache: " << block_cache_mb.value_or(64) << "mb";
    LOG(ctx, ss);

    ss = {};
    ss << "WAL enabled: " << with_wal;
    LOG(ctx, ss);

    ss = {};
    ss << "Using direct I/O: " << direct_io;
    LOG(ctx, ss);

    rocksdb_initialise(ctx, with_wal, block_cache_mb.value_or(64), direct_io, dbpath);

    // Override methods in Valkey with our own variant
    InstallCallback("set", (void *)on_command_set);
    InstallCallback("get", (void *)on_command_get);

    LOG(ctx, "RocksDB module loaded");
    return VALKEYMODULE_OK;
}
