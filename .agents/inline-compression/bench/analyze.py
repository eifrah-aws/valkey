#!/usr/bin/env python3
"""Checks a compression-bench.sh run, and prints the numbers for the report.

It reads only the raw files of each scenario (sb JSON reports, INFO
snapshots, compression.csv), computes every metric again, and compares the
result with summary.csv. It also checks that the run did what the scenario
asked for, and that the INFO counters add up. Exit code 1 when a check fails.

Usage: analyze.py RESULTS_DIR [--markdown FILE]

With --markdown, it also writes the result tables into FILE, between the
lines "<!-- tables:start -->" and "<!-- tables:end -->". All numbers in the
tables come from the raw files, as sb and INFO report them.
"""

import csv
import json
import os
import sys

TOUCHES = ["10", "30", "50", "100"]
REQUESTED = 5000000
CONNECTIONS = 512
# sb gives each connection REQUESTED / CONNECTIONS requests, rounded down.
EXPECTED_REQUESTS = (REQUESTED // CONNECTIONS) * CONNECTIONS
WATCH_SECONDS = 600

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
    return ok


def info(path):
    out = {}
    for line in open(path):
        line = line.strip()
        if ":" in line and not line.startswith("#"):
            k, v = line.split(":", 1)
            out[k] = v
    return out


def num(d, k):
    return float(d[k])


def scenarios(run_dir):
    out = []
    for line in open(os.path.join(run_dir, "scenarios.txt")):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, start, runtime = [x.strip() for x in line.split("|")]
        out.append((name, " ".join(start.split()), runtime.split()))
    return out


def main(run_dir, md_path=None):
    summary = {r["scenario"]: r for r in csv.DictReader(open(os.path.join(run_dir, "summary.csv")))}
    data = {}

    for name, start, runtime in scenarios(run_dir):
        d = os.path.join(run_dir, name)
        if not check(os.path.isdir(d), f"{name}: no result directory"):
            continue
        settings = dict(zip(runtime[0::2], runtime[1::2]))
        compressed = "--compression-mode lz4" in start

        # The run used the startup arguments of the scenario.
        check(open(os.path.join(d, "start-args.txt")).read().split() == start.split(),
              f"{name}: start-args.txt does not match scenarios.txt")
        i_start = info(os.path.join(d, "info-start.txt"))
        check(i_start["compression_mode"] == ("lz4" if compressed else "off"), f"{name}: wrong compression_mode")
        if compressed:
            ct = start.split("--compression-threads")[1].split()[0]
            check(i_start["compression_threads"] == ct, f"{name}: wrong compression_threads")
        check(i_start.get("redis_git_sha1", "") == open(os.path.join(run_dir, "baseline-off", "info-start.txt")).read().split("redis_git_sha1:")[1].split()[0],
              f"{name}: a different build than the baseline")

        # sb reports: all requests ran, with the right options.
        s = json.load(open(os.path.join(d, "set.json")))
        check(s["total_requests"] == EXPECTED_REQUESTS, f"{name}: SET did not run all requests")
        check(s["options"]["test"] == "set" and s["options"]["dataset"] == "github-events", f"{name}: wrong SET options")
        gets = {}
        for t in TOUCHES:
            g = json.load(open(os.path.join(d, f"get-touch-{t}.json")))
            check(g["total_requests"] == EXPECTED_REQUESTS, f"{name}: GET {t}% did not run all requests")
            want = None if t == "100" else int(t)
            check(g["options"]["touch_keys"] == want, f"{name}: GET {t}% has touch_keys {g['options']['touch_keys']}")
            check(g["options"]["limit_rps"] == 100000, f"{name}: GET {t}% has no 100k rps limit")
            check(g["total_hits"] >= 0.99 * g["total_requests"], f"{name}: GET {t}% has many misses")
            gets[t] = g

        a = info(os.path.join(d, "info-after-set.txt"))
        b = info(os.path.join(d, "info-after-watch.txt"))
        e = info(os.path.join(d, "info-after-get-touch-100.txt"))
        rows = list(csv.DictReader(open(os.path.join(d, "compression.csv"))))
        check(int(rows[-1]["elapsed_s"]) >= WATCH_SECONDS, f"{name}: the watch was shorter than {WATCH_SECONDS} s")

        if compressed:
            # The policy ran: only cursor counts passes.
            policy = settings.get("compression-sweeper-policy")
            passes = num(b, "compression_total_sweeper_passes")
            check((passes > 0) == (policy == "cursor") or num(b, "compressed_values") < 600000,
                  f"{name}: sweeper passes {passes} do not match policy {policy}")
            if policy == "random":
                check(passes == 0, f"{name}: random policy counted passes")
            if settings.get("compression-sweeper-backoff-seconds") == "0":
                check(num(b, "compression_total_sweeper_backoffs") == 0, f"{name}: backoff 0 but pauses counted")
            # The INFO counters add up (design doc section 9.1), at every snapshot.
            for label, snap in [("after-watch", b), ("end", e)]:
                skipped = sum(num(snap, k) for k in [
                    "compression_total_keys_skipped_already_compressed", "compression_total_keys_skipped_not_string",
                    "compression_total_keys_skipped_in_use", "compression_total_keys_skipped_size",
                    "compression_total_keys_skipped_hot"])
                check(num(snap, "compression_total_keys_checked") == num(snap, "compression_total_keys_eligible") + skipped,
                      f"{name} {label}: checked != eligible + skipped")
                check(num(snap, "compression_total_keys_eligible") == num(snap, "compression_total_jobs_queued") +
                      num(snap, "compression_total_keys_skipped_already_queued"),
                      f"{name} {label}: eligible != queued + already queued")
                check(num(snap, "compression_total_jobs_queued") == num(snap, "compression_total_values_compressed") +
                      num(snap, "compression_total_values_dropped") + num(snap, "compression_jobs_in_flight"),
                      f"{name} {label}: queued != compressed + dropped + in flight")
                check(num(snap, "compression_saved_bytes") == num(snap, "compressed_values_original_bytes") -
                      num(snap, "compressed_values_bytes"), f"{name} {label}: saved != original - frames")
            # No client reads during the watch, so the count only goes up.
            vals = [int(r["compressed_values"]) for r in rows]
            check(all(x <= y for x, y in zip(vals, vals[1:])), f"{name}: compressed_values went down during the watch")
            check(num(b, "compression_total_values_decompressed") == 0, f"{name}: values were decompressed during the watch")
        else:
            check(num(b, "compressed_values") == 0, f"{name}: baseline has compressed values")

        get_info = {t: info(os.path.join(d, f"info-after-get-touch-{t}.txt")) for t in TOUCHES}
        data[name] = dict(start=start, settings=settings, compressed=compressed, set=s, gets=gets,
                          a=a, b=b, e=e, rows=rows, get_info=get_info)

    # Times to 50/90/99% of the best compressed_values of the run.
    best = max(int(r["compressed_values"]) for v in data.values() for r in v["rows"])

    def time_to(v, pct):
        for r in v["rows"]:
            if int(r["compressed_values"]) >= best * pct / 100:
                return r["elapsed_s"]
        return "not reached"

    # Compare with summary.csv, field by field.
    for name, v in data.items():
        srow = summary.get(name)
        if not check(srow is not None, f"{name}: missing in summary.csv"):
            continue
        a, b, e = v["a"], v["b"], v["e"]
        mine = {
            "set_rps": v["set"]["rps"], "set_p99_ms": v["set"]["latency_ms"]["p99"],
            "used_memory_after_set": a["used_memory"], "used_memory_after_watch": b["used_memory"],
            "compressed_values": b["compressed_values"], "compression_saved_bytes": b["compression_saved_bytes"],
            "t50_s": time_to(v, 50), "t90_s": time_to(v, 90), "t99_s": time_to(v, 99),
            "keys_checked": b["compression_total_keys_checked"], "sweeper_backoffs": b["compression_total_sweeper_backoffs"],
            "used_memory_end": e["used_memory"],
            "values_decompressed_by_gets": int(num(e, "compression_total_values_decompressed") -
                                               num(b, "compression_total_values_decompressed")),
        }
        for t in TOUCHES:
            mine[f"get{t}_p99_ms"] = v["gets"][t]["latency_ms"]["p99"]
            mine[f"get{t}_p999_ms"] = v["gets"][t]["latency_ms"]["p999"]
            mine[f"get{t}_rps"] = v["gets"][t]["rps"]
        for k, x in mine.items():
            check(str(x) == srow[k] or (srow[k] and float(srow[k]) == float(x)),
                  f"{name}: summary.csv {k}={srow[k]}, raw files give {x}")

    report(data, best)
    if md_path:
        write_markdown(md_path, markdown_tables(data, best))
        print()
        print(f"Wrote the tables to {md_path}")
    print()
    if failures:
        print(f"FAILED: {len(failures)} check(s)")
        for f in failures:
            print("  -", f)
        return 1
    print(f"OK: all checks passed for {len(data)} scenarios")
    return 0


def report(data, best):
    base = data["baseline-off"]
    base_mem = num(base["a"], "used_memory")
    print(f"Best compressed_values in the run: {best}")
    print()
    print(f"{'scenario':20} {'mem after watch':>15} {'vs base':>8} {'mem end':>8} {'t50':>5} {'t90':>5} {'t99':>5} "
          f"{'checked':>9} {'main cpu':>8} {'worker s':>8} {'dec by GET':>10}")
    for name, v in data.items():
        b, e = v["b"], v["e"]
        mem = num(b, "used_memory")
        rows = v["rows"]

        def t(p):
            for r in rows:
                if int(r["compressed_values"]) >= best * p / 100:
                    return r["elapsed_s"]
            return "-"
        main_cpu = (num(b, "used_cpu_sys_main_thread") + num(b, "used_cpu_user_main_thread") -
                    num(v["a"], "used_cpu_sys_main_thread") - num(v["a"], "used_cpu_user_main_thread"))
        worker = float(b.get("compression_total_compression_time_us", 0)) / 1e6
        dec = float(e.get("compression_total_values_decompressed", 0)) - float(b.get("compression_total_values_decompressed", 0))
        print(f"{name:20} {mem / 1e9:13.2f}GB {100 * (mem - base_mem) / base_mem:7.1f}% {num(e, 'used_memory') / 1e9:6.2f}GB "
              f"{t(50):>5} {t(90):>5} {t(99):>5} {b.get('compression_total_keys_checked', '0'):>9} {main_cpu:8.2f} "
              f"{worker:8.2f} {int(dec):>10}")
    print()
    print("SET rps / p99 ms, and GET p99 / p999 ms (ratio of p99 to the baseline with the same io-threads)")
    print(f"{'scenario':20} {'SET rps':>8} {'p99':>6}  " + "  ".join(f"{'GET ' + t + '%':>22}" for t in TOUCHES))
    for name, v in data.items():
        io = v["start"].split("--io-threads")[1].split()[0]
        ref = {"12": "baseline-off", "6": "baseline-off-io6", "24": "baseline-off-io24"}[io]
        cells = []
        for t in TOUCHES:
            lat = v["gets"][t]["latency_ms"]
            rp99 = lat["p99"] / data[ref]["gets"][t]["latency_ms"]["p99"]
            low = " rps=%d" % v["gets"][t]["rps"] if v["gets"][t]["rps"] < 99000 else ""
            cells.append(f"{lat['p99']:5.2f}/{lat['p999']:5.2f} x{rp99:4.1f}{low}")
        print(f"{name:20} {v['set']['rps']:8d} {v['set']['latency_ms']['p99']:6.2f}  " + "  ".join(f"{c:>22}" for c in cells))

    # Run-to-run spread: these scenarios change only things that should not
    # change the GET path much, so their spread shows the noise.
    same = [n for n in ["cursor-base", "cursor-ct16", "cursor-if10000", "cursor-io24"] if n in data]
    print()
    for t in TOUCHES:
        vals = [data[n]["gets"][t]["latency_ms"]["p99"] for n in same]
        print(f"Noise check, GET {t}% p99 over {', '.join(same)}: min {min(vals):.2f}, max {max(vals):.2f} ms")


PCTS = ["pmin", "p50", "p90", "p95", "p99", "p995", "p999", "pmax"]


def scenario_settings(v):
    args = v["start"].split()

    def arg(name, default="-"):
        return args[args.index(name) + 1] if name in args else default
    st = v["settings"]
    return [arg("--io-threads"), arg("--compression-mode", "off"), arg("--compression-threads"),
            arg("--compression-max-inflight-requests"), st.get("compression-sweeper-policy", "-"),
            st.get("compression-sweeper-samples", "-"), st.get("compression-sweeper-backoff-seconds", "-")]


def gib(v):
    """Bytes as GiB (1024^3 bytes, like INFO used_memory_human), 2 digits."""
    return f"{float(v) / 1024 ** 3:.2f} GiB"


def md_table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def markdown_tables(data, best):
    names = list(data)
    parts = []

    parts.append("### Scenarios\n\nEach scenario changes one setting from cursor-base. "
                 "All runs also pass `--save '' --protected-mode no --maxmemory-policy allkeys-lfu`.\n")
    parts.append(md_table(["Scenario", "io-threads", "compression-mode", "compression-threads",
                           "max-inflight-requests", "sweeper-policy", "sweeper-samples", "backoff-seconds"],
                          [[f"`{n}`"] + scenario_settings(data[n]) for n in names]))

    def t(v, p):
        for r in v["rows"]:
            if int(r["compressed_values"]) >= best * p / 100:
                return r["elapsed_s"]
        return "-"
    base_mem = num(data["baseline-off"]["a"], "used_memory")
    rows = []
    for n in names:
        a, b, e = data[n]["a"], data[n]["b"], data[n]["e"]
        mem = num(b, "used_memory")
        rows.append([f"`{n}`", gib(a["used_memory"]), gib(b["used_memory"]), f"{100 * (mem - base_mem) / base_mem:.1f}%",
                     b.get("compressed_values", "0"), gib(b.get("compressed_values_original_bytes", 0)),
                     gib(b.get("compressed_values_bytes", 0)), gib(b.get("compression_saved_bytes", 0)),
                     gib(e["used_memory"])])
    parts.append("### Memory\n\nIn GiB (1 GiB = 1024^3 bytes, the unit of INFO `used_memory_human`). `used_memory` "
                 "after the SET load, after the 10-minute watch, and at the end (after the 4 GET runs). The change "
                 "is against `baseline-off` after SET, computed from the bytes. The `compressed_*` columns are "
                 "after the watch.\n")
    parts.append(md_table(["Scenario", "used_memory after SET", "used_memory after watch", "change",
                           "compressed_values", "compressed_values_original_bytes", "compressed_values_bytes",
                           "compression_saved_bytes", "used_memory at end"], rows))

    rows = []
    for n in names:
        v = data[n]
        a, b, e = v["a"], v["b"], v["e"]
        main_cpu = (num(b, "used_cpu_sys_main_thread") + num(b, "used_cpu_user_main_thread") -
                    num(a, "used_cpu_sys_main_thread") - num(a, "used_cpu_user_main_thread"))
        total_cpu = num(b, "used_cpu_sys") + num(b, "used_cpu_user") - num(a, "used_cpu_sys") - num(a, "used_cpu_user")
        worker = float(b.get("compression_total_compression_time_us", 0)) / 1e6
        dec = int(float(e.get("compression_total_values_decompressed", 0)) -
                  float(b.get("compression_total_values_decompressed", 0)))
        rows.append([f"`{n}`", t(v, 50), t(v, 90), t(v, 99), b.get("compression_total_keys_checked", "0"),
                     b.get("compression_total_keys_eligible", "0"), b.get("compression_total_sweeper_passes", "0"),
                     b.get("compression_total_sweeper_backoffs", "0"), f"{main_cpu:.2f}", f"{total_cpu:.2f}",
                     f"{worker:.2f}", dec])
    parts.append(f"### Sweeper, during the 10-minute watch\n\nt50/t90/t99: seconds after the end of the SET load "
                 f"until `compressed_values` reached 50/90/99% of {best} (the best value of the run); \"-\" means "
                 f"not reached in 600 s. CPU is in seconds, from INFO. The last column counts the decompressions of "
                 f"all 4 GET runs.\n")
    parts.append(md_table(["Scenario", "t50 s", "t90 s", "t99 s", "keys_checked", "keys_eligible", "passes",
                           "backoffs", "main-thread CPU s", "total CPU s", "worker compression s",
                           "values decompressed by GETs"], rows))

    rows = []
    for n in names:
        s = data[n]["set"]
        rows.append([f"`{n}`", s["rps"]] + [s["latency_ms"][p] for p in PCTS])
    parts.append("### SET load\n\n5M requests (4,999,680 after sb rounds to 512 connections), 1M keys, "
                 "`github-events` values. Latency in ms, as sb reports it.\n")
    parts.append(md_table(["Scenario", "rps"] + PCTS, rows))

    for touch in TOUCHES:
        rows = []
        for n in names:
            v = data[n]
            g = v["gets"][touch]
            prev = v["b"] if touch == TOUCHES[0] else v["get_info"][TOUCHES[TOUCHES.index(touch) - 1]]
            cur = v["get_info"][touch]
            dec = int(float(cur.get("compression_total_values_decompressed", 0)) -
                      float(prev.get("compression_total_values_decompressed", 0)))
            rows.append([f"`{n}`", g["rps"]] + [g["latency_ms"][p] for p in PCTS] +
                        [g["total_hits"], prev.get("compressed_values", "0"), dec])
        label = "all keys (no `--touch-keys`)" if touch == "100" else f"`--touch-keys {touch}` (the first {touch}% of the keys)"
        parts.append(f"### GET, {label}\n\n5M requests, `--limit-rps 100000`. Latency in ms, as sb reports it. "
                     f"\"compressed before\" is `compressed_values` when the run starts; \"decompressed\" is the "
                     f"number of values this run decompressed.\n")
        parts.append(md_table(["Scenario", "rps"] + PCTS + ["hits", "compressed before", "decompressed"], rows))
    return "\n\n".join(x.strip() for x in parts)


def write_markdown(path, tables):
    text = open(path).read()
    start, end = "<!-- tables:start -->", "<!-- tables:end -->"
    if start not in text or end not in text:
        raise SystemExit(f"{path}: no {start} / {end} lines")
    head, rest = text.split(start, 1)
    tail = rest.split(end, 1)[1]
    open(path, "w").write(head + start + "\n\n" + tables + "\n\n" + end + tail)


if __name__ == "__main__":
    args = sys.argv[1:]
    md = None
    if "--markdown" in args:
        i = args.index("--markdown")
        md = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(args[0], md))
