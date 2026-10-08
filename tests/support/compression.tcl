# Helpers for the inline compression tests.

# Sets the LFU counter of key to COMPRESSOR_LFU_THRESHOLD (4), so the sweeper
# sees it as cold. The value and the TTL stay. A new key starts at 5, and with
# LFU eviction it is hot until the counter decays to 4. This takes up to a
# minute, so the tests use this helper. At 4, one read moves the counter to 5,
# and the key is hot again, like a real key. It needs an LFU maxmemory-policy.
proc make_cold {r key} {
    set ttl [$r pttl $key]
    if {$ttl < 0} { set ttl 0 }
    $r restore $key $ttl [$r dump $key] REPLACE FREQ 4
}

# Waits until the sweeper compressed key.
proc wait_compressed {r key} {
    wait_for_condition 100 50 {
        [$r object encoding $key] eq "compressed"
    } else {
        fail "$key was not compressed"
    }
}

# Makes key cold, and waits until the sweeper compressed it.
proc compress_key {r key} {
    make_cold $r $key
    wait_compressed $r $key
}
