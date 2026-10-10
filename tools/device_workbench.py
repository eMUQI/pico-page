#!/usr/bin/env python3
"""Continuous, private evidence capture for the existing local PP diagnostic protocol."""
import argparse
from collections import Counter, defaultdict
import hashlib
import html
import json
import math
from pathlib import Path
import re
import select
import subprocess
import sys
import threading
import time

from device_console import Console, open_serial

OUTCOMES = ("success", "failure", "cancelled", "timeout")
REQUIRED_CONDITIONS = ("hardware", "tf", "books_sha256", "cache", "network")


def fields(value):
    return dict(part.split("=", 1) for part in value.split() if "=" in part)


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


class Session(Console):
    """One reader owns the serial stream; requests share a bounded response slot.

    All received bytes are written before parsing. No serial reopen on timeout:
    a disconnect terminates this session and is an explicit evidence gap.
    """
    def __init__(self, connection, output, timeout=10):
        self.serial, self.output, self.timeout = connection, Path(output), timeout
        self.output.mkdir(parents=True, exist_ok=False)
        self.raw = (self.output / "serial.bin").open("wb")
        self.chunks = (self.output / "chunks.jsonl").open("w")
        self.events = (self.output / "events.jsonl").open("w")
        self.marks = (self.output / "marks.jsonl").open("w")
        self.lock = threading.RLock()
        self.condition = threading.Condition(self.lock)
        self.command_lock = threading.Lock()
        self.identifier = 0
        self.waiting = None
        self.reply = None
        self.entries = []
        self.failure = None
        self.stop = threading.Event()
        self.gaps = []
        self.offset = 0
        self.thread = threading.Thread(target=self._read, name="serial-reader", daemon=True)
        self.thread.start()

    def event(self, kind, **values):
        with self.lock:
            item = dict(host_ns=time.monotonic_ns(), utc_ns=time.time_ns(), type=kind, **values)
            self.events.write(json.dumps(item, ensure_ascii=False) + "\n")
            self.events.flush()
            return item

    def gap(self, reason):
        with self.lock:
            self.gaps.append(reason)
            self.event("gap", reason=reason)

    def _read(self):
        pending = bytearray()
        discard = False
        try:
            while not self.stop.is_set():
                chunk = self.serial.read(min(4096, max(1, self.serial.in_waiting)))
                if not chunk:
                    continue
                with self.lock:
                    self.raw.write(chunk)
                    self.raw.flush()
                    self.chunks.write(json.dumps(dict(offset=self.offset, size=len(chunk),
                                                     host_ns=time.monotonic_ns(), utc_ns=time.time_ns())) + "\n")
                    self.chunks.flush()
                    self.offset += len(chunk)
                for byte in chunk:
                    if byte == 10:
                        if not discard:
                            self._line(pending.decode("utf-8", errors="replace").strip())
                        pending.clear()
                        discard = False
                    elif not discard:
                        pending.append(byte)
                        if len(pending) > 8192:
                            self.gap("serial_line_overflow")
                            pending.clear()
                            discard = True
        except Exception as exc:
            with self.condition:
                self.failure = str(exc)
                self.gap("disconnect_or_reader_failure: " + str(exc))
                self.condition.notify_all()
        finally:
            if pending or discard:
                self.gap("partial_serial_line_at_end")

    def _line(self, line):
        match = re.fullmatch(r"PP (\d+) (.*)", line)
        if not match:
            self.event("log", line=line)
            return
        identifier, value = int(match[1]), match[2]
        parsed = fields(value)
        self.event("device" if identifier == 0 else "reply", id=identifier, line=value, fields=parsed)
        with self.condition:
            if identifier != self.waiting:
                return
            if value.startswith("ENTRY "):
                if len(self.entries) == 32:
                    self.failure = "response_entry_overflow"
                    self.gap(self.failure)
                else:
                    self.entries.append(value)
            elif value == "OK" or value.startswith(("OK ", "ERROR ")):
                self.reply = value
            self.condition.notify_all()

    def request(self, command):
        with self.command_lock:
            with self.condition:
                self.identifier += 1
                data = f"PP {self.identifier} {command}\n".encode()
                if len(data) > 4224 or any(c < 32 or c == 127 for c in data[:-1]):
                    raise ValueError("Invalid diagnostic command")
                self.waiting, self.reply, self.entries = self.identifier, None, []
                deadline = time.monotonic() + self.timeout
                self.event("command", id=self.identifier, command=command)
                try:
                    if self.failure:
                        raise OSError(self.failure)
                    if self.serial.write(data) != len(data):
                        raise OSError("short serial write")
                    while self.reply is None and self.failure is None:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            self.event("command_timeout", id=self.identifier)
                            self.gap("command_timeout")
                            raise TimeoutError(command)
                        self.condition.wait(remaining)
                    if self.failure:
                        raise OSError(self.failure)
                    if self.reply.startswith("ERROR "):
                        raise RuntimeError(self.reply)
                    return fields(self.reply), list(self.entries)
                finally:
                    self.waiting = None

    def mark(self, description):
        item = self.event("mark", description=description[:512], window_seconds=10)
        with self.lock:
            self.marks.write(json.dumps(item, ensure_ascii=False) + "\n")
            self.marks.flush()

    def drain(self, max_batches=16):
        for _ in range(max_batches):
            state, _ = self.request("trace")
            if state.get("schema") != "1":
                raise RuntimeError("Unsupported trace schema")
            if int(state["queued"]) == 0:
                return state
        self.gap("trace_drain_budget_exhausted")

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            # The configured serial read timeout is 200 ms. Do not close evidence
            # files underneath a reader that violated that transport contract.
            raise OSError("serial reader did not stop")
        self.serial.close()
        for stream in (self.raw, self.chunks, self.events, self.marks):
            stream.close()


def refresh_check(before, after):
    """A driver confirmation plus independent state invariants, never a physical-screen claim."""
    if after.get("frame_ok") != "1" or after.get("purpose") != "2" or after.get("clean") != "1":
        raise AssertionError("K2 did not confirm a full manual refresh")
    for key in ("view", "revision", "page", "crc32"):
        if key not in before or key not in after or before[key] != after[key]:
            raise AssertionError(f"K2 changed or omitted {key}")


def refresh_scenario(session):
    deadline = time.monotonic() + session.timeout
    while True:
        before, _ = session.request("status")
        if before.get("view") != "reading":
            return dict(status="skip", reason="Open a synthetic test book before starting")
        if all(before.get(k) == "0" for k in ("pending", "content_busy", "save_busy", "save_due")) and before.get("saved") == "1":
            break
        if time.monotonic() >= deadline:
            raise TimeoutError("reading did not become quiescent")
        session.drain()
    session.drain()
    accepted, _ = session.request("key K2")
    if accepted.get("accepted") != "1":
        raise AssertionError("K2 was not accepted")
    deadline = time.monotonic() + session.timeout
    while time.monotonic() < deadline:
        after, _ = session.request("status")
        if int(after.get("frame", "0")) > int(before["frame"]) and after.get("pending") == "0":
            refresh_check(before, after)
            session.drain()
            session.capture(session.output / "canvas.pgm")
            return dict(status="pass", scope="driver_and_app_state_only", before=before, after=after)
        session.drain()
    raise TimeoutError("K2 frame was not independently confirmed")


def percentile(samples, p):
    return sorted(samples)[max(0, math.ceil(len(samples) * p) - 1)]


def analyze(output):
    """Conservative offline analysis. Missing starts/ends or drops invalidate percentiles."""
    active, counts, samples = {}, Counter(), defaultdict(list)
    gaps, last_seq, metric_active, failures = [], None, {}, 0
    event_count = 0
    last_queued = 0
    last_device_sequence = None
    dropped_baseline = tx_baseline = 0
    with (Path(output) / "events.jsonl").open() as stream:
        for line in stream:
            event = json.loads(line)
            if event["type"] == "baseline":
                # The connection's initial backlog remains in raw evidence but is outside
                # this measurement window. Active operations will need a new start event.
                active.clear()
                counts.clear()
                samples.clear()
                metric_active.clear()
                last_seq = event["sequence"]
                event_count = failures = 0
                gaps = [g for g in gaps if g not in ("device_trace_dropped", "device_tx_failures",
                        "trace_sequence_gap", "operation_start_missing", "device_restart_or_reinitialize")]
                dropped_baseline = event["dropped"]
                tx_baseline = event["tx_failures"]
            if event["type"] == "gap":
                gaps.append(event["reason"])
            if event["type"] == "command_timeout":
                counts["command_timeout"] += 1
            f = event.get("fields", {})
            if event["type"] == "reply":
                if event["line"].startswith("ERROR "):
                    counts["command_failure"] += 1
                if "dropped" in f and int(f["dropped"]) != dropped_baseline:
                    gaps.append("device_trace_dropped")
                if "tx_failures" in f and int(f["tx_failures"]) != tx_baseline:
                    gaps.append("device_tx_failures")
                if "queued" in f:
                    last_queued = int(f["queued"])
                if "sequence" in f:
                    last_device_sequence = int(f["sequence"])
                if "active" in f:
                    metric_active[f["kind"]] = int(f["active"])
            if event["type"] == "device" and event["line"].startswith("READY"):
                gaps.append("device_restart_or_reinitialize")
            if event["type"] == "device" and event["line"].startswith("FRAME") and f.get("success") == "0":
                failures += 1
            if "seq" not in f or "op" not in f:
                continue
            event_count += 1
            sequence, operation = int(f["seq"]), int(f["op"])
            if (sequence <= 0 or operation <= 0 or f["begin"] not in ("0", "1") or
                    not 0 <= int(f["kind"]) < 4 or not 0 <= int(f["result"]) < len(OUTCOMES) or
                    int(f["time_us"]) < 0 or int(f["duration_us"]) < 0):
                raise ValueError("Invalid structured event fields")
            if last_seq is not None and sequence != last_seq + 1:
                gaps.append("trace_sequence_gap")
            last_seq = sequence
            if f["begin"] == "1":
                if operation in active:
                    gaps.append("duplicate_operation")
                active[operation] = f
            else:
                start = active.pop(operation, None)
                if start is None:
                    gaps.append("operation_start_missing")
                elif any(start[k] != f[k] for k in ("kind", "session", "layout", "frame", "request", "detail")):
                    gaps.append("operation_context_changed")
                result = int(f["result"])
                counts[OUTCOMES[result]] += 1
                samples[f["kind"]].append(int(f["duration_us"]))
    if last_queued:
        gaps.append("undrained_events_at_end")
    if last_device_sequence is not None and last_seq is not None and last_device_sequence != last_seq:
        gaps.append("trace_tail_sequence_gap")
    counts["unfinished"] = max(len(active), sum(metric_active.values()))
    if counts["unfinished"]:
        gaps.append("operations_unfinished")
    distributions = {}
    for kind, values in samples.items():
        distributions[kind] = dict(n=len(values), max_us=max(values),
                                   p50_us=None if gaps else percentile(values, .5),
                                   p95_us=None if gaps else percentile(values, .95),
                                   p99_us=None if gaps else percentile(values, .99))
    completed = sum(counts[k] for k in OUTCOMES)
    return dict(counts={k: counts[k] for k in (*OUTCOMES, "unfinished", "command_failure", "command_timeout")},
                gaps=sorted(set(gaps)), distributions=distributions, events=event_count,
                frame_failures=failures, failure_rate=counts["failure"] / completed if completed else None)


def finalize(output, metadata, result):
    output = Path(output)
    try:
        analysis = analyze(output)
    except (KeyError, ValueError, TypeError) as exc:
        analysis = dict(gaps=["malformed_structured_evidence"], error=str(exc), counts={}, distributions={}, events=0)
        result = dict(status="infra_error", reason="Cannot parse complete structured evidence")
    if result["status"] == "pass":
        if analysis["gaps"] or not analysis["events"] or metadata.get("missing"):
            result = dict(status="inconclusive", reason="Incomplete evidence or run conditions", assertion=result)
        elif analysis["counts"]["failure"] or analysis["counts"]["timeout"] or analysis["counts"]["command_failure"] or analysis["frame_failures"]:
            result = dict(status="fail", reason="Failure observed during the run", assertion=result)
    run = dict(schema=1, metadata=metadata, result=result, analysis=analysis)
    write_json(output / "run.json", run)
    marks = [json.loads(line) for line in (output / "marks.jsonl").read_text().splitlines()]
    windows = []
    for mark in marks:
        nearby = []
        with (output / "events.jsonl").open() as events:
            for line in events:
                event = json.loads(line)
                if abs(event["host_ns"] - mark["host_ns"]) <= 10_000_000_000:
                    nearby.append(event)
        windows.append(dict(mark=mark, events=nearby))
    write_json(output / "mark-windows.json", windows)
    body = html.escape(json.dumps(run, ensure_ascii=False, indent=2))
    mark_html = html.escape(json.dumps(windows, ensure_ascii=False, indent=2))
    (output / "report.html").write_text('<!doctype html><meta charset="utf-8"><title>Pico Page test run</title>'
        '<style>body{font:16px system-ui;max-width:1000px;margin:2em auto}pre{white-space:pre-wrap}</style>'
        f'<h1>{html.escape(result["status"])}</h1><p>Application/driver evidence; physical display, touch and power untested.</p>'
        '<p><a href="run.json">run.json</a> · <a href="events.jsonl">events</a> · '
        '<a href="serial.bin">raw bytes</a> · <a href="chunks.jsonl">byte timestamps</a></p>'
        + ('<p><a href="canvas.pgm">captured canvas (not physical screen)</a></p>' if (output / 'canvas.pgm').exists() else '')
        + f'<pre>{body}</pre><h2>Issue markers ±10 seconds</h2><pre>{mark_html}</pre>')
    return run


def metadata_for(args):
    metadata = json.loads(args.metadata.read_text()) if args.metadata else {}
    metadata["source_commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    metadata["source_dirty"] = bool(subprocess.check_output(["git", "status", "--porcelain"], text=True).strip())
    metadata["host_platform"] = sys.platform
    metadata["mode"] = args.mode
    artifacts = {}
    if args.firmware:
        for name in ("pico_page.bin", "pico_page.elf", "sdkconfig"):
            path = args.firmware / name
            if path.is_file():
                artifacts[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        config = args.firmware / "sdkconfig"
        if config.exists():
            metadata["effective_sdkconfig"] = config.read_text()
    metadata["artifacts_sha256"] = artifacts
    metadata["missing"] = [k for k in REQUIRED_CONDITIONS if not metadata.get(k)]
    metadata["missing"] += [k for k in ("pico_page.bin", "pico_page.elf", "sdkconfig") if k not in artifacts]
    if not metadata.get("device_build_verified"):
        metadata["missing"].append("device_build_verified")
    return metadata


def compare(first, second):
    left, right = (json.loads(Path(p).read_text()) for p in (first, second))
    keys = (*REQUIRED_CONDITIONS, "source_dirty", "effective_sdkconfig", "mode")
    # Mode flags may differ for Release/Performance A/B; all other configuration must match.
    def conditions(run):
        m = run["metadata"].copy()
        mode_flags = ("CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE", "CONFIG_PICO_PAGE_PERF_METRICS",
                      "CONFIG_PICO_PAGE_TRACE_EVENTS", "CONFIG_PICO_PAGE_LEGACY_PERF_LOG")
        m["effective_sdkconfig"] = "\n".join(line for line in m.get("effective_sdkconfig", "").splitlines()
                                               if not any(flag in line for flag in mode_flags))
        return {k: m.get(k) for k in keys}
    comparable = (conditions(left) == conditions(right) and not left["metadata"].get("missing")
                  and not right["metadata"].get("missing") and not left["analysis"]["gaps"] and not right["analysis"]["gaps"]
                  and bool(left["analysis"]["distributions"]) and bool(right["analysis"]["distributions"])
                  and bool(left["metadata"].get("source_commit")) and bool(right["metadata"].get("source_commit"))
                  and left["metadata"].get("source_dirty") is False and right["metadata"].get("source_dirty") is False)
    return dict(comparable=comparable, reason="Conditions match" if comparable else "Conditions or evidence differ; do not infer a performance change",
                source_commits=[left["metadata"].get("source_commit"), right["metadata"].get("source_commit")],
                same_source=left["metadata"].get("source_commit") == right["metadata"].get("source_commit"),
                first=left["analysis"], second=right["analysis"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare", nargs=2, metavar=("BASE_RUN", "NEW_RUN"))
    parser.add_argument("--port")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--metadata", type=Path, help="Private JSON: hardware, tf, books_sha256, cache, network, device_build_verified")
    parser.add_argument("--firmware", type=Path, help="Build directory containing bin, ELF and effective sdkconfig")
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--mode", choices=("manual", "refresh"), default="manual")
    args = parser.parse_args()
    if args.compare:
        print(json.dumps(compare(*args.compare), ensure_ascii=False, indent=2))
        return 0
    if not args.port or not args.output or args.timeout <= 0:
        parser.error("--port, --output and a positive --timeout are required")
    if args.output.exists():
        parser.error("--output must be a new private directory")
    metadata = metadata_for(args)
    result = dict(status="inconclusive", reason="Manual observation has no automatic functional assertion")
    session = None
    try:
        session = Session(open_serial(args.port), args.output, args.timeout)
        session.event("start", mode=args.mode)
        session.request("status")
        baseline = session.drain()
        if baseline is None:
            raise RuntimeError("Cannot establish trace baseline")
        session.event("baseline", dropped=int(baseline["dropped"]), tx_failures=int(baseline["tx_failures"]),
                      sequence=int(baseline["sequence"]))
        if args.mode == "refresh":
            result = refresh_scenario(session)
        else:
            print("Recording. Enter 'mark short description' to mark a problem; 'end' to finish.")
            while True:
                ready, _, _ = select.select([sys.stdin], [], [], .25)
                if ready:
                    command = sys.stdin.readline()
                    if not command or command.strip() == "end":
                        break
                    if command.startswith("mark"):
                        session.mark(command[4:].strip())
                session.drain()
        session.drain()
        session.request("metrics")
    except AssertionError as exc:
        result = dict(status="fail", reason=str(exc))
    except TimeoutError as exc:
        result = dict(status="inconclusive", reason="Timeout: " + str(exc))
    except KeyboardInterrupt:
        result = dict(status="inconclusive", reason="Interrupted")
    except (OSError, RuntimeError, ValueError, ImportError) as exc:
        result = dict(status="infra_error", reason=str(exc))
    finally:
        if session:
            session.event("end")
            session.close()
            run = finalize(args.output, metadata, result)
            print(json.dumps(run["result"], ensure_ascii=False))
        else:
            args.output.mkdir(parents=True, exist_ok=False)
            for name in ("events.jsonl", "marks.jsonl", "chunks.jsonl", "serial.bin"):
                (args.output / name).touch()
            finalize(args.output, metadata, result)
    return 0 if result["status"] in ("pass", "skip") and session and run["result"]["status"] == result["status"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
