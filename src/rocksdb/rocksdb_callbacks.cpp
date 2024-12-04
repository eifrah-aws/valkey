#include <sstream>
#include <cstdlib>
#include <string>
#include "lmdb.hpp"

extern "C" void send_reply_cstring(void *, const char *, size_t);
extern "C" void send_reply_error(void *, const char *);
extern "C" void send_reply_ok(void *);
extern "C" void send_reply_nil(void *);
extern "C" void log_message(const char *);
extern "C" int LMDB_enabled(void);

namespace {
/// Placing the database in /dev/shm enhances performance
static std::string DB_PATH = "/tmp/valkey-on-lmdb";

static lmdb::DB db;

/// Read the database path from the environment variables
std::optional<std::string> read_env_var(std::string_view name) {
    const char *path = ::getenv(name.data());
    if (path) {
        return std::string(path);
    }
    return {};
}
} // namespace

/// Implement "SET" based on database
extern "C" void lmdb_set(void *clnt, const char *argv[], const int argc) {
    auto key = std::string_view(argv[1]);
    auto value = std::string_view(argv[2]);

    if (db.put(key, value)) {
        send_reply_ok(clnt);
    } else {
        std::stringstream ss;
        ss << "-LMDB failed to put record in the database. " << db.last_error();
        send_reply_error(clnt, ss.str().c_str());
        std::abort();
    }
}

/// Implement "SET" based on database
extern "C" void lmdb_get(void *clnt, const char *argv[], const int argc) {
    auto key = std::string_view(argv[1]);
    auto d = db.get(key);
    if (d.has_value()) {
        auto &value = d.value();
        send_reply_cstring(clnt, value.data(), value.length());
    } else {
        send_reply_nil(clnt);
    }
}

/// Initialise the database options and open it
extern "C" void lmdb_initialise(void) {
    if (!LMDB_enabled()) {
        log_message("lmdb_initialise(): LMDB: is not enabled");
        return;
    }

    if (db.is_open()) {
        // Already opened
        log_message("lmdb_initialise(): database is already initialised. Ignoring call");
        return;
    }

    const auto dbpath = read_env_var("DB_PATH").value_or(DB_PATH);
    db.open(dbpath);

    if (!db.is_open()) {
        // Abort
        std::stringstream ss;
        ss << "Failed to open database. " << db.last_error();
        log_message(ss.str().c_str());
        std::abort();
    }

    std::stringstream ss;
    ss << "LMDB successfully initialised at: " << dbpath;
    log_message(ss.str().c_str());
}

/// Shutdown the database
extern "C" void lmdb_shutdown(void) {
    if (!LMDB_enabled()) {
        log_message("lmdb_shutdown(): LMDB: is not enabled");
        return;
    }

    db.close();
    log_message("LMDB shutdown started...done");
}
