/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Checks for the inline compression settings. See compressor_config.h. */

#include "compressor/compressor_config.h"
#include "server.h"

int compressorConfigCheck(const char **err) {
    /* Forkless save reads keys on another thread, and a read can decompress a
     * value in place. Not supported yet. */
    if (server.compression_mode != COMPRESSOR_ALG_NONE && server.forkless_infrastructure_enabled) {
        *err = "compression-mode can't be used with forkless-infrastructure-enabled yes";
        return 0;
    }
    if (server.compression_min_value_size > server.compression_max_value_size) {
        *err = "compression-min-value-size can't be greater than compression-max-value-size";
        return 0;
    }
    return 1;
}
