set testmodule [file normalize tests/modules/scan.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module scan keyspace} {
        # the module create a scan command with filtering which also return values
        r set x 1
        r set y 2
        r set z 3
        r hset h f v
        lsort [r scan.scan_strings]
    } {{x 1} {y 2} {z 3}}

    test {Module scan hash listpack} {
        r hmset hh f1 v1 f2 v2
        assert_encoding listpack hh
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}}

    test {Module scan hash listpack with int value} {
        r hmset hh1 f1 1
        assert_encoding listpack hh1
        lsort [r scan.scan_key hh1]
    } {{f1 1}}

    test {Module scan hash dict} {
        r config set hash-max-ziplist-entries 2
        r hmset hh3 f1 v1 f2 v2 f3 v3
        assert_encoding hashtable hh3
        lsort [r scan.scan_key hh3]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan zset listpack} {
        r zadd zz 1 f1 2 f2
        assert_encoding listpack zz
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2}}

    test {Module scan zset skiplist} {
        r config set zset-max-ziplist-entries 2
        r zadd zz1 1 f1 2 f2 3 f3
        assert_encoding btree zz1
        lsort [r scan.scan_key zz1]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan set intset} {
        r del ss
        r sadd ss 1 2
        assert_encoding intset ss
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}}}

    test {Module scan set dict} {
        r del ssa
        r config set set-max-intset-entries 2
        r sadd ssa 1 2 ; # Created as intset
        r sadd ssa 3   ; # Converted to hashtable
        assert_encoding hashtable ssa
        lsort [r scan.scan_key ssa]
    } {{1 {}} {2 {}} {3 {}}}

    test {Module scan set listpack} {
        r del ss1
        r sadd ss1 a b c
        assert_encoding listpack ss1
        lsort [r scan.scan_key ss1]
    } {{a {}} {b {}} {c {}}}

    # ---- VM_ScanKeyRawBorrowed: borrowed (ptr,len) scan, must match native scan ----

    test {Module scan_key_raw hash listpack} {
        r del rh
        r hmset rh f1 v1 f2 v2
        assert_encoding listpack rh
        lsort [r scan.scan_key_raw rh]
    } {{f1 v1} {f2 v2}}

    test {Module scan_key_raw hash listpack with int value} {
        r del rh1
        r hmset rh1 f1 1
        assert_encoding listpack rh1
        lsort [r scan.scan_key_raw rh1]
    } {{f1 1}}

    test {Module scan_key_raw hash dict} {
        r del rh3
        r hmset rh3 f1 v1 f2 v2 f3 v3
        assert_encoding hashtable rh3
        lsort [r scan.scan_key_raw rh3]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan_key_raw zset listpack} {
        r del rz
        r zadd rz 1 f1 2 f2
        assert_encoding listpack rz
        lsort [r scan.scan_key_raw rz]
    } {{f1 1} {f2 2}}

    test {Module scan_key_raw zset btree} {
        r del rz1
        r zadd rz1 1 f1 2 f2 3 f3
        assert_encoding btree rz1
        lsort [r scan.scan_key_raw rz1]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan_key_raw zset fractional score (d2string form)} {
        r del rz2
        r zadd rz2 1.5 f1 2 f2 3 f3
        assert_encoding btree rz2
        assert_equal [lsort [r zrange rz2 0 -1 withscores]] [lsort {f1 1.5 f2 2 f3 3}]
        lsort [r scan.scan_key_raw rz2]
    } {{f1 1.5} {f2 2} {f3 3}}

    test {Module scan_key_raw set intset} {
        r del rs
        r sadd rs 1 2
        assert_encoding intset rs
        lsort [r scan.scan_key_raw rs]
    } {{1 {}} {2 {}}}

    test {Module scan_key_raw set dict} {
        r del rsa
        r sadd rsa 1 2 ; # Created as intset
        r sadd rsa 3   ; # Converted to hashtable
        assert_encoding hashtable rsa
        lsort [r scan.scan_key_raw rsa]
    } {{1 {}} {2 {}} {3 {}}}

    test {Module scan_key_raw set listpack} {
        r del rs1
        r sadd rs1 a b c
        assert_encoding listpack rs1
        lsort [r scan.scan_key_raw rs1]
    } {{a {}} {b {}} {c {}}}

    test {Module scan_key_raw unsupported key type returns empty} {
        # VM_ScanKeyRawBorrowed returns 0 with errno=EINVAL for a wrong-type key,
        # so the command's scan loop terminates immediately and replies with an
        # empty array rather than looping forever.
        r del rstr rlist
        r set rstr hello
        r rpush rlist a b c
        assert_equal {} [r scan.scan_key_raw rstr]
        assert_equal {} [r scan.scan_key_raw rlist]
    }

    test "Unload the module - scan" {
        assert_equal {OK} [r module unload scan]
    }
}

start_server {tags {"modules"} overrides {compression-mode lz4 maxmemory-policy allkeys-lfu}} {
    r module load $testmodule

    test {Module scan reads compressed string values} {
        set v [string repeat {{"user_id":1,"role":"member"},} 40]
        r set x $v
        r set y $v
        # With LFU a new key is cold, so the sweeper compresses it soon.
        wait_for_condition 100 50 {
            [r object encoding x] eq "compressed" && [r object encoding y] eq "compressed"
        } else {
            fail "values were not compressed"
        }
        set res [lsort [r scan.scan_strings]]
        # A module scan reads a copy. The stored values stay compressed.
        assert_equal compressed [r object encoding x]
        assert_equal compressed [r object encoding y]
        set res
    } [list [list x [string repeat {{"user_id":1,"role":"member"},} 40]] [list y [string repeat {{"user_id":1,"role":"member"},} 40]]]

    test {Module scan during a child process does not change compressed values} {
        set v [string repeat {{"user_id":2,"role":"member"},} 40]
        r flushall
        r set x $v
        wait_for_condition 100 50 {[r object encoding x] eq "compressed"} else { fail "x was not compressed" }
        for {set i 0} {$i < 10} {incr i} { r set filler:$i 1 }
        r config set rdb-key-save-delay 200000
        r bgsave
        wait_for_condition 50 20 {[s rdb_bgsave_in_progress] == 1} else { fail "no child" }
        set res [r scan.scan_strings]
        assert_equal compressed [r object encoding x]
        r config set rdb-key-save-delay 0
        waitForBgsave r
        assert {[lsearch -exact $res [list x $v]] >= 0}
    }
}
