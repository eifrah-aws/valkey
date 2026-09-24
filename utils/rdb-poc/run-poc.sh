#!/usr/bin/env bash
# RDB compression POC. See README.md in this directory.
#
# Phase 0: fill the keyspace, save an RDB, keep it.
# Step 1:  load the RDB as usual. Baseline.
# Step 2:  load the same RDB, compressing each string value with LZ4 on the way in.
#
# Both steps report load time and used_memory, from the server's own log and INFO.

set -uo pipefail

KEYS=${KEYS:-5000000}
VALUE_SIZE=${VALUE_SIZE:-512}
PORT=${PORT:-7777}

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SERVER="$ROOT/src/valkey-server"
CLI="$ROOT/src/valkey-cli"
WORK=${WORK:-$ROOT/.poc-run}
RDB="$WORK/poc.rdb"
LOG="$WORK/server.log"

for bin in "$SERVER" "$CLI"; do
    [ -x "$bin" ] || { echo "missing $bin, run: make -C src -j8" >&2; exit 1; }
done
mkdir -p "$WORK"

vk() { "$CLI" -p "$PORT" "$@"; }

start_server() { # $1 = extra config, appended
    rm -f "$LOG"
    "$SERVER" --port "$PORT" --dir "$WORK" --dbfilename "$(basename "$RDB")" \
        --save '' --appendonly no --daemonize no --logfile "$LOG" \
        --protected-mode no $1 &
    SRV_PID=$!
    # PING answers while the RDB is still loading, so wait for DBSIZE instead. It
    # returns -LOADING until the load finishes.
    for _ in $(seq 1 3000); do
        n=$(vk dbsize 2>/dev/null)
        case "$n" in
        '' | *LOADING* | *ERR*) ;;
        *) return 0 ;;
        esac
        kill -0 "$SRV_PID" 2>/dev/null || { echo "server died, log:"; cat "$LOG"; exit 1; }
        sleep 0.2
    done
    echo "server did not finish loading" >&2; cat "$LOG"; exit 1
}

stop_server() {
    [ -n "${SRV_PID:-}" ] || return 0
    vk shutdown nosave >/dev/null 2>&1
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=
}

report() { # $1 = label
    local load_line used human keys
    load_line=$(grep "DB loaded from disk" "$LOG" | tail -1)
    used=$(vk info memory | awk -F: '/^used_memory:/ {print $2}' | tr -d '\r')
    human=$(vk info memory | awk -F: '/^used_memory_human:/ {print $2}' | tr -d '\r')
    keys=$(vk dbsize)
    echo "--- $1"
    echo "    ${load_line#*# }"
    echo "    keys=$keys used_memory=$used ($human)"
    grep "POC compress-on-load" "$LOG" | tail -1 | sed 's/^/    /'
}

echo "=== phase 0: build the keyspace and save an RDB ==="
echo "    keys=$KEYS value_size=$VALUE_SIZE work_dir=$WORK"
if [ -f "$RDB" ] && [ "${REUSE_RDB:-1}" = 1 ]; then
    echo "    reusing existing $RDB ($(du -h "$RDB" | cut -f1)). Set REUSE_RDB=0 to rebuild."
else
    start_server ""
    vk flushall >/dev/null
    t0=$(date +%s)
    python3 "$HERE/gen-load.py" "$KEYS" "$VALUE_SIZE" | "$CLI" -p "$PORT" --pipe
    t1=$(date +%s)
    echo "    fill took $((t1 - t0))s, dbsize=$(vk dbsize)"
    echo "    saving..."
    vk save >/dev/null
    stop_server
    echo "    rdb: $(du -h "$RDB" | cut -f1)"
fi

echo
echo "=== step 1: baseline, load the RDB as usual ==="
start_server "--rdb-poc-compress-on-load no"
report "baseline"
stop_server

echo
echo "=== step 2: compress each value with LZ4 while loading ==="
start_server "--rdb-poc-compress-on-load yes"
report "compress-on-load"
stop_server

echo
echo "Note: after step 2 the keyspace holds raw LZ4 output with no header and no"
echo "marking, so its values are not readable. That is intentional for this POC."
