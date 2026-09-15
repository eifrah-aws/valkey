/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Building and releasing a compressor instance. The backends themselves live in
 * compressor_alg_lz4.c and compressor_alg_zstd.c, and this file is the only place
 * that knows which ids map to which function table. */

#include "compressor_alg.h"
#include "serverassert.h"
#include "zmalloc.h"

/* The function tables, one per compressor_alg_*.c file. Declared here and not in
 * compressor_alg.h, because this is the only file that needs them: everyone else
 * calls newCompressor(). A backend this build leaves out has no table, so its
 * file is not compiled and its case below must be removed too. */
extern const compressorApi compressorApiLz4;

/* Applies the operator's range, then asks the backend. This is the function
 * behind compressorInstance.max_output_size, so it is the same for every backend
 * and the range check exists once. */
static size_t compressorMaxOutputSize(const compressorInstance *instance, size_t input_len) {
    assert(instance != NULL);

    /* The range the operator asked for. 0 means no limit on that side. */
    if (input_len < instance->config.min_input_len) return 0;
    if (instance->config.max_input_len != 0 && input_len > instance->config.max_input_len) return 0;

    /* What the library itself can handle. */
    return instance->api->lib_max_output_size(input_len);
}

compressorInstance *newCompressor(compressorAlgId id, const compressorConfig *config) {
    compressorInstance *c = zcalloc(sizeof(*c));

    switch (id) {
    case COMPRESSOR_ALG_LZ4:
        c->api = &compressorApiLz4;
        c->dict_max_size = COMPRESSOR_LZ4_DICT_MAX;
        break;
    case COMPRESSOR_ALG_ZSTD:
        /* zstd is not vendored yet, so there is no function table to point at. */
        zfree(c);
        return NULL;
    case COMPRESSOR_ALG_NONE:
    default:
        zfree(c);
        return NULL;
    }

    c->id = id;
    c->name = compressorAlgIdName(id);
    c->max_output_size = compressorMaxOutputSize;
    if (config != NULL) c->config = *config;

    if (c->api->state_new(c) != C_OK) {
        zfree(c);
        return NULL;
    }
    return c;
}

void freeCompressor(compressorInstance *c) {
    if (c == NULL) return;
    c->api->state_free(c);
    zfree(c);
}
