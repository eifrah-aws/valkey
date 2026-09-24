#!/usr/bin/env bash
#
# RDB compression POC, one command.
#
#   ./run-rdb-poc.sh
#
# Builds the server, fills a keyspace, saves an RDB, then loads that same RDB
# twice: once as usual, and once compressing every string value with LZ4 on the
# way in. Reports load time and used_memory for both, and the compression ratio.
#
# Branch valkey-inline-compression-rdb-poc only. Do not merge.
# Background and the findings so far: utils/rdb-poc/README.md
#
# Knobs, all optional:
#   KEYS=5000000        number of keys
#   VALUE_SIZE=512      bytes per value
#   REPEATS=3           how many times to run each load
#   PORT=6379           server port. The script stops every running valkey-server
#                       first, then refuses to run if the port is still taken.
#   WORK=<repo>/.poc-run  scratch directory, holds the RDB and the logs
#   REUSE_RDB=1         reuse an existing RDB in WORK. Set 0 to rebuild it.
#   SKIP_BUILD=0        set 1 to skip the build
#   MALLOC=             passed to make. Leave unset to take the platform default,
#                       which is jemalloc on Linux and libc on macOS.

set -uo pipefail

KEYS=${KEYS:-5000000}
VALUE_SIZE=${VALUE_SIZE:-512}
REPEATS=${REPEATS:-3}
PORT=${PORT:-6379}
REUSE_RDB=${REUSE_RDB:-1}
SKIP_BUILD=${SKIP_BUILD:-0}

ROOT=$(cd "$(dirname "$0")" && pwd)
SERVER="$ROOT/src/valkey-server"
CLI="$ROOT/src/valkey-cli"
GEN="$ROOT/utils/rdb-poc/gen-load.py"
WORK=${WORK:-$ROOT/.poc-run}
RDB="$WORK/poc.rdb"
LOG="$WORK/server.log"
RESULTS="$WORK/results.txt"

SRV_PID=

cleanup() {
    if [ -n "$SRV_PID" ] && kill -0 "$SRV_PID" 2>/dev/null; then
        "$CLI" -p "$PORT" shutdown nosave >/dev/null 2>&1
        sleep 1
        kill -9 "$SRV_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

die() {
    echo "error: $*" >&2
    exit 1
}
vk() { "$CLI" -p "$PORT" "$@"; }

human_bytes() { # $1 = bytes
    awk -v b="$1" 'BEGIN {
        if (b >= 1073741824) printf "%.2f GB", b/1073741824;
        else if (b >= 1048576) printf "%.1f MB", b/1048576;
        else printf "%d B", b;
    }'
}

cpus() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

total_ram_bytes() {
    if [ -r /proc/meminfo ]; then
        awk '/^MemTotal:/ {print $2 * 1024}' /proc/meminfo
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.memsize 2>/dev/null || echo 0
    else
        echo 0
    fi
}

# ---------------------------------------------------------------- environment

echo "=== environment ==="
echo "    uname:  $(uname -srm)"
echo "    cpus:   $(cpus)"
ram=$(total_ram_bytes)
[ "$ram" != 0 ] && echo "    ram:    $(human_bytes "$ram")"
echo "    repo:   $ROOT"
echo "    branch: $(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null) at $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"
echo "    keys=$KEYS value_size=$VALUE_SIZE repeats=$REPEATS port=$PORT"
echo "    work:   $WORK"

# A rough check. The baseline keyspace needs roughly value_size + 170 bytes per
# key, and the RDB on disk needs about value_size + 25.
need_mem=$(awk -v k="$KEYS" -v v="$VALUE_SIZE" 'BEGIN {print int(k * (v + 170))}')
need_disk=$(awk -v k="$KEYS" -v v="$VALUE_SIZE" 'BEGIN {print int(k * (v + 25))}')
echo "    needs:  about $(human_bytes "$need_mem") of RAM and $(human_bytes "$need_disk") of disk"
if [ "$ram" != 0 ] && [ "$need_mem" -gt "$ram" ]; then
    echo "    WARNING: that is more RAM than this host has. Lower KEYS." >&2
fi

mkdir -p "$WORK" || die "cannot create $WORK"
# Resolve symlinks, so the dir the server reports back matches what we compare
# against. On macOS /tmp is a symlink to /private/tmp.
WORK=$(cd "$WORK" && pwd -P)
RDB="$WORK/poc.rdb"
LOG="$WORK/server.log"
RESULTS="$WORK/results.txt"

port_in_use() { # $1 = port
    if command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1 && return 0
    fi
    # Fall back to asking. A reply on the port means something is there.
    "$CLI" -p "$1" ping >/dev/null 2>&1 && return 0
    return 1
}

# Stop every valkey-server on this host before we start. A stale server holding
# our port is dangerous, because phase 0 runs FLUSHALL and would wipe it.
stop_all_valkey_servers() {
    local pids i
    pids=$(pgrep -f 'valkey-server' 2>/dev/null)
    if [ -z "$pids" ]; then
        echo "    no valkey-server process is running"
        return 0
    fi
    echo "    stopping these valkey-server processes:"
    # shellcheck disable=SC2086
    ps -o pid=,command= -p $pids 2>/dev/null | sed 's/^/      /'
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null
    for i in $(seq 1 50); do
        pgrep -f 'valkey-server' >/dev/null 2>&1 || { echo "    all stopped"; return 0; }
        sleep 0.2
    done
    echo "    some did not stop in 10s, sending SIGKILL"
    pkill -9 -f 'valkey-server' 2>/dev/null
    sleep 1
    pgrep -f 'valkey-server' >/dev/null 2>&1 && die "could not stop every valkey-server"
    echo "    all stopped"
    return 0
}

echo
echo "=== stopping any running valkey-server ==="
stop_all_valkey_servers

if port_in_use "$PORT"; then
    echo "error: port $PORT is still in use after stopping every valkey-server." >&2
    echo "       Something that is not a valkey-server holds it:" >&2
    if command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null | sed 's/^/       /' >&2
    fi
    echo "       Free it, or run again with PORT=<other>." >&2
    exit 1
fi

# ---------------------------------------------------------------------- build

if [ "$SKIP_BUILD" = 1 ]; then
    echo
    echo "=== build: skipped (SKIP_BUILD=1) ==="
else
    echo
    echo "=== build ==="
    make_args="-C $ROOT/src -j$(cpus) valkey-server valkey-cli"
    [ -n "${MALLOC:-}" ] && make_args="$make_args MALLOC=$MALLOC"
    echo "    make $make_args"
    # shellcheck disable=SC2086
    if ! make $make_args >"$WORK/build.log" 2>&1; then
        tail -40 "$WORK/build.log" >&2
        die "build failed, full log in $WORK/build.log"
    fi
    echo "    ok"
fi

[ -x "$SERVER" ] || die "missing $SERVER"
[ -x "$CLI" ] || die "missing $CLI"
[ -r "$GEN" ] || die "missing $GEN"
command -v python3 >/dev/null 2>&1 || die "python3 is required by $GEN"

# --------------------------------------------------------------------- server

start_server() { # $1 = extra config
    rm -f "$LOG"
    # shellcheck disable=SC2086
    "$SERVER" --port "$PORT" --dir "$WORK" --dbfilename "$(basename "$RDB")" \
        --save '' --appendonly no --daemonize no --logfile "$LOG" \
        --protected-mode no $1 &
    SRV_PID=$!
    # PING answers while the RDB is still loading, so wait for DBSIZE. It returns
    # -LOADING until the load finishes.
    local i n
    for i in $(seq 1 6000); do
        n=$(vk dbsize 2>/dev/null)
        case "$n" in
        '' | *LOADING* | *ERR*) ;;
        *) return 0 ;;
        esac
        kill -0 "$SRV_PID" 2>/dev/null || { cat "$LOG" >&2; die "server exited during startup"; }
        sleep 0.2
    done
    cat "$LOG" >&2
    die "server did not finish loading"
}

# Proves the server answering on $PORT is the one we just started, before any
# command that writes. Checks the pid, the working directory, and the presence of
# the POC switch, which only this branch has.
verify_our_server() {
    local their_pid their_dir
    their_pid=$(info_field server process_id)
    [ "$their_pid" = "$SRV_PID" ] ||
        die "port $PORT answers from pid $their_pid, but we started $SRV_PID. Refusing to continue."
    their_dir=$(vk config get dir | tail -1 | tr -d '\r')
    [ "$their_dir" = "$WORK" ] ||
        die "server on port $PORT reports dir=$their_dir, expected $WORK. Refusing to continue."
    vk config get rdb-poc-compress-on-load | grep -q rdb-poc-compress-on-load ||
        die "server on port $PORT has no rdb-poc-compress-on-load. It is not this build."
}

stop_server() {
    [ -n "$SRV_PID" ] || return 0
    vk shutdown nosave >/dev/null 2>&1
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=
}

info_field() { vk info "$1" | awk -F: -v f="$2" '$1 == f {gsub(/\r/, "", $2); print $2}'; }

# ------------------------------------------------------- phase 0: fill and save

echo
echo "=== phase 0: build the keyspace and save an RDB ==="
if [ -f "$RDB" ] && [ "$REUSE_RDB" = 1 ]; then
    echo "    reusing $RDB, $(human_bytes "$(wc -c <"$RDB")"). Set REUSE_RDB=0 to rebuild."
else
    rm -f "$RDB"
    start_server ""
    verify_our_server
    cp -f "$LOG" "$WORK/server-fill.log" 2>/dev/null
    ALLOCATOR=$(info_field memory mem_allocator)
    echo "    allocator: $ALLOCATOR"
    vk flushall >/dev/null
    t0=$(date +%s)
    python3 "$GEN" "$KEYS" "$VALUE_SIZE" | "$CLI" -p "$PORT" --pipe >"$WORK/fill.log" 2>&1 ||
        { tail -20 "$WORK/fill.log" >&2; die "fill failed"; }
    t1=$(date +%s)
    got=$(vk dbsize)
    echo "    filled $got keys in $((t1 - t0))s"
    [ "$got" = "$KEYS" ] || echo "    WARNING: expected $KEYS keys, got $got" >&2
    echo "    saving..."
    save_reply=$(vk save)
    [ "$save_reply" = OK ] || die "SAVE replied: $save_reply"
    stop_server
    [ -s "$RDB" ] || die "SAVE said OK but $RDB is missing or empty"
    echo "    rdb: $(human_bytes "$(wc -c <"$RDB")")"
fi

# ------------------------------------------------------------ the two load runs

: >"$RESULTS"

run_load() { # $1 = mode, no|yes   $2 = run number
    start_server "--rdb-poc-compress-on-load $1"
    verify_our_server
    cp -f "$LOG" "$WORK/server-load-$1-$2.log" 2>/dev/null
    local secs used rss alloc keys poc
    secs=$(grep "DB loaded from disk" "$LOG" | tail -1 | sed 's/.*: \([0-9.]*\) seconds/\1/')
    used=$(info_field memory used_memory)
    rss=$(info_field memory used_memory_rss)
    alloc=$(info_field memory mem_allocator)
    keys=$(vk dbsize)
    poc=$(grep "POC compress-on-load" "$LOG" | tail -1 | sed 's/.*POC compress-on-load: //')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$secs" "$used" "$rss" "$keys" "$poc" >>"$RESULTS"
    echo "    run $2: load=${secs}s used_memory=$(human_bytes "$used") rss=$(human_bytes "$rss") keys=$keys"
    [ -n "$poc" ] && echo "           $poc"
    ALLOCATOR=$alloc
    stop_server
}

echo
echo "=== step 1: baseline, load the RDB as usual ==="
for r in $(seq 1 "$REPEATS"); do run_load no "$r"; done

echo
echo "=== step 2: compress each value with LZ4 while loading ==="
for r in $(seq 1 "$REPEATS"); do run_load yes "$r"; done

# ------------------------------------------------------------------ the summary

echo
echo "=== summary ==="
awk -F'\t' -v alloc="$ALLOCATOR" -v keys="$KEYS" -v vsize="$VALUE_SIZE" '
function human(b) {
    b = b + 0;  # substr() returns a string, and a string compares lexically
    if (b >= 1073741824) return sprintf("%.2f GB", b/1073741824);
    if (b >= 1048576) return sprintf("%.1f MB", b/1048576);
    if (b >= 1024) return sprintf("%.1f KB", b/1024);
    return sprintf("%d B", b);
}
$1 == "no"  { n_secs += $3; n_used = $4; n_rss = $5; n_cnt++ }
$1 == "yes" { y_secs += $3; y_used = $4; y_rss = $5; y_cnt++
              if (match($7, /plain=[0-9]+/))      { plain = substr($7, RSTART+6, RLENGTH-6) }
              if (match($7, /compressed=[0-9]+ bytes/)) { comp = substr($7, RSTART+11, RLENGTH-17) }
              if (match($7, /lz4_time=[0-9.]+/))  { lz4 = substr($7, RSTART+9, RLENGTH-9) }
            }
END {
    nb = n_secs / n_cnt; yb = y_secs / y_cnt;
    printf "    keys=%s value_size=%s allocator=%s runs=%d\n", keys, vsize, alloc, n_cnt;
    printf "    %-22s %14s %14s %12s\n", "", "baseline", "compress", "change";
    printf "    %-22s %13.2fs %13.2fs %+10.1f%%\n", "load time (mean)", nb, yb, (yb-nb)/nb*100;
    printf "    %-22s %14s %14s %+10.1f%%\n", "used_memory", human(n_used), human(y_used), (y_used-n_used)/n_used*100;
    printf "    %-22s %14s %14s %+10.1f%%\n", "used_memory_rss", human(n_rss), human(y_rss), (y_rss-n_rss)/n_rss*100;
    if (plain > 0) {
        printf "    %-22s %14s %14s %+10.1f%%\n", "value bytes", human(plain), human(comp), (comp-plain)/plain*100;
        printf "    %-22s %14s %14.2fs %11s\n", "time inside LZ4", "-", lz4, sprintf("%.0f%% of delta", lz4/(yb-nb)*100);
    }
}' "$RESULTS"

echo
echo "    per-run rows: $RESULTS"
echo "    server log:   $LOG"
echo
echo "Note: after step 2 the keyspace holds raw LZ4 output with no header and no"
echo "marking, so its values are not readable. That is intentional for this POC."
