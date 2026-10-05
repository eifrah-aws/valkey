# Tests for the inline compression settings.

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

start_server {tags {"compression external:skip"} overrides {compression-mode lz4 compression-threads 0 compression-dict-size 2048}} {
    test {Startup-only compression settings can be set at startup} {
        assert_equal {compression-mode lz4} [r config get compression-mode]
        assert_equal {compression-threads 0} [r config get compression-threads]
        assert_equal {compression-dict-size 2048} [r config get compression-dict-size]
    }
}

tags {"compression external:skip"} {
    test {Bad compression settings stop the server at startup} {
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-mode zstd]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-mode gzip]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-threads 17]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-max-value-size 131073]
        assert_match "*FATAL CONFIG FILE ERROR*" [compression_start_fails --compression-dict-size 100]
        assert_match "*compression-min-value-size can't be greater than compression-max-value-size*" \
            [compression_start_fails --compression-min-value-size 2048 --compression-max-value-size 1024]
    }
}
