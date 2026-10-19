/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Checks for the inline compression settings. See compressor_config.h. */

#include "compressor/compressor_config.h"
#include "server.h"

int compressorConfigCheck(const char **err) {
    if (server.compression_min_value_size > server.compression_max_value_size) {
        *err = "compression-min-value-size can't be greater than compression-max-value-size";
        return 0;
    }
    return 1;
}
