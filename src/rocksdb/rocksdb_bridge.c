#include "../server.h"

/// Process a "SET" command against RocksDB
void rocksdb_set(void *, const char *[], const int);

/// Process a "GET" command against RocksDB
void rocksdb_get(void *, const char *[], const int);

/// Open the database and set default configurations
void rocksdb_initialise(void);

/// Route the command "SET" to the RocksDB variant
void rocksdb_set_callback(client *c) {
    const char *argv[10]; // Maximum of 10 arguments

    for (int i = 0; i < c->argc; ++i) {
        argv[i] = c->argv[i]->ptr;
    }
    rocksdb_set(c, argv, c->argc);
}

/// Route the command "SET" to the RocksDB variant
void rocksdb_get_callback(client *c) {
    const char *argv[10]; // Maximum of 10 arguments

    for (int i = 0; i < c->argc; ++i) {
        argv[i] = c->argv[i]->ptr;
    }
    rocksdb_get(c, argv, c->argc);
}


/// Interface for replies

void send_reply_ok(void *c) {
    addReplyStatus((client *)c, "OK");
}

void send_reply_nil(void *c) {
    addReplyNull((client *)c);
}

void send_reply_cstring(void *c, const char *msg) {
    addReplyBulkCString((client *)c, msg);
}

void send_reply_error(void *c, const char *errmsg) {
    addReplyError((client *)c, errmsg);
}
