# Tests for the inline compression settings.

source tests/support/compression.tcl

# Starts the server with the given arguments and returns its output. Use it only
# with settings that make the start fail, because the server exits at once.
proc compression_start_fails {args} {
    catch {exec $::VALKEY_SERVER_BIN --port 0 {*}$args 2>@1} out
    return $out
}

start_server {tags {"compression external:skip"}} {
    test {Compression settings have the right defaults} {
        assert_equal {compression-mode off} [r config get compression-mode]
        assert_equal {compression-threads 1} [r config get compression-threads]
        assert_equal {compression-min-value-size 256} [r config get compression-min-value-size]
        assert_equal {compression-max-value-size 131072} [r config get compression-max-value-size]
        assert_equal {compression-dict-size 102400} [r config get compression-dict-size]
        assert_equal {compression-max-inflight-requests 100} [r config get compression-max-inflight-requests]
        assert_equal {compression-lfu-threshold 4} [r config get compression-lfu-threshold]
        assert_equal {compression-sweeper-policy cursor} [r config get compression-sweeper-policy]
        assert_equal {compression-sweeper-backoff-seconds 60} [r config get compression-sweeper-backoff-seconds]
    }

    test {Startup-only compression settings can not be changed} {
        foreach {name value} {compression-mode lz4 compression-threads 2 compression-dict-size 2048} {
            assert_error "*can't set immutable config*" {r config set $name $value}
        }
    }

    test {Compression value sizes can be changed at runtime} {
        r config set compression-min-value-size 512
        r config set compression-max-value-size 4096
        assert_equal {compression-min-value-size 512} [r config get compression-min-value-size]
        assert_equal {compression-max-value-size 4096} [r config get compression-max-value-size]
        # Both in one command, in the order that would fail one by one.
        r config set compression-min-value-size 8192 compression-max-value-size 16384
        assert_equal {compression-min-value-size 8192} [r config get compression-min-value-size]
        r config set compression-min-value-size 256 compression-max-value-size 131072
    }

    test {Compression value sizes have limits} {
        assert_error "*must be between 1 and 131072*" {r config set compression-max-value-size 131073}
        assert_error "*must be between 1 and 131072*" {r config set compression-min-value-size 0}
        assert_equal {compression-max-value-size 131072} [r config get compression-max-value-size]
    }

    test {compression-max-inflight-requests can be changed at runtime and has limits} {
        r config set compression-max-inflight-requests 1
        assert_equal {compression-max-inflight-requests 1} [r config get compression-max-inflight-requests]
        r config set compression-max-inflight-requests 10000
        assert_error "*must be between 1 and 10000*" {r config set compression-max-inflight-requests 0}
        assert_error "*must be between 1 and 10000*" {r config set compression-max-inflight-requests 10001}
        assert_equal {compression-max-inflight-requests 10000} [r config get compression-max-inflight-requests]
        r config set compression-max-inflight-requests 100
    }

    test {compression-lfu-threshold can be changed at runtime and has limits} {
        r config set compression-lfu-threshold 0
        assert_equal {compression-lfu-threshold 0} [r config get compression-lfu-threshold]
        r config set compression-lfu-threshold 255
        assert_error "*must be between 0 and 255*" {r config set compression-lfu-threshold -1}
        assert_error "*must be between 0 and 255*" {r config set compression-lfu-threshold 256}
        assert_equal {compression-lfu-threshold 255} [r config get compression-lfu-threshold]
        r config set compression-lfu-threshold 4
    }

    test {compression-sweeper-policy can be changed at runtime} {
        r config set compression-sweeper-policy random
        assert_equal {compression-sweeper-policy random} [r config get compression-sweeper-policy]
        assert_error "*argument(s) must be one of the following*" {r config set compression-sweeper-policy lru}
        assert_equal {compression-sweeper-policy random} [r config get compression-sweeper-policy]
        r config set compression-sweeper-policy cursor
    }

    test {compression-sweeper-backoff-seconds can be changed at runtime and has limits} {
        r config set compression-sweeper-backoff-seconds 0
        assert_equal {compression-sweeper-backoff-seconds 0} [r config get compression-sweeper-backoff-seconds]
        r config set compression-sweeper-backoff-seconds 3600
        assert_error "*must be between 0 and 3600*" {r config set compression-sweeper-backoff-seconds -1}
        assert_error "*must be between 0 and 3600*" {r config set compression-sweeper-backoff-seconds 3601}
        assert_equal {compression-sweeper-backoff-seconds 3600} [r config get compression-sweeper-backoff-seconds]
        r config set compression-sweeper-backoff-seconds 60
    }

    test {Compression min value size can not be greater than max} {
        assert_error "*compression-min-value-size can't be greater than compression-max-value-size*" {
            r config set compression-min-value-size 4096 compression-max-value-size 1024
        }
        assert_error "*compression-min-value-size can't be greater than compression-max-value-size*" {
            r config set compression-max-value-size 100
        }
        # A failed CONFIG SET changes nothing.
        assert_equal {compression-min-value-size 256} [r config get compression-min-value-size]
        assert_equal {compression-max-value-size 131072} [r config get compression-max-value-size]
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 compression-threads 2 compression-dict-size 2048}} {
    test {Startup-only compression settings can be set at startup} {
        assert_equal {compression-mode lz4} [r config get compression-mode]
        assert_equal {compression-threads 2} [r config get compression-threads]
        assert_equal {compression-dict-size 2048} [r config get compression-dict-size]
    }
}

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 compression-max-inflight-requests 1 maxmemory-policy allkeys-lfu}} {
    test {Values are compressed with one job in flight} {
        set v [string repeat {{"user_id":3,"role":"member"},} 40]
        foreach k {a b c} {
            r set $k $v
            make_cold r $k
        }
        foreach k {a b c} { wait_compressed r $k }
        assert_equal $v [r get a]
    }

    test {With one job in flight, values that do not compress do not block other keys} {
        # Each cron call stops after one job, often in the middle of a
        # bucket. A random value is dropped and queued again on each pass.
        # The next call must go on after it in the bucket, not start the
        # bucket again.
        r config set compression-sweeper-policy cursor
        r config set hz 100
        r flushall
        set v [string repeat {{"user_id":3,"role":"member"},} 40]
        for {set i 0} {$i < 40} {incr i} {
            r set good$i $v
            r set bad$i [randstring 2000 2000 binary]
            make_cold r good$i
            make_cold r bad$i
        }
        wait_for_condition 200 50 {
            [getInfoProperty [r info compression] compressed_values] == 40
        } else {
            fail "not all values that compress were compressed"
        }
        assert_equal raw [r object encoding bad0]
        r config set hz 10
    }
}

tags {"compression external:skip"} {
    test {Bad compression settings stop the server at startup} {
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-mode zstd]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-mode gzip]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-threads 0]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-threads 17]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-max-value-size 131073]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-dict-size 100]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-max-inflight-requests 0]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-lfu-threshold 256]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-sweeper-policy lru]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-sweeper-backoff-seconds 3601]
        assert_match "*compression-min-value-size can't be greater than compression-max-value-size*" \
            [compression_start_fails --compression-min-value-size 2048 --compression-max-value-size 1024]
    }
}
