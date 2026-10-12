# Helpers for the inline compression tests.

# Sets the LFU counter of key to compression-lfu-threshold (4 by default), so
# the sweeper sees it as cold. The value and the TTL stay. A new key starts at
# 5, and with LFU eviction it is hot until the counter decays to 4. This takes
# up to a minute, so the tests use this helper. Below 5, one read always moves
# the counter up by one, so the key is hot again, like a real key. It needs an
# LFU maxmemory-policy.
proc make_cold {r key} {
    set ttl [$r pttl $key]
    if {$ttl < 0} { set ttl 0 }
    set threshold [lindex [$r config get compression-lfu-threshold] 1]
    $r restore $key $ttl [$r dump $key] REPLACE FREQ $threshold
    wake_sweeper $r
}

# Ends the backoff pause of the cursor sweeper. After a pass with almost no
# eligible key it pauses for compression-sweeper-backoff-seconds, and a change
# of that setting ends the pause. CONFIG SET to the same value changes
# nothing, so this sets 0 and then the old value.
proc wake_sweeper {r} {
    set seconds [lindex [$r config get compression-sweeper-backoff-seconds] 1]
    $r config set compression-sweeper-backoff-seconds 0
    $r config set compression-sweeper-backoff-seconds $seconds
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
