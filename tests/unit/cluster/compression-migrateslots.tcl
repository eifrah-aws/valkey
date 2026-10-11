# Slot migration of compressed values. The source sends plain bytes: the
# child that writes the snapshot decompresses each value. The value on the
# source stays compressed, and the target's sweeper compresses it again.
# See design-docs/inline-compression.md, section 4.1.

source tests/support/compression.tcl

# A JSON-like value of about 1 KiB. It compresses well.
proc migrate_value {seed} {
    return [string repeat "{\"user_id\":$seed,\"role\":\"member\",\"active\":true}," 25]
}

proc migrate_info {idx field} {
    getInfoProperty [R $idx info compression] $field
}

# A new key starts at LFU 5. The snapshot sends SET and then PEXPIREAT for a
# key with a TTL, and on the target the PEXPIREAT moves the counter to 6. With
# compression-lfu-threshold 6, all keys are cold at once, on both sides, and
# the tests only wait for the sweeper.
start_cluster 3 0 {tags {logreqres:skip external:skip cluster network compression} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu compression-lfu-threshold 6 compression-sweeper-policy cursor}} {
    set tag "{6ZJ}"
    set n 20
    set node0_id [R 0 CLUSTER MYID]

    test "Slot migration sends compressed values as plain bytes" {
        R 2 set $tag:k0 [migrate_value 0] px 1000000
        for {set i 1} {$i < $n} {incr i} {
            R 2 set $tag:k$i [migrate_value $i]
        }
        for {set i 0} {$i < $n} {incr i} {
            wait_compressed [Rn 2] $tag:k$i
        }

        set decompressed [migrate_info 2 compression_total_values_decompressed]
        set copies [migrate_info 2 compression_total_temporary_copies_made]

        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        wait_for_condition 100 100 {
            [R 0 CLUSTER COUNTKEYSINSLOT 16383] == $n && [R 2 CLUSTER COUNTKEYSINSLOT 16383] == 0
        } else {
            fail "slot 16383 was not migrated"
        }
        wait_for_cluster_propagation

        # Only the child decompressed the values. The main thread of the
        # source did not change them.
        assert_equal $decompressed [migrate_info 2 compression_total_values_decompressed]
        assert_equal $copies [migrate_info 2 compression_total_temporary_copies_made]
    }

    test "The target compresses migrated values again" {
        # Check the encoding before any read: a GET can move the LFU counter
        # above the threshold.
        for {set i 0} {$i < $n} {incr i} {
            wait_compressed [Rn 0] $tag:k$i
        }
        for {set i 0} {$i < $n} {incr i} {
            assert_equal [migrate_value $i] [R 0 get $tag:k$i]
        }
        assert_morethan [R 0 pttl $tag:k0] 0
        assert_equal -1 [R 0 pttl $tag:k1]
    }
}
