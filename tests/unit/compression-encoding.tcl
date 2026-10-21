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

    test {DEBUG COMPRESS-KEY fails when compression-mode is off} {
        assert_error "*compression-mode is off*" {r debug compress-key k}
    } {} {needs:debug}
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    test {The sweeper compresses a cold value} {
        r set k [compression_value]
        compress_key r k
        assert_equal [compression_value] [r get k]
    }

    test {DEBUG COMPRESS-KEY compresses a hot value now} {
        set k [new_key]
        r set $k [compression_value]
        set info [r info compression]
        set queued [getInfoProperty $info compression_total_jobs_queued]
        set values [getInfoProperty $info compressed_values]
        assert_equal OK [r debug compress-key $k]
        assert_equal compressed [r object encoding $k]
        # The gauges change, the sweeper counters do not.
        set info [r info compression]
        assert_equal [expr {$values + 1}] [getInfoProperty $info compressed_values]
        assert_equal $queued [getInfoProperty $info compression_total_jobs_queued]
        assert_equal [compression_value] [r get $k]
    } {} {needs:debug}

    test {DEBUG COMPRESS-KEY replies with the reason when it can't compress} {
        assert_error "*no such key*" {r debug compress-key nosuchkey}
        set k [new_key]
        r set $k [compression_value]
        r debug compress-key $k
        assert_error "*already compressed*" {r debug compress-key $k}
        r rpush list:$k a
        assert_error "*not a plain string*" {r debug compress-key list:$k}
        # APPEND makes a RAW string. A short SET value is embedded (EMBSTR).
        r set small:$k a
        r append small:$k [string repeat a 99]
        assert_error "*size is outside the configured range*" {r debug compress-key small:$k}
        r set random:$k [randstring 2000 2000 binary]
        assert_error "*does not compress enough*" {r debug compress-key random:$k}
        assert_equal raw [r object encoding random:$k]
    } {} {needs:debug}

    test {Compressing and decompressing a value sends no keyspace events} {
        set k [new_key]
        r set $k [compression_value]
        make_cold r $k
        r config set notify-keyspace-events KEA
        set rd [valkey_deferring_client]
        $rd psubscribe __key*__:*
        $rd read
        # The sweeper installs the frame. The next events must be the ones of
        # the marker key.
        wait_compressed r $k
        r set marker 1
        assert_match "*marker*" [$rd read]
        assert_match "*marker*" [$rd read]
        # A GET decompresses the value in place.
        assert_equal [compression_value] [r get $k]
        assert_equal raw [r object encoding $k]
        r set marker 2
        assert_match "*marker*" [$rd read]
        $rd close
        r config set notify-keyspace-events ""
    }

    test {With LFU, a new key stays plain until it is cold} {
        set k [new_key]
        r set $k [compression_value]
        assert_equal 5 [r object freq $k]
        wait_for_sweep r
        assert_equal raw [r object encoding $k]
        compress_key r $k
    }

    test {compression-lfu-threshold decides when a new key is cold} {
        # A new key starts at 5. With a threshold of 5, it is cold at once.
        r config set compression-lfu-threshold 5
        set k [new_key]
        r set $k [compression_value]
        wake_sweeper r
        wait_compressed r $k
        r config set compression-lfu-threshold 4
        set k2 [new_key]
        r set $k2 [compression_value]
        wait_for_sweep r
        assert_equal raw [r object encoding $k2]
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

    test {Commands that do not read the bytes do not decompress} {
        set k [new_key]
        r set $k [compression_value] EX 1000
        compress_key r $k
        set before [getInfoProperty [r info compression] compression_total_values_decompressed]
        assert_equal string [r type $k]
        assert_equal 1 [r exists $k]
        assert_range [r ttl $k] 900 1000
        assert_range [r pttl $k] 900000 1000000
        assert_morethan [r expiretime $k] 0
        assert_equal compressed [r object encoding $k]
        assert_equal $before [getInfoProperty [r info compression] compression_total_values_decompressed]
    }

    test {STRLEN does not decompress} {
        set k [new_key]
        r set $k [compression_value 5]
        compress_key r $k
        set info [r info compression]
        set decompressed [getInfoProperty $info compression_total_values_decompressed]
        set copies [getInfoProperty $info compression_total_temporary_copies_made]
        assert_equal [string length [compression_value 5]] [r strlen $k]
        assert_equal compressed [r object encoding $k]
        set info [r info compression]
        assert_equal $decompressed [getInfoProperty $info compression_total_values_decompressed]
        assert_equal $copies [getInfoProperty $info compression_total_temporary_copies_made]
        assert_equal 0 [r strlen [new_key]]
    }

    test {DUMP does not decompress, and RESTORE gives the same value} {
        set k [new_key]
        r set $k [compression_value 3]
        compress_key r $k
        set payload [r dump $k]
        assert_equal compressed [r object encoding $k]
        set k2 [new_key]
        r restore $k2 0 $payload
        assert_equal [compression_value 3] [r get $k2]
        assert_equal [compression_value 3] [r get $k]
    }

    test {DEBUG DIGEST-VALUE does not decompress} {
        set k [new_key]
        r set $k [compression_value 4]
        compress_key r $k
        set digest [r debug digest-value $k]
        assert_equal compressed [r object encoding $k]
        r get $k
        assert_equal raw [r object encoding $k]
        assert_equal $digest [r debug digest-value $k]
    } {} {needs:debug}

    test {MIGRATE COPY does not decompress the source} {
        start_server {overrides {compression-mode off}} {
            set src [srv -1 client]
            set dst [srv 0 client]
            set k [new_key]
            $src set $k [compression_value 5]
            compress_key $src $k
            # The test clients use db 9.
            assert_equal OK [$src migrate [srv 0 host] [srv 0 port] $k 9 5000 COPY]
            assert_equal compressed [$src object encoding $k]
            assert_equal [compression_value 5] [$dst get $k]
            assert_equal [compression_value 5] [$src get $k]
        }
    }

    test {Commands that change only the key or the TTL do not decompress} {
        r flushall
        set before [getInfoProperty [r info compression] compression_total_values_decompressed]
        set k [new_key]
        r set $k [compression_value 6]
        compress_key r $k

        assert_equal 1 [r expire $k 1000]
        assert_equal 1 [r pexpire $k 1000000]
        assert_equal 1 [r expireat $k [expr {[clock seconds] + 1000}]]
        assert_equal 1 [r pexpireat $k [expr {[clock milliseconds] + 1000000}]]
        assert_range [r ttl $k] 900 1000
        assert_equal 1 [r persist $k]
        assert_equal -1 [r ttl $k]
        assert_equal 1 [r touch $k]
        assert_equal compressed [r object encoding $k]

        set k2 [new_key]
        r rename $k $k2
        assert_equal compressed [r object encoding $k2]
        set k3 [new_key]
        assert_equal 1 [r renamenx $k2 $k3]
        assert_equal compressed [r object encoding $k3]

        r select 10
        r del $k3
        r select 9
        assert_equal 1 [r move $k3 10]
        r select 10
        assert_equal compressed [r object encoding $k3]
        assert_equal 1 [r move $k3 9]
        r select 9
        assert_equal compressed [r object encoding $k3]

        assert_equal $before [getInfoProperty [r info compression] compression_total_values_decompressed]
        assert_equal 1 [getInfoProperty [r info compression] compressed_values]
        assert_equal [compression_value 6] [r get $k3]
    }

    test {COPY copies the frame and does not decompress} {
        r flushall
        set k [new_key]
        r set $k [compression_value 7] EX 1000
        compress_key r $k
        set before [getInfoProperty [r info compression] compression_total_values_decompressed]
        set k2 [new_key]
        assert_equal 1 [r copy $k $k2]
        assert_equal compressed [r object encoding $k]
        assert_equal compressed [r object encoding $k2]
        assert_range [r ttl $k2] 900 1000
        set info [r info compression]
        assert_equal 2 [getInfoProperty $info compressed_values]
        assert_equal [expr {2 * [string length [compression_value 7]]}] [getInfoProperty $info compressed_values_original_bytes]
        assert_equal $before [getInfoProperty $info compression_total_values_decompressed]
        assert_equal [compression_value 7] [r get $k2]
        assert_equal [compression_value 7] [r get $k]
        r del $k $k2
        assert_equal 0 [getInfoProperty [r info compression] compressed_values]
        assert_equal 0 [getInfoProperty [r info compression] compressed_values_bytes]
    }

    test {Overwriting a compressed key with COPY, RENAME, or MOVE updates INFO compression} {
        r flushall
        r select 10
        r flushdb
        r select 9
        foreach k {a b c d e} {
            r set $k [compression_value]
            compress_key r $k
        }
        assert_equal 1 [r copy a b REPLACE]
        assert_equal OK [r rename c d]
        r select 10
        r set e [compression_value 8]
        compress_key r e
        r select 9
        assert_equal 1 [r move e 10 REPLACE]
        # a, b, d in db 9 and e in db 10.
        assert_equal 4 [getInfoProperty [r info compression] compressed_values]
        r select 10
        assert_equal [compression_value] [r get e]
        r flushdb
        r select 9
        r flushall
        assert_equal 0 [getInfoProperty [r info compression] compressed_values]
        assert_equal 0 [getInfoProperty [r info compression] compressed_values_bytes]
    }

    test {EXPIRE in the past deletes a compressed key} {
        r flushall
        r set k [compression_value]
        compress_key r k
        r expire k -1
        assert_equal 0 [r exists k]
        assert_equal 0 [getInfoProperty [r info compression] compressed_values]
    }

    test {COPY and EXPIRE during a child process do not change the stored value} {
        r flushall
        r set k [compression_value 9]
        compress_key r k
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }
        set made [getInfoProperty [r info compression] compression_total_temporary_copies_made]
        assert_equal 1 [r copy k k2]
        assert_equal 1 [r expire k 1000]
        assert_equal compressed [r object encoding k]
        assert_equal compressed [r object encoding k2]
        assert_equal $made [getInfoProperty [r info compression] compression_total_temporary_copies_made]
        r config set rdb-key-save-delay 0
        waitForBgsave r
        assert_equal [compression_value 9] [r get k2]
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

    test {MULTI and EVAL during a child process hold one temporary copy at a time} {
        r flushall
        set keys {}
        for {set i 0} {$i < 5} {incr i} {
            set k [new_key]
            lappend keys $k
            r set $k [compression_value $i]
            compress_key r $k
        }
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        set made [getInfoProperty [r info compression] compression_total_temporary_copies_made]
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }

        # INFO runs after the GETs. Each GET released its copy when it ended.
        r multi
        foreach k $keys { r get $k }
        r info compression
        set res [r exec]
        for {set i 0} {$i < 5} {incr i} { assert_equal [compression_value $i] [lindex $res $i] }
        assert_equal 0 [getInfoProperty [lindex $res end] compression_temporary_copies]

        set info [r eval {
            for _, k in ipairs(KEYS) do server.call('get', k) end
            return server.call('info', 'compression')
        } [llength $keys] {*}$keys]
        assert_equal 0 [getInfoProperty $info compression_temporary_copies]

        assert_equal [expr {$made + 10}] [getInfoProperty [r info compression] compression_total_temporary_copies_made]
        foreach k $keys { assert_equal compressed [r object encoding $k] }
        r config set rdb-key-save-delay 0
        waitForBgsave r
    }

    test {PFCOUNT during a child process keeps its cached count} {
        r flushall
        # A dense HLL is large and has many zero registers, so it compresses.
        r config set hll-sparse-max-bytes 0
        set k [new_key]
        r pfadd $k a b c d e
        compress_key r $k
        for {set i 0} {$i < 10} {incr i} { r set filler:$i x }
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }

        # PFCOUNT writes the count into the stored value, so it decompresses it
        # in place, also during a child.
        set dirty [s rdb_changes_since_last_save]
        assert_equal 5 [r pfcount $k]
        assert_equal raw [r object encoding $k]
        # The second PFCOUNT uses the cached count, so it does not change the
        # key again.
        assert_equal 5 [r pfcount $k]
        assert_equal [expr {$dirty + 1}] [s rdb_changes_since_last_save]

        r config set rdb-key-save-delay 0
        r config set hll-sparse-max-bytes 3000
        waitForBgsave r
    }

    test {Commands that only check if a key exists do not decompress} {
        set k [new_key]
        r set $k [compression_value]
        compress_key r $k
        set info [r info compression]
        set decompressed [getInfoProperty $info compression_total_values_decompressed]
        set values [getInfoProperty $info compressed_values]

        assert_equal {} [r set $k x NX]
        assert_equal {} [r set $k x IFEQ wrong]
        assert_equal 0 [r msetnx $k x [new_key] y]
        assert_error "*BUSYKEY*" {r restore $k 0 [r dump $k]}
        assert_equal compressed [r object encoding $k]

        # SET replaces the value. The old one is freed, not decompressed.
        r set $k new
        assert_equal new [r get $k]
        set info [r info compression]
        assert_equal $decompressed [getInfoProperty $info compression_total_values_decompressed]
        assert_equal [expr {$values - 1}] [getInfoProperty $info compressed_values]
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

    test {Replicas get compressed values in a full sync, and plain values in the stream} {
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
            # The full sync sends the frame (RDB 82), so the value stays
            # compressed, also with compression-mode off.
            assert_equal compressed [$replica object encoding before]
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
        # A new key is not idle for COMPRESSION_MIN_IDLE_SECONDS, so it stays
        # plain. RESTORE IDLETIME makes a key idle.
        r set hot [compression_value 1]
        r set src [compression_value 2]
        r restore cold 0 [r dump src] IDLETIME 120
        wake_sweeper r
        wait_compressed r cold
        assert_equal raw [r object encoding hot]
        assert_equal [compression_value 2] [r get cold]
    }
}
