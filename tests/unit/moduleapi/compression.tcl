# A module read must not change a compressed value. A module write handle
# decompresses it in place.

source tests/support/compression.tcl

set testmodule [file normalize tests/modules/compression.so]

proc module_compression_value {} {
    return [string repeat {{"user_id":3,"role":"member"},} 40]
}

start_server {tags {"modules"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    r module load $testmodule

    test {A module read handle leaves the value compressed} {
        r set k1 [module_compression_value]
        compress_key r k1
        assert_equal [module_compression_value] [r compression.read k1]
        assert_equal compressed [r object encoding k1]
    }

    test {A module read handle leaves the value compressed during a child process} {
        r set k2 [module_compression_value]
        compress_key r k2
        for {set i 0} {$i < 10} {incr i} { r set filler:$i 1 }
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }
        assert_equal [module_compression_value] [r compression.read k2]
        assert_equal compressed [r object encoding k2]
        r config set rdb-key-save-delay 0
        waitForBgsave r
    }

    test {A module write handle decompresses the value in place} {
        r set k3 [module_compression_value]
        compress_key r k3
        assert_equal [module_compression_value] [r compression.readwrite k3]
        assert_equal raw [r object encoding k3]
    }

    test {A module read of a missing key replies null} {
        assert_equal {} [r compression.read nosuchkey]
    }
}
