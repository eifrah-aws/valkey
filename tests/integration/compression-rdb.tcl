# Tests for compressed string values in RDB (RDB version 82). A compressed
# value is saved as its frame (RDB_TYPE_STRING_COMPRESSED), and it is loaded
# back as the same frame. A target that knows only an older RDB version gets
# the plain bytes.
#
# The servers use allkeys-lfu, so a new key is hot and the sweeper does not
# compress it. The tests compress keys with DEBUG COMPRESS-KEY.

source tests/support/compression.tcl

# A JSON-like value of about 1 KiB. It compresses well.
proc rdb_value {{seed 0}} {
    set v ""
    for {set i 0} {$i < 25} {incr i} {
        append v "{\"user_id\":[expr {$seed + $i}],\"role\":\"member\",\"active\":true},"
    }
    return $v
}

proc cinfo {r field} {
    return [getInfoProperty [$r info compression] $field]
}

# The offset of the frame in a DUMP payload: after the type byte and the RDB
# length of the frame.
proc dump_frame_offset {payload} {
    binary scan [string index $payload 1] cu b
    switch [expr {$b >> 6}] {
        0 { return 2 }
        1 { return 3 }
        2 { return 6 }
    }
    fail "unexpected length encoding in the DUMP payload"
}

# Returns the payload with the byte at offset changed to value.
proc set_payload_byte {payload offset value} {
    return [string replace $payload $offset $offset [binary format cu $value]]
}

# Acts as a replica that says it runs Valkey version, and returns the RDB that
# the primary sends in the full sync. The primary must use diskless sync with
# no delay, because only a replica with "capa eof" gets an older RDB version.
proc fullsync_rdb {host port version} {
    set s [socket $host $port]
    fconfigure $s -translation binary -blocking 1
    puts -nonewline $s "REPLCONF capa eof version $version\r\n"
    flush $s
    assert_equal "+OK" [string trim [gets $s]]
    puts -nonewline $s "PSYNC ? -1\r\n"
    flush $s
    # Empty lines are pings.
    while 1 {
        set line [string trim [gets $s]]
        if {[string match {+FULLRESYNC*} $line]} break
    }
    while 1 {
        set line [string trim [gets $s]]
        if {$line ne {}} break
    }
    assert_match {$EOF:*} $line
    set mark [string range $line 5 end]
    set rdb ""
    fconfigure $s -blocking 0
    set deadline [expr {[clock milliseconds] + 10000}]
    while {[string range $rdb end-[expr {[string length $mark] - 1}] end] ne $mark} {
        if {[clock milliseconds] > $deadline} {
            close $s
            fail "no end mark in the full sync"
        }
        set chunk [read $s]
        if {$chunk eq {}} { after 10 }
        append rdb $chunk
    }
    close $s
    return [string range $rdb 0 end-[string length $mark]]
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    test {DEBUG RELOAD keeps compressed values compressed, and plain values plain} {
        r flushall
        r set c [rdb_value 1]
        r set p [rdb_value 2]
        r set small abc
        r set n 12345
        r debug compress-key c
        set info [r info compression]
        set values [getInfoProperty $info compressed_values]
        set bytes [getInfoProperty $info compressed_values_bytes]
        set original [getInfoProperty $info compressed_values_original_bytes]
        assert_equal 1 $values

        r debug reload

        assert_equal compressed [r object encoding c]
        assert_equal raw [r object encoding p]
        assert_equal embstr [r object encoding small]
        assert_equal int [r object encoding n]
        # The load adds the frames to the gauges.
        set info [r info compression]
        assert_equal $values [getInfoProperty $info compressed_values]
        assert_equal $bytes [getInfoProperty $info compressed_values_bytes]
        assert_equal $original [getInfoProperty $info compressed_values_original_bytes]
        # The load does not decompress.
        assert_equal 0 [getInfoProperty $info compression_total_values_decompressed]
        assert_equal [rdb_value 1] [r get c]
        assert_equal [rdb_value 2] [r get p]
        assert_equal abc [r get small]
        assert_equal 12345 [r get n]
    } {} {needs:debug}

    test {A compressed value with a TTL keeps the TTL across DEBUG RELOAD} {
        r flushall
        r set c [rdb_value 3] PX 1000000
        r debug compress-key c
        r debug reload
        assert_equal compressed [r object encoding c]
        assert_range [r pttl c] 1 1000000
        assert_equal [rdb_value 3] [r get c]
    } {} {needs:debug}

    test {A server with compression-mode off loads compressed values} {
        r flushall
        r set c [rdb_value 4]
        r debug compress-key c
        r save
        set dir [tmpdir compression-rdb]
        file copy [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]] $dir/off.rdb
        start_server [list overrides [list dir $dir dbfilename off.rdb compression-mode off]] {
            assert_equal compressed [r object encoding c]
            assert_equal 1 [cinfo r compressed_values]
            assert_equal [rdb_value 4] [r get c]
            assert_equal raw [r object encoding c]
        }
    } {} {needs:debug}

    test {DUMP of a compressed value has the frame, and RESTORE keeps it} {
        r flushall
        r set c [rdb_value 5]
        r set p [rdb_value 5]
        r debug compress-key c
        set payload [r dump c]
        # RDB_TYPE_STRING_COMPRESSED is 24, RDB_TYPE_STRING is 0.
        assert_equal 24 [scan [string index $payload 0] %c]
        assert_equal 0 [scan [string index [r dump p] 0] %c]
        r restore c2 0 $payload
        assert_equal compressed [r object encoding c2]
        assert_equal 2 [cinfo r compressed_values]
        assert_equal [rdb_value 5] [r get c2]
        assert_equal compressed [r object encoding c]
    }

    test {RESTORE refuses a bad frame} {
        r flushall
        r set c [rdb_value 6]
        r debug compress-key c
        set payload [r dump c]
        set off [dump_frame_offset $payload]
        r debug set-skip-checksum-validation 1

        # Unknown algorithm (offset 8 of the frame).
        set bad [set_payload_byte $payload [expr {$off + 8}] 200]
        set lines [count_log_lines 0]
        assert_error "*Bad data format*" {r restore bad1 0 $bad}
        verify_log_message 0 "*Bad compressed string: unknown frame algorithm*" $lines

        # Unknown format version (high 4 bits of offset 9).
        set bad [set_payload_byte $payload [expr {$off + 9}] 0xF0]
        set lines [count_log_lines 0]
        assert_error "*Bad data format*" {r restore bad2 0 $bad}
        verify_log_message 0 "*Bad compressed string: unknown frame format version*" $lines

        # A dictionary id. Dictionaries are not supported yet.
        set bad [set_payload_byte $payload [expr {$off + 4}] 1]
        set lines [count_log_lines 0]
        assert_error "*Bad data format*" {r restore bad3 0 $bad}
        verify_log_message 0 "*Bad compressed string: the frame needs a dictionary*" $lines

        # A wrong uncompressed length: the header is good, but the body does
        # not decompress to that length.
        binary scan [string index $payload $off] cu low
        set bad [set_payload_byte $payload $off [expr {($low + 1) % 256}]]
        set lines [count_log_lines 0]
        assert_error "*Bad data format*" {r restore bad4 0 $bad}
        verify_log_message 0 "*Bad compressed string: decompression failed*" $lines

        r debug set-skip-checksum-validation 0
        assert_equal 0 [r exists bad1 bad2 bad3 bad4]
        assert_equal 1 [cinfo r compressed_values]
        assert_equal [rdb_value 6] [r get c]
    } {} {needs:debug}

    test {AOF rewrite with RDB preamble keeps compressed values} {
        r flushall
        r config set appendonly yes
        r config set aof-use-rdb-preamble yes
        waitForBgrewriteaof r
        r set c [rdb_value 7]
        r debug compress-key c
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal compressed [r object encoding c]
        assert_equal [rdb_value 7] [r get c]
        r config set appendonly no
    } {OK} {needs:debug}

    test {valkey-check-rdb reads compressed values} {
        r flushall
        r set c [rdb_value 8]
        r debug compress-key c
        r save
        set file [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
        catch {exec $::VALKEY_CHECK_RDB_BIN $file} result
        assert_match {*\\o/ RDB looks OK! \\o/*} $result
    } {} {needs:debug}

    test {A full sync keeps compressed values compressed on the replica} {
        start_server {overrides {compression-mode off}} {
            set primary [srv -1 client]
            set replica [srv 0 client]
            $primary flushall
            $primary set c [rdb_value 9]
            $primary set p [rdb_value 10]
            $primary debug compress-key c
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            assert_equal compressed [$replica object encoding c]
            assert_equal raw [$replica object encoding p]
            assert_equal 1 [cinfo $replica compressed_values]
            assert_equal [rdb_value 9] [$replica get c]
            assert_equal [rdb_value 10] [$replica get p]
        }
    } {} {needs:debug}

    test {A replica with an older RDB version gets plain bytes} {
        r flushall
        r config set repl-diskless-sync yes
        r config set repl-diskless-sync-delay 0
        r config set rdbcompression no
        r set c [rdb_value 11]
        r debug compress-key c

        # Valkey 9.2 knows RDB 81, so it gets RDB_TYPE_STRING and the value.
        set rdb [fullsync_rdb [srv 0 host] [srv 0 port] 9.2.0]
        assert_equal VALKEY081 [string range $rdb 0 8]
        assert {[string first [rdb_value 11] $rdb] != -1}

        # Valkey 9.3 knows RDB 82, so it gets the frame.
        set rdb [fullsync_rdb [srv 0 host] [srv 0 port] 9.3.0]
        assert_equal VALKEY082 [string range $rdb 0 8]
        assert {[string first [rdb_value 11] $rdb] == -1}

        assert_equal compressed [r object encoding c]
        r config set rdbcompression yes
        r config set repl-diskless-sync-delay 5
    } {OK} {needs:debug needs:repl}
}
