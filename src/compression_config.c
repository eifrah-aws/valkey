/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Checks for the inline compression settings. See compression_config.h. */

#include "fmacros.h" /* must come before any system header */
#include "compression_config.h"
#include "server.h"

int compressionConfigCheck(const char **err) {
    if (server.compression_min_value_size > server.compression_max_value_size) {
        *err = "compression-min-value-size can't be greater than compression-max-value-size";
        return 0;
    }
    return 1;
}

const char *compressionModeName(int mode) {
    if (mode == ALGO_NONE) return "off";
    return compressionAlgoName((compressionAlgo)mode);
}
