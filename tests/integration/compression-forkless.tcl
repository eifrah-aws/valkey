# Tests for compressed values with forkless save. A thread in the same
# process writes the RDB while the main thread runs commands, so a command
# must not change an object that the save thread holds.
#
# The servers use allkeys-lfu, so a new key is hot and the sweeper does not
# compress it. The tests compress keys with DEBUG COMPRESS-KEY, and slow the
# save down with rdb-key-save-delay.

source tests/support/compression.tcl

# A JSON-like value of about 1 KiB. It compresses well.
proc forkless_value {{seed 0}} {
    return [string repeat "{\"user_id\":$seed,\"role\":\"member\",\"active\":true}," 25]
}

# A value of about 400 bytes, small enough for forkless save to clone it
# (at most 512 bytes with the key).
proc forkless_small_value {{seed 0}} {
    return [string repeat "{\"id\":$seed,\"role\":\"member\"}," 15]
}

proc cinfo {r field} {
    return [getInfoProperty [$r info compression] $field]
}

# Starts a slow forkless BGSAVE: 0.2 s for each key.
proc start_slow_forkless_save {r} {
    $r config set rdb-key-save-delay 200000
    $r bgsave
    wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no save" }
    assert_equal forkless [s rdb_current_bgsave_type]
}

proc finish_save {r} {
    $r config set rdb-key-save-delay 0
    waitForBgsave $r
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu forkless-infrastructure-enabled yes bgsave-default-method forkless save ""}} {
    test {The server starts with compression and forkless save} {
        assert_equal lz4 [lindex [r config get compression-mode] 1]
        assert_equal yes [lindex [r config get forkless-infrastructure-enabled] 1]
    }

    test {A read during a forkless save does not change a value that the save holds} {
        r flushall
        for {set i 0} {$i < 20} {incr i} {
            r set k$i [forkless_value $i]
            r debug compress-key k$i
        }
        set copies [cinfo r compression_total_temporary_copies_made]
        start_slow_forkless_save r

        # Keys that the save thread holds get a temporary copy and stay
        # compressed. The other keys are decompressed in place.
        for {set i 0} {$i < 20} {incr i} {
            assert_equal [forkless_value $i] [r get k$i]
        }
        set still 0
        for {set i 0} {$i < 20} {incr i} {
            if {[r object encoding k$i] eq "compressed"} { incr still }
        }
        assert_morethan $still 0
        assert_morethan [cinfo r compression_total_temporary_copies_made] $copies

        finish_save r
        r debug reload nosave
        # The save thread wrote the keys that it held as frames, as they are
        # (RDB 82), without decompressing them, so they load compressed.
        set loaded 0
        for {set i 0} {$i < 20} {incr i} {
            if {[r object encoding k$i] eq "compressed"} { incr loaded }
        }
        assert_equal $still $loaded
        for {set i 0} {$i < 20} {incr i} {
            assert_equal [forkless_value $i] [r get k$i]
        }
    } {} {needs:debug}
}

# A new server, because the save queue grows with each save. The first save
# queues at most 100 keys, so a write to most of the 1000 keys sends a clone.
start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu forkless-infrastructure-enabled yes bgsave-default-method forkless save ""}} {
    test {A write during a forkless save clones the plain bytes of a compressed value} {
        for {set i 0} {$i < 1000} {incr i} {
            r set s$i [forkless_small_value $i]
            r debug compress-key s$i
        }
        # 10 ms for each key, so the save is still running after the writes.
        r config set rdb-key-save-delay 10000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no save" }
        assert_equal forkless [s rdb_current_bgsave_type]

        # A write to a key that is not saved yet sends a clone of the value to
        # the save thread first. A write to a key that the save holds waits.
        for {set i 0} {$i < 1000} {incr i} {
            r append s$i "-new"
        }
        assert_equal 1 [s rdb_bgsave_in_progress]
        finish_save r

        # The RDB has the values from the start of the save.
        r debug reload nosave
        for {set i 0} {$i < 1000} {incr i} {
            assert_equal [forkless_small_value $i] [r get s$i]
        }
    } {} {needs:debug}
}
