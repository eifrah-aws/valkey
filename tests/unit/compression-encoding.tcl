# Tests for compressed string values: the background sweeper compresses cold
# values, and a read decompresses them.
#
# Most servers here use allkeys-lfu. A new key is hot for up to a minute, so
# the tests use make_cold (RESTORE FREQ 4) to make a key cold at once. A read
# or a write makes it hot again.

source tests/support/compression.tcl

# A JSON-like value of about 1 KiB. It compresses well.
proc compression_value {{seed 0}} {
    set v ""
    for {set i 0} {$i < 25} {incr i} {
        append v "{\"user_id\":[expr {$seed + $i}],\"role\":\"member\",\"active\":true},"
    }
    return $v
}

# Returns a key name that was never used. Each check needs a new key.
proc new_key {} {
    incr ::compression_key_id
    return "key:$::compression_key_id"
}

# Sets a witness key and waits until it is compressed. After this, the sweeper
# had a chance to look at the other keys too.
proc wait_for_sweep {r} {
    $r set __witness [compression_value 99]
    compress_key $r __witness
    $r del __witness
}

start_server {tags {"compression external:skip"} overrides {maxmemory-policy allkeys-lfu}} {
    test {Nothing is compressed when compression-mode is off} {
        r set k [compression_value]
        after 500
        assert_equal raw [r object encoding k]
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    test {The sweeper compresses a cold value} {
        r set k [compression_value]
        compress_key r k
        assert_equal [compression_value] [r get k]
    }

    test {With LFU, a new key stays plain until it is cold} {
        set k [new_key]
        r set $k [compression_value]
        assert_equal 5 [r object freq $k]
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
        compress_key r $k
    }

    test {A compressed value uses less memory} {
        set k [new_key]
        r set $k [compression_value]
        compress_key r $k
        set compressed_usage [r memory usage $k]
        r get $k
        assert_equal raw [r object encoding $k]
        assert_lessthan $compressed_usage [r memory usage $k]
    }

    test {GET returns the value, and the key stays plain while it is hot} {
        set k [new_key]
        set v [compression_value 7]
        r set $k $v
        compress_key r $k
        assert_equal $v [r get $k]
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
        assert_equal $v [r get $k]
    }

    test {OBJECT and MEMORY USAGE do not decompress} {
        set k [new_key]
        r set $k [compression_value]
        compress_key r $k
        r object refcount $k
        r object freq $k
        r memory usage $k
        assert_equal compressed [r object encoding $k]
    }

    test {Small values are not compressed} {
        set k [new_key]
        set k2 [new_key]
        r set $k [string repeat a 100]
        r set $k2 [string repeat a 200]
        make_cold r $k
        make_cold r $k2
        wait_for_sweep r
        assert_not_equal compressed [r object encoding $k]
        assert_not_equal compressed [r object encoding $k2]
    }

    test {Values above compression-max-value-size are not compressed} {
        set k [new_key]
        r config set compression-max-value-size 1500
        r set $k [string repeat [compression_value] 2]
        make_cold r $k
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
        r config set compression-max-value-size 131072
    }

    test {Values that do not compress stay plain} {
        set k [new_key]
        r set $k [randstring 2000 2000 binary]
        make_cold r $k
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
    }

    test {The TTL stays when a value is compressed} {
        set k [new_key]
        r set $k [compression_value] EX 1000
        compress_key r $k
        assert_range [r ttl $k] 900 1000
    }

    test {String commands work on compressed values} {
        set v [compression_value]
        foreach {cmd expected} [list \
            strlen [string length $v] \
            {getrange 10 20} [string range $v 10 20] \
            getdel $v \
            {getex PERSIST} $v \
            [list set [compression_value 4] GET] $v \
        ] {
            set k [new_key]
            r set $k $v
            compress_key r $k
            set args [lrange $cmd 1 end]
            assert_equal $expected [r [lindex $cmd 0] $k {*}$args]
        }

        set k [new_key]
        r set $k $v
        compress_key r $k
        r append $k "tail"
        assert_equal "${v}tail" [r get $k]

        set k [new_key]
        r set $k $v
        compress_key r $k
        r setrange $k 0 "XX"
        assert_equal "XX[string range $v 2 end]" [r get $k]
    }

    test {MGET and MULTI work on compressed values} {
        set a [new_key]
        set b [new_key]
        r set $a [compression_value 1]
        r set $b [compression_value 2]
        compress_key r $a
        compress_key r $b
        assert_equal [list [compression_value 1] [compression_value 2]] [r mget $a $b]

        set a [new_key]
        r set $a [compression_value 1]
        compress_key r $a
        r multi
        r get $a
        r append $a "x"
        set res [r exec]
        assert_equal [compression_value 1] [lindex $res 0]
        assert_equal "[compression_value 1]x" [r get $a]
    }

    test {A write makes the key hot, so it stays plain} {
        set k [new_key]
        r set $k [compression_value 1]
        compress_key r $k
        r set $k [compression_value 2]
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
        assert_equal [compression_value 2] [r get $k]
    }

    test {The sweeper pauses while a child process runs} {
        r flushall
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        # Each key takes 0.2 s to save, so the child lives about 2 s.
        r config set rdb-key-save-delay 200000
        r bgsave
        set k [new_key]
        r set $k [compression_value]
        make_cold r $k
        after 1000
        assert_equal 1 [s rdb_bgsave_in_progress]
        assert_equal raw [r object encoding $k]
        r config set rdb-key-save-delay 0
        waitForBgsave r
        wait_compressed r $k
    }

    test {Reads during a child process do not change compressed values} {
        r flushall
        set a [new_key]
        set b [new_key]
        set t [new_key]
        r set $a [compression_value 1]
        r set $b [compression_value 2]
        r set $t [compression_value 3] EX 1000
        foreach k [list $a $b $t] { compress_key r $k }
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }

        # Reads get a plain copy, and the stored value stays compressed.
        assert_equal [compression_value 1] [r get $a]
        assert_equal [list [compression_value 1] [compression_value 2]] [r mget $a $b]
        assert_equal [string length [compression_value 2]] [r strlen $b]
        assert_range [r ttl $t] 900 1000
        r copy $t copy_of_t
        assert_range [r ttl copy_of_t] 900 1000
        assert_equal [compression_value 3] [r get copy_of_t]
        foreach k [list $a $b $t] { assert_equal compressed [r object encoding $k] }

        # A write still decompresses the stored value.
        r append $b "x"
        assert_equal raw [r object encoding $b]
        assert_equal "[compression_value 2]x" [r get $b]

        r config set rdb-key-save-delay 0
        waitForBgsave r
        # Without a child, a read decompresses the stored value again.
        assert_equal [compression_value 1] [r get $a]
        assert_equal raw [r object encoding $a]
    }

    test {DEBUG DIGEST is the same for compressed and plain values} {
        r flushall
        r set k [compression_value]
        compress_key r k
        set digest [debug_digest]
        r get k
        assert_equal raw [r object encoding k]
        assert_equal $digest [debug_digest]
    } {} {needs:debug}

    test {DEBUG RELOAD keeps compressed values} {
        r flushall
        r set k [compression_value]
        compress_key r k
        r debug reload
        assert_equal [compression_value] [r get k]
    } {} {needs:debug}

    test {AOF rewrite without RDB preamble keeps compressed values} {
        r flushall
        r config set appendonly yes
        r config set aof-use-rdb-preamble no
        waitForBgrewriteaof r
        r set k [compression_value]
        compress_key r k
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal [compression_value] [r get k]
        r config set appendonly no
    } {OK} {needs:debug}

    test {Replicas get plain values} {
        start_server {overrides {compression-mode off}} {
            set primary [srv -1 client]
            set replica [srv 0 client]
            $primary flushall
            # Full sync.
            $primary set before [compression_value 1]
            compress_key $primary before
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            # Replication stream.
            $primary set after [compression_value 2]
            wait_for_ofs_sync $primary $replica
            compress_key $primary after
            assert_equal [compression_value 1] [$replica get before]
            assert_equal [compression_value 2] [$replica get after]
            assert_equal raw [$replica object encoding after]
        }
    }

    test {Replicas get plain values from SET with a TTL} {
        start_server {overrides {compression-mode off}} {
            set primary [srv -1 client]
            set replica [srv 0 client]
            $primary flushall
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            # Each of these is sent to the replica as SET key value PXAT ms.
            $primary setex k1 1000 [compression_value 1]
            $primary psetex k2 1000000 [compression_value 2]
            $primary set k3 [compression_value 3] EX 1000
            $primary set k4 [compression_value 4] PX 1000000
            $primary set k5 [compression_value 5] EXAT [expr {[clock seconds] + 1000}]
            $primary set k6 [compression_value 6] EX 1000 GET
            wait_for_ofs_sync $primary $replica
            for {set i 1} {$i <= 6} {incr i} {
                compress_key $primary k$i
                assert_equal [compression_value $i] [$replica get k$i]
                assert_range [$replica ttl k$i] 900 1000
            }
        }
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4}} {
    test {With LRU, only idle values are compressed} {
        # A new key is not idle for COMPRESSOR_MIN_IDLE_SECONDS, so it stays
        # plain. RESTORE IDLETIME makes a key idle.
        r set hot [compression_value 1]
        r set src [compression_value 2]
        r restore cold 0 [r dump src] IDLETIME 120
        wait_compressed r cold
        assert_equal raw [r object encoding hot]
        assert_equal [compression_value 2] [r get cold]
    }
}

tags {"compression external:skip"} {
    test {compression-mode can't be used with forkless-infrastructure-enabled} {
        catch {exec $::VALKEY_SERVER_BIN --port 0 --compression-mode lz4 --forkless-infrastructure-enabled yes 2>@1} out
        assert_match "*compression-mode can't be used with forkless-infrastructure-enabled yes*" $out
    }
}
