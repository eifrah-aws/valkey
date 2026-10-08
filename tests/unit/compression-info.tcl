# Tests for INFO compression. See design-docs/inline-compression.md, 9.1.

source tests/support/compression.tcl

proc cinfo {field} {
    getInfoProperty [r info compression] $field
}

# A JSON-like value of about 1 KiB. It compresses well.
proc cinfo_value {{seed 0}} {
    set v ""
    for {set i 0} {$i < 25} {incr i} {
        append v "{\"user_id\":[expr {$seed + $i}],\"role\":\"member\",\"active\":true},"
    }
    return $v
}

# The counters must always add up, see 9.1. All fields come from one INFO
# reply, because the sweeper can run between two INFO calls.
proc cinfo_check_totals {} {
    set info [r info compression]
    foreach f {keys_checked keys_eligible keys_skipped_already_compressed keys_skipped_not_string
               keys_skipped_in_use keys_skipped_size keys_skipped_hot keys_skipped_already_queued
               values_queued values_compressed values_compressed_and_dropped
               values_compressed_and_dropped_low_saving values_compressed_and_dropped_changed
               values_compressed_and_dropped_now_skipped compression_queue_length} {
        set v($f) [getInfoProperty $info $f]
    }
    set skipped [expr {$v(keys_skipped_already_compressed) + $v(keys_skipped_not_string) +
                       $v(keys_skipped_in_use) + $v(keys_skipped_size) + $v(keys_skipped_hot)}]
    assert_equal $v(keys_checked) [expr {$v(keys_eligible) + $skipped}]
    assert_equal $v(keys_eligible) [expr {$v(values_queued) + $v(keys_skipped_already_queued)}]
    assert_equal $v(values_compressed_and_dropped) [expr {$v(values_compressed_and_dropped_low_saving) +
                                                          $v(values_compressed_and_dropped_changed) +
                                                          $v(values_compressed_and_dropped_now_skipped)}]
    assert_equal $v(values_queued) [expr {$v(values_compressed) + $v(values_compressed_and_dropped) +
                                          $v(compression_queue_length)}]
}

start_server {tags {"compression external:skip"}} {
    test {INFO compression is there when compression is off} {
        assert_equal off [cinfo compression_mode]
        assert_equal 0 [cinfo compressed_values]
        assert_equal 0 [cinfo keys_checked]
        # Not in the default INFO output.
        assert_equal {} [getInfoProperty [r info] compression_mode]
        assert_equal off [getInfoProperty [r info all] compression_mode]
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 compression-threads 2 maxmemory-policy allkeys-lfu}} {
    test {INFO compression shows the settings} {
        assert_equal lz4 [cinfo compression_mode]
        assert_equal 2 [cinfo compression_threads]
    }

    test {INFO compression counts compressed values and their sizes} {
        r flushall
        set total 0
        foreach k {a b c} {
            r set $k [cinfo_value]
            incr total [string length [cinfo_value]]
        }
        # Stored as raw (too long for embstr), but under the 256-byte minimum.
        r set small [string repeat x 250]
        assert_equal raw [r object encoding small]
        foreach k {a b c} { compress_key r $k }

        assert_equal 3 [cinfo compressed_values]
        assert_equal $total [cinfo compressed_values_original_bytes]
        set bytes [cinfo compressed_values_bytes]
        assert_lessthan $bytes $total
        assert_equal [expr {$total - $bytes}] [cinfo compression_saved_bytes]
        assert_morethan_equal [cinfo values_compressed] 3
        assert_morethan [cinfo keys_checked] 0
        assert_morethan [cinfo keys_skipped_size] 0
        # Compressed keys are sampled again, and counted on their own.
        wait_for_condition 100 50 {[cinfo keys_skipped_already_compressed] > 0} else {
            fail "compressed keys were not counted"
        }
        assert_morethan [cinfo compression_time_us] -1
        cinfo_check_totals
    }

    test {INFO compression counts reads that decompress} {
        set before [cinfo values_decompressed]
        assert_equal [cinfo_value] [r get a]
        assert_equal [expr {$before + 1}] [cinfo values_decompressed]
        assert_equal 2 [cinfo compressed_values]
        assert_morethan [cinfo decompression_time_us] -1
    }

    test {INFO compression updates the sizes when a value is deleted} {
        r del b c
        assert_equal 0 [cinfo compressed_values]
        assert_equal 0 [cinfo compressed_values_original_bytes]
        assert_equal 0 [cinfo compressed_values_bytes]
    }

    test {INFO compression counts values that do not compress} {
        r set random [randstring 2000 2000 binary]
        make_cold r random
        wait_for_condition 100 50 {[cinfo values_compressed_and_dropped_low_saving] > 0} else {
            fail "the random value was not dropped"
        }
        assert_equal raw [r object encoding random]
        cinfo_check_totals
    }

    test {INFO compression counts temporary copies during a save} {
        r flushall
        r set k [cinfo_value]
        compress_key r k
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        set made [cinfo temporary_copies_made]
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }
        r get k
        assert_equal [expr {$made + 1}] [cinfo temporary_copies_made]
        # The copy is freed when the command ends.
        assert_equal 0 [cinfo temporary_copies]
        assert_equal 1 [cinfo compressed_values]
        wait_for_condition 50 20 {[cinfo compression_paused_during_save] > 0} else { fail "the sweeper did not pause" }
        r config set rdb-key-save-delay 0
        waitForBgsave r
    }

    test {CONFIG RESETSTAT resets the counters but keeps the sizes} {
        assert_equal 1 [cinfo compressed_values]
        # The sweeper keeps running, so keys_checked can grow again right after
        # the reset. It is still far below the value before the reset.
        wait_for_condition 100 50 {[cinfo keys_checked] > 1000} else { fail "the sweeper did not run" }
        set before [cinfo keys_checked]
        r config resetstat
        assert_lessthan [cinfo keys_checked] $before
        assert_equal 0 [cinfo values_compressed]
        assert_equal 0 [cinfo temporary_copies_made]
        assert_equal 1 [cinfo compressed_values]
        assert_morethan [cinfo compressed_values_bytes] 0
    }

    test {INFO compression sizes go to 0 after FLUSHALL ASYNC} {
        r flushall
        foreach k {x y z} { r set $k [cinfo_value] }
        foreach k {x y z} { compress_key r $k }
        r flushall async
        wait_for_condition 100 50 {[cinfo compressed_values] == 0} else { fail "the sizes did not go to 0" }
        assert_equal 0 [cinfo compressed_values_original_bytes]
        assert_equal 0 [cinfo compressed_values_bytes]
    }
}
