# Tests for incoming values: a replica compresses the values that its primary
# sends at once, without waiting until they are cold. The replica queues the
# key names (compression_incoming_pending_keys) and the workers compress them.
#
# The replica uses allkeys-lfu, so a new key is hot and the sweeper does not
# compress it. Only the incoming path can compress it. The primary has
# compression off, so the values arrive plain.

source tests/support/compression.tcl

# A JSON-like value of about 1 KiB. It compresses well.
proc incoming_value {{seed 0}} {
    return [string repeat "{\"user_id\":$seed,\"role\":\"member\",\"active\":true}," 25]
}

proc cinfo {r field} {
    return [getInfoProperty [$r info compression] $field]
}

proc wait_compressed_values {r n} {
    wait_for_condition 100 50 {
        [cinfo $r compressed_values] == $n
    } else {
        fail "compressed_values is [cinfo $r compressed_values], not $n"
    }
}

start_server {tags {"compression external:skip"}} {
    start_server {overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
        set primary [srv -1 client]
        set replica [srv 0 client]
        $replica replicaof [srv -1 host] [srv -1 port]
        wait_for_sync $replica

        test {A replica compresses the values from its primary at once} {
            set queued [cinfo $replica compression_total_incoming_jobs_queued]
            for {set i 0} {$i < 100} {incr i} {
                $primary set k$i [incoming_value $i]
            }
            wait_for_ofs_sync $primary $replica
            wait_compressed_values $replica 100
            assert_equal 0 [cinfo $replica compression_incoming_pending_keys]
            assert_equal [expr {$queued + 100}] [cinfo $replica compression_total_incoming_jobs_queued]
            # The sweeper did not do it: the keys are hot.
            assert_equal 0 [cinfo $replica compression_total_jobs_queued]
            assert_equal [$primary debug digest] [$replica debug digest]
            for {set i 0} {$i < 100} {incr i} {
                assert_equal [incoming_value $i] [$replica get k$i]
            }
        } {} {needs:debug}

        test {A replica compresses the new value of an APPEND from its primary} {
            $primary set a [incoming_value 1]
            wait_for_ofs_sync $primary $replica
            wait_compressed $replica a
            $primary append a [incoming_value 2]
            wait_for_ofs_sync $primary $replica
            wait_compressed $replica a
            assert_equal [$primary get a] [$replica get a]
        }

        test {A key that is written again while its job runs gets a new job} {
            # The first job waits 0.5 s in the worker, so the second write
            # arrives while it is in flight.
            $replica config set compression-debug-job-delay 500000
            $primary set r1 [incoming_value 30]
            wait_for_ofs_sync $primary $replica
            wait_for_condition 50 10 {
                [cinfo $replica compression_jobs_in_flight] == 1
            } else {
                fail "no job in flight"
            }
            $primary set r1 [incoming_value 31]
            wait_for_ofs_sync $primary $replica
            # The key waits in the set until the old job is done.
            assert_equal 1 [cinfo $replica compression_incoming_pending_keys]
            $replica config set compression-debug-job-delay 0

            # The old job is dropped (the value changed), and the new value
            # is compressed.
            wait_compressed $replica r1
            assert_equal 0 [cinfo $replica compression_incoming_pending_keys]
            assert_equal [incoming_value 31] [$replica get r1]
        }

        test {Values that can not be compressed are skipped} {
            set queued [cinfo $replica compression_total_incoming_jobs_queued]
            $primary set small abc
            $primary set n 12345
            $primary rpush list [incoming_value 3]
            $primary del k0
            wait_for_ofs_sync $primary $replica
            wait_for_condition 100 50 {
                [cinfo $replica compression_incoming_pending_keys] == 0
            } else {
                fail "the incoming keys were not drained"
            }
            assert_equal $queued [cinfo $replica compression_total_incoming_jobs_queued]
            assert_equal embstr [$replica object encoding small]
        }

        test {Incoming keys wait during a child process, and a deleted or changed key is handled} {
            # A slow BGSAVE on the replica pauses the incoming keys.
            $replica config set rdb-key-save-delay 100000
            $replica bgsave
            wait_for_condition 50 20 {[s 0 rdb_bgsave_in_progress] == 1} else { fail "no save" }

            $primary set p1 [incoming_value 10]
            $primary set p2 [incoming_value 11]
            $primary set p3 [incoming_value 12]
            $primary set p1 [incoming_value 13]
            $primary del p2
            wait_for_ofs_sync $primary $replica
            # p1 is there once, p2 is deleted, p3 is new.
            assert_equal 3 [cinfo $replica compression_incoming_pending_keys]
            assert_equal raw [$replica object encoding p1]

            $replica config set rdb-key-save-delay 0
            $replica bgsave cancel
            wait_for_condition 100 50 {[s 0 rdb_bgsave_in_progress] == 0} else { fail "save not done" }
            wait_compressed $replica p1
            wait_compressed $replica p3
            assert_equal 0 [cinfo $replica compression_incoming_pending_keys]
            assert_equal [incoming_value 13] [$replica get p1]
            assert_equal 0 [$replica exists p2]
        } {} {needs:debug}

        test {FLUSHALL while keys are pending} {
            $replica config set rdb-key-save-delay 100000
            $replica bgsave
            wait_for_condition 50 20 {[s 0 rdb_bgsave_in_progress] == 1} else { fail "no save" }
            $primary set f1 [incoming_value 20]
            $primary set f2 [incoming_value 21]
            $primary flushall
            wait_for_ofs_sync $primary $replica
            assert_morethan [cinfo $replica compression_incoming_pending_keys] 0

            $replica config set rdb-key-save-delay 0
            $replica bgsave cancel
            wait_for_condition 100 50 {[s 0 rdb_bgsave_in_progress] == 0} else { fail "save not done" }
            wait_for_condition 100 50 {
                [cinfo $replica compression_incoming_pending_keys] == 0
            } else {
                fail "the incoming keys were not drained"
            }
            assert_equal 0 [cinfo $replica compressed_values]
        }
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    test {A normal client write is not an incoming key} {
        r set k [incoming_value]
        after 300
        assert_equal raw [r object encoding k]
        assert_equal 0 [cinfo r compression_incoming_pending_keys]
        assert_equal 0 [cinfo r compression_total_incoming_jobs_queued]
    }
}
