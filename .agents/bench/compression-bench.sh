#!/usr/bin/env bash
# Benchmark for inline compression. Runs on the client machine.
#
# For each scenario in the scenarios file, it:
#   1. restarts valkey-server on the server machine (over SSH) with the
#      scenario's startup arguments, and sets the runtime settings,
#   2. loads the data with sb (SET),
#   3. watches the compression for --watch-seconds, with one CSV row every
#      --watch-step seconds,
#   4. runs GET for each value of --get-touch (100 means all keys).
# At the end it writes summary.csv, with one row per scenario.
#
# Run ./compression-bench.sh --help for the options.

set -u -o pipefail

# ------------------------------- Settings -------------------------------

SCENARIOS_FILE="$(dirname "$0")/scenarios.txt"
FILTER="."
LIST_ONLY=0
SERVER_HOST=172.31.39.77
SERVER_PORT=6379
SERVER_SSH=""
SERVER_KEY=""
SERVER_USER=ubuntu
SERVER_DIR=/home/ubuntu/valkey-inline-compression/.build-release
SERVER_BIN=./bin/valkey-server
SERVER_LOG=/home/ubuntu/bench-valkey.log
SERVER_ALWAYS_ARGS="--save '' --protected-mode no --maxmemory-policy allkeys-lfu"
SB=/home/ubuntu/benchmark/target/release/sb
SB_DATASET_DIR=/home/ubuntu/benchmark/dataset
SB_CLIENT_ARGS="-c 512 --threads 16"
SB_SET_ARGS="-n 5000000 -t set --dataset github-events"
SB_GET_ARGS="-n 5000000 -t get --limit-rps 100000"
GET_TOUCH="10 30 50 100"
WATCH_SECONDS=600
WATCH_STEP=5
RESULTS_ROOT="$HOME/bench-results"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [OPTIONS]

Runs the scenarios in the scenarios file, one after the other, and writes the
results to <results>/<date-time>/. Each scenario restarts the server.

Scenarios:
  -s, --scenarios FILE      Scenarios file (default: $SCENARIOS_FILE)
  -f, --filter REGEX        Run only the scenarios whose name matches REGEX
                            (default: all)
  -l, --list                Print the scenarios that would run, and exit

Server:
      --server-host HOST    Server address (default: $SERVER_HOST)
      --server-port PORT    Server port (default: $SERVER_PORT)
      --server-user USER    SSH user on the server (default: $SERVER_USER)
      --server-key FILE     SSH key for the server (default: the default
                            keys of ssh)
      --server-ssh CMD      Full SSH command for the server. Replaces
                            --server-user and --server-key.
      --server-dir DIR      Directory to start valkey-server in
                            (default: $SERVER_DIR)
      --server-bin PATH     valkey-server, relative to --server-dir
                            (default: $SERVER_BIN)
      --server-log FILE     Server log file (default: $SERVER_LOG)
      --server-args ARGS    Arguments for every start, before the
                            scenario's arguments
                            (default: $SERVER_ALWAYS_ARGS)

Client (sb):
      --sb PATH             sb binary (default: $SB)
      --dataset-dir DIR     sb dataset directory (default: $SB_DATASET_DIR)
      --sb-client-args ARGS Connections and threads
                            (default: $SB_CLIENT_ARGS)
      --set-args ARGS       sb arguments for the load
                            (default: $SB_SET_ARGS)
      --get-args ARGS       sb arguments for each GET run
                            (default: $SB_GET_ARGS)
      --get-touch LIST      Touch percentages for the GET runs, 100 means all
                            keys (default: "$GET_TOUCH")

Run:
  -w, --watch-seconds N     How long to watch the compression after the load
                            (default: $WATCH_SECONDS)
      --watch-step N        Seconds between two CSV rows (default: $WATCH_STEP)
  -o, --results DIR         Results root directory (default: $RESULTS_ROOT)
  -h, --help                Print this help, and exit

Examples:
  # A short test of two scenarios:
  $(basename "$0") -f '^(baseline-off|cursor-base)\$' -w 30 --get-touch "10 100" \\
      --get-args "-n 200000 -t get --limit-rps 100000"

  # The full run, in the background:
  nohup ./$(basename "$0") > bench.log 2>&1 &
USAGE
}

die() {
    echo "$(basename "$0"): $*" >&2
    echo "Run $(basename "$0") --help for the options." >&2
    exit 1
}

ALL_ARGS="$*"
while [ $# -gt 0 ]; do
    opt="$1"
    case "$opt" in
    -h | --help) usage; exit 0 ;;
    -l | --list) LIST_ONLY=1; shift; continue ;;
    esac
    [ $# -ge 2 ] || die "$opt needs a value"
    case "$opt" in
    -s | --scenarios) SCENARIOS_FILE="$2" ;;
    -f | --filter) FILTER="$2" ;;
    --server-host) SERVER_HOST="$2" ;;
    --server-port) SERVER_PORT="$2" ;;
    --server-user) SERVER_USER="$2" ;;
    --server-key) SERVER_KEY="$2" ;;
    --server-ssh) SERVER_SSH="$2" ;;
    --server-dir) SERVER_DIR="$2" ;;
    --server-bin) SERVER_BIN="$2" ;;
    --server-log) SERVER_LOG="$2" ;;
    --server-args) SERVER_ALWAYS_ARGS="$2" ;;
    --sb) SB="$2" ;;
    --dataset-dir) SB_DATASET_DIR="$2" ;;
    --sb-client-args) SB_CLIENT_ARGS="$2" ;;
    --set-args) SB_SET_ARGS="$2" ;;
    --get-args) SB_GET_ARGS="$2" ;;
    --get-touch) GET_TOUCH="$2" ;;
    -w | --watch-seconds) WATCH_SECONDS="$2" ;;
    --watch-step) WATCH_STEP="$2" ;;
    -o | --results) RESULTS_ROOT="$2" ;;
    *) die "unknown option: $opt" ;;
    esac
    shift 2
done

if [ -z "$SERVER_SSH" ]; then
    SERVER_SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=4"
    [ -n "$SERVER_KEY" ] && SERVER_SSH="$SERVER_SSH -i $SERVER_KEY"
    SERVER_SSH="$SERVER_SSH $SERVER_USER@$SERVER_HOST"
fi
SB_COMMON="$SB_CLIENT_ARGS -h $SERVER_HOST -p $SERVER_PORT"
SB_SET_ARGS="$SB_SET_ARGS --dataset-dir $SB_DATASET_DIR"
SERVER_ALWAYS_ARGS="--port $SERVER_PORT $SERVER_ALWAYS_ARGS"

# ------------------------------- Helpers --------------------------------

# A small RESP client: vk CMD [ARG ...]. Prints the reply. Errors go to
# stderr and give exit code 1. The client machine has no valkey-cli.
read -r -d '' RESP_CLIENT <<'PY'
import socket, sys
host, port, args = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
s = socket.create_connection((host, port), timeout=30)
req = b"*%d\r\n" % len(args) + b"".join(b"$%d\r\n%s\r\n" % (len(a.encode()), a.encode()) for a in args)
s.sendall(req)
f = s.makefile("rb")
def read():
    line = f.readline()
    if not line:
        raise SystemExit("connection closed")
    t, rest = line[:1], line[1:-2]
    if t == b"+":
        return rest.decode()
    if t == b"-":
        sys.stderr.write(rest.decode() + "\n")
        raise SystemExit(1)
    if t == b":":
        return rest.decode()
    if t == b"$":
        n = int(rest)
        if n < 0:
            return ""
        data = f.read(n + 2)[:-2]
        return data.decode(errors="replace")
    if t == b"*":
        n = int(rest)
        return "\n".join(str(read()) for _ in range(max(n, 0)))
    raise SystemExit("bad reply: %r" % line)
print(read())
PY

vk() {
    python3 -c "$RESP_CLIENT" "$SERVER_HOST" "$SERVER_PORT" "$@"
}

log() {
    echo "[$(date '+%F %T')] $*"
}

# Reads one field from an INFO reply on stdin.
info_field() {
    tr -d '\r' | awk -F: -v k="$1" '$1 == k { print $2; exit }'
}

stop_server() {
    vk SHUTDOWN NOSAVE >/dev/null 2>&1
    # Also stops a server that does not answer, or an old one.
    $SERVER_SSH "pkill -x valkey-server; for i in \$(seq 1 50); do pgrep -x valkey-server >/dev/null || exit 0; sleep 0.2; done; pkill -9 -x valkey-server" >/dev/null 2>&1
    return 0
}

# Starts valkey-server with the given arguments and waits for PING. Returns 1
# when it does not answer in 60 s.
start_server() {
    local args="$1"
    $SERVER_SSH "cd $SERVER_DIR && : > $SERVER_LOG && $SERVER_BIN $SERVER_ALWAYS_ARGS $args --daemonize yes --logfile $SERVER_LOG" || return 1
    for _ in $(seq 1 120); do
        [ "$(vk PING 2>/dev/null)" = "PONG" ] && return 0
        sleep 0.5
    done
    return 1
}

# Runs sb with the given arguments, and saves its JSON report in $1.
run_sb() {
    local out="$1"
    shift
    # shellcheck disable=SC2086
    "$SB" $SB_COMMON "$@" --json > "$out" 2> "$out.stderr"
}

# CSV header and one row of the compression watch.
WATCH_FIELDS="compressed_values compressed_values_original_bytes compressed_values_bytes compression_saved_bytes compression_jobs_in_flight compression_sweeper_paused compression_temporary_copies compression_total_keys_checked compression_total_keys_eligible compression_total_keys_skipped_already_compressed compression_total_keys_skipped_not_string compression_total_keys_skipped_in_use compression_total_keys_skipped_size compression_total_keys_skipped_hot compression_total_keys_skipped_already_queued compression_total_jobs_queued compression_total_values_compressed compression_total_values_dropped compression_total_sweeper_passes compression_total_sweeper_backoffs compression_total_compression_time_us compression_total_values_decompressed compression_total_decompression_time_us used_memory used_memory_rss used_cpu_sys used_cpu_user used_cpu_sys_main_thread used_cpu_user_main_thread"

watch_header() {
    echo "elapsed_s $WATCH_FIELDS" | tr ' ' ','
}

watch_row() {
    local elapsed="$1" info row f v
    info=$(vk INFO everything 2>/dev/null | tr -d '\r')
    row="$elapsed"
    for f in $WATCH_FIELDS; do
        v=$(echo "$info" | awk -F: -v k="$f" '$1 == k { print $2; exit }')
        row="$row,${v:-}"
    done
    echo "$row"
}

# ------------------------------ Scenarios -------------------------------

[ -f "$SCENARIOS_FILE" ] || die "no scenarios file: $SCENARIOS_FILE"

# Prints the scenarios that match the filter, one per line:
# name|startup arguments|runtime settings. Empty lines and lines that start
# with # are skipped.
selected_scenarios() {
    local name start_args runtime
    while IFS='|' read -r name start_args runtime || [ -n "$name" ]; do
        name=$(echo "$name" | xargs)
        [ -z "$name" ] && continue
        case "$name" in \#*) continue ;; esac
        echo "$name" | grep -Eq -- "$FILTER" || continue
        echo "$name|$(echo "${start_args:-}" | xargs)|$(echo "${runtime:-}" | xargs)"
    done < "$SCENARIOS_FILE"
}

if [ "$LIST_ONLY" = 1 ]; then
    selected_scenarios | awk -F'|' '{ printf "%-20s start:   %s\n%-20s runtime: %s\n", $1, $2, "", $3 }'
    exit 0
fi
[ -n "$(selected_scenarios)" ] || die "no scenario matches: $FILTER"
command -v python3 >/dev/null || die "python3 is needed"
[ -x "$SB" ] || die "sb not found: $SB"
$SERVER_SSH true < /dev/null || die "can't SSH to the server: $SERVER_SSH"

RUN_DIR="$RESULTS_ROOT/$(date '+%Y%m%d-%H%M%S')"
mkdir -p "$RUN_DIR"
cp "$SCENARIOS_FILE" "$RUN_DIR/scenarios.txt"
echo "$0 $ALL_ARGS" > "$RUN_DIR/command.txt"
cp "$0" "$RUN_DIR/compression-bench.sh"
log "Results in $RUN_DIR"

# One scenario per line, from selected_scenarios(). The list is read on fd 3:
# ssh and sb read stdin, and would eat the lines.
while IFS='|' read -r -u 3 name start_args runtime; do
    dir="$RUN_DIR/$name"
    mkdir -p "$dir"
    log "== $name: start: $start_args | runtime: $runtime"
    echo "$start_args" > "$dir/start-args.txt"
    echo "$runtime" > "$dir/runtime.txt"

    stop_server
    if ! start_server "$start_args"; then
        log "$name: the server did not start, skipped"
        $SERVER_SSH "cat $SERVER_LOG" > "$dir/valkey.log" 2>&1
        continue
    fi

    # Runtime settings, two words at a time.
    set -- $runtime
    while [ $# -ge 2 ]; do
        vk CONFIG SET "$1" "$2" >/dev/null || log "$name: CONFIG SET $1 $2 failed"
        shift 2
    done
    vk CONFIG RESETSTAT >/dev/null
    vk INFO everything > "$dir/info-start.txt"

    log "$name: SET"
    run_sb "$dir/set.json" $SB_SET_ARGS
    vk INFO everything > "$dir/info-after-set.txt"

    log "$name: watching compression for $WATCH_SECONDS s"
    watch_header > "$dir/compression.csv"
    start=$(date +%s)
    while :; do
        elapsed=$(( $(date +%s) - start ))
        watch_row "$elapsed" >> "$dir/compression.csv"
        [ "$elapsed" -ge "$WATCH_SECONDS" ] && break
        sleep "$WATCH_STEP"
    done
    vk INFO everything > "$dir/info-after-watch.txt"

    for touch in $GET_TOUCH; do
        log "$name: GET, touch $touch%"
        if [ "$touch" = "100" ]; then
            run_sb "$dir/get-touch-$touch.json" $SB_GET_ARGS
        else
            run_sb "$dir/get-touch-$touch.json" $SB_GET_ARGS --touch-keys "$touch"
        fi
        vk INFO everything > "$dir/info-after-get-touch-$touch.txt"
    done

    $SERVER_SSH "cat $SERVER_LOG" > "$dir/valkey.log" 2>&1
    log "$name: done"
done 3< <(selected_scenarios)

stop_server

# ------------------------------- Summary --------------------------------

log "Writing $RUN_DIR/summary.csv"
python3 - "$RUN_DIR" "$GET_TOUCH" <<'PY'
import csv, json, os, sys

run_dir, touches = sys.argv[1], sys.argv[2].split()

def info(path):
    out = {}
    try:
        for line in open(path):
            if ":" in line and not line.startswith("#"):
                k, v = line.strip().split(":", 1)
                out[k] = v
    except OSError:
        pass
    return out

def num(d, k):
    try:
        return float(d.get(k, ""))
    except ValueError:
        return None

def report(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return None

def rows(path):
    try:
        return list(csv.DictReader(open(path)))
    except OSError:
        return []

names = sorted(d for d in os.listdir(run_dir) if os.path.isdir(os.path.join(run_dir, d)))
order = [l.split("|")[0].strip() for l in open(os.path.join(run_dir, "scenarios.txt"))
         if l.strip() and not l.strip().startswith("#")]
names = [n for n in order if n in names]

# The best compressed_values of the run is the reference for the 50/90/99%
# times, so all scenarios are compared to the same number.
watch = {n: rows(os.path.join(run_dir, n, "compression.csv")) for n in names}
best = max([float(r["compressed_values"] or 0) for n in names for r in watch[n]] or [0])

def time_to(n, pct):
    for r in watch[n]:
        if best > 0 and float(r["compressed_values"] or 0) >= best * pct / 100:
            return r["elapsed_s"]
    return "not reached"

head = ["scenario", "set_rps", "set_p50_ms", "set_p99_ms", "set_p999_ms",
        "used_memory_after_set", "used_memory_after_watch", "compressed_values", "compression_saved_bytes",
        "t50_s", "t90_s", "t99_s", "keys_checked", "keys_eligible", "sweeper_passes", "sweeper_backoffs",
        "main_thread_cpu_s_during_watch", "total_cpu_s_during_watch", "compression_time_s"]
for t in touches:
    head += ["get%s_rps" % t, "get%s_p50_ms" % t, "get%s_p99_ms" % t, "get%s_p999_ms" % t, "get%s_hits" % t]
head += ["values_decompressed_by_gets", "used_memory_end"]

with open(os.path.join(run_dir, "summary.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(head)
    for n in names:
        d = os.path.join(run_dir, n)
        s = report(os.path.join(d, "set.json")) or {}
        a, b = info(os.path.join(d, "info-after-set.txt")), info(os.path.join(d, "info-after-watch.txt"))
        lat = s.get("latency_ms", {})
        def delta(k):
            x, y = num(a, k), num(b, k)
            return "" if x is None or y is None else round(y - x, 2)
        main_cpu = ""
        if delta("used_cpu_sys_main_thread") != "" and delta("used_cpu_user_main_thread") != "":
            main_cpu = round(delta("used_cpu_sys_main_thread") + delta("used_cpu_user_main_thread"), 2)
        total_cpu = ""
        if delta("used_cpu_sys") != "" and delta("used_cpu_user") != "":
            total_cpu = round(delta("used_cpu_sys") + delta("used_cpu_user"), 2)
        ct = num(b, "compression_total_compression_time_us")
        row = [n, s.get("rps", ""), lat.get("p50", ""), lat.get("p99", ""), lat.get("p999", ""),
               a.get("used_memory", ""), b.get("used_memory", ""), b.get("compressed_values", ""),
               b.get("compression_saved_bytes", ""), time_to(n, 50), time_to(n, 90), time_to(n, 99),
               b.get("compression_total_keys_checked", ""), b.get("compression_total_keys_eligible", ""),
               b.get("compression_total_sweeper_passes", ""), b.get("compression_total_sweeper_backoffs", ""),
               main_cpu, total_cpu, "" if ct is None else round(ct / 1e6, 2)]
        for t in touches:
            g = report(os.path.join(d, "get-touch-%s.json" % t)) or {}
            gl = g.get("latency_ms", {})
            row += [g.get("rps", ""), gl.get("p50", ""), gl.get("p99", ""), gl.get("p999", ""), g.get("total_hits", "")]
        e = info(os.path.join(d, "info-after-get-touch-%s.txt" % touches[-1])) if touches else {}
        dec_a, dec_e = num(b, "compression_total_values_decompressed"), num(e, "compression_total_values_decompressed")
        row += ["" if dec_a is None or dec_e is None else int(dec_e - dec_a), e.get("used_memory", "")]
        w.writerow(row)
PY
log "Done. Summary: $RUN_DIR/summary.csv"
