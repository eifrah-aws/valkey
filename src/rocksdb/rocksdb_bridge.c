#include "../server.h"
#include <stdlib.h>

/// Process a "SET" command against RocksDB
void lmdb_set(void *, const char *[], const int);

/// Process a "GET" command against RocksDB
void lmdb_get(void *, const char *[], const int);

/// Open the database and set default configurations
void lmdb_initialise(void);

/// Route the command "SET" to the RocksDB variant
void lmdb_set_callback(client *c) {
    const char *argv[10]; // Maximum of 10 arguments

    for (int i = 0; i < c->argc; ++i) {
        argv[i] = c->argv[i]->ptr;
    }
    lmdb_set(c, argv, c->argc);
}

/// Route the command "SET" to the RocksDB variant
void lmdb_get_callback(client *c) {
    const char *argv[10]; // Maximum of 10 arguments

    for (int i = 0; i < c->argc; ++i) {
        argv[i] = c->argv[i]->ptr;
    }
    lmdb_get(c, argv, c->argc);
}


/// Interface for replies

void send_reply_ok(void *c) {
    addReplyStatus((client *)c, "OK");
}

void send_reply_nil(void *c) {
    addReplyNull((client *)c);
}

void send_reply_cstring(void *c, const char *msg, size_t len) {
    addReplyBulkCBuffer((client *)c, (const void *)msg, len);
}

void send_reply_error(void *c, const char *errmsg) {
    addReplyError((client *)c, errmsg);
}

void log_message(const char *message) {
    serverLog(LL_NOTICE,
              "%s", message);
}

/// Return 1 of rocksb support should be enabled, 0 otherwise
int LMDB_enabled(void) {
    // Check environment variable to see if RocksDB should be loaded
    const char *penv = getenv("DB_ENABLED");
    return penv && strcmp("1", penv) == 0;
}
