#!/usr/bin/env python3
"""Generate RESP SET commands for the RDB compression POC, on stdout.

Pipe into valkey-cli --pipe. Values are JSON-like records of about VALUE_SIZE
bytes. The data shape decides the compression ratio, so it is worth being precise
about what this produces:

- Field names repeat across every value. That is the redundancy a trained
  dictionary exploits, and a single value on its own cannot.
- Field contents differ per key, and the free-text field is built from a word
  list, so there is little repetition inside one value. That is what real JSON
  looks like, and it is why LZ4 without a dictionary saves only a modest amount.

Do not use random bytes: they do not compress, and the POC would measure nothing.
Do not use a repeated filler pattern either: it compresses far better than real
data and flatters the result.

Usage: gen-load.py <key-count> [value-size] | valkey-cli --pipe
"""

import random
import sys

KEY_COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 5_000_000
VALUE_SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 512

WORDS = (
    "order shipped pending refund customer address invoice payment card declined "
    "approved retry queue region zone bucket object cache expired session token "
    "device mobile desktop browser latency request response status code header "
    "body parse error retry limit quota window bucket average median percentile "
    "cluster shard replica primary failover snapshot restore backup archive"
).split()

HEAD = (
    '{"user_id":%d,"user_name":"person_%d","email_address":"person_%d@example.com",'
    '"created_at":"2026-09-24T11:%02d:%02dZ","is_active":%s,"role":"%s",'
    '"preferences":{"language":"en","timezone":"UTC","theme":"%s"},'
    '"login_count":%d,"last_seen_at":"2026-09-24T12:%02d:%02dZ","notes":"'
)
TAIL = '"}'

ROLES = ("member", "admin", "viewer", "owner")
THEMES = ("dark", "light", "system")


def make_value(i):
    rnd = random.Random(i)
    head = HEAD % (
        i,
        i,
        i,
        i % 60,
        (i * 7) % 60,
        "true" if i % 3 else "false",
        ROLES[i % len(ROLES)],
        THEMES[i % len(THEMES)],
        i * 3,
        i % 60,
        (i * 11) % 60,
    )
    budget = VALUE_SIZE - len(head) - len(TAIL)
    if budget <= 0:
        return (head + TAIL)[:VALUE_SIZE]
    words = []
    used = 0
    while used < budget:
        w = rnd.choice(WORDS)
        words.append(w)
        used += len(w) + 1
    notes = " ".join(words)[:budget]
    return head + notes + TAIL


out = sys.stdout
for i in range(KEY_COUNT):
    key = "poc:key:%d" % i
    val = make_value(i)
    out.write("*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (len(key), key, len(val), val))
    if (i & 0xFFFF) == 0:
        out.flush()
out.flush()
