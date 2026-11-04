#!/usr/bin/env python3
"""Continuous, private evidence capture for the existing local PP diagnostic protocol."""
import argparse
from collections import Counter, defaultdict
import html
import json
import math
from pathlib import Path
import re
import select
import sys
import threading
import time

from device_console import Console, open_serial
from firmware_provenance import artifacts, source_state

OUTCOMES = ("success", "failure", "cancelled", "timeout")

READING_BODY, HEADER_BACK = (342, 500), (40, 40)
TOOL_TYPOGRAPHY, TOOL_SYNC = (205, 1150), (342, 1150)
SYNC_CHECK, SYNC_CANCEL, CONTINUE_OFFLINE, SYNC_STAY = (342, 996), (342, 1096), (342, 1128), (184, 799)
FONT_SIZE, MINUS, PLUS, APPLY, PANEL_CLOSE = (423, 820), (99, 1020), (585, 1020), (504, 1136), (342, 628)
REQUIRED_CONDITIONS = ("hardware", "tf", "books_sha256", "cache", "network")


def fields(value):
    parsed = dict(part.split("=", 1) for part in value.split() if "=" in part)
    if value.startswith("OK accepted "):
        parsed["accepted"] = "1"
    return parsed


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


class Session(Console):
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
        self.trace = None
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
            state, entries = self.request("trace")
            if state.get("schema") != "1":
                raise RuntimeError("Unsupported trace schema")
            if self.trace is not None:
                self.trace += [fields(entry) for entry in entries]
            if int(state["queued"]) == 0:
                return state
        self.gap("trace_drain_budget_exhausted")

    def capture(self, output):
        return super().capture(output, after_chunk=self.drain)

    def cutoff(self):
        state, _ = self.request("trace_stop")
        if state.get("stopped") != "1":
            raise RuntimeError("Device did not freeze the capture")
        self.event("cutoff", sequence=int(state["sequence"]))
        self.drain()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise OSError("serial reader did not stop")
        self.serial.close()
        for stream in (self.raw, self.chunks, self.events, self.marks):
            stream.close()


def refresh_check(before, after):
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


QUIET = ("pending", "content_busy", "save_busy", "save_due")
LONG_WAIT_SECONDS = 60


def settled(state):
    return state.get("view") == "reading" and state.get("saved") == "1" and all(state.get(k) == "0" for k in QUIET)


def quiescent(state):
    return settled(state) and state.get("count") == "idle"


def wait_for(session, accept, what, timeout=None):
    deadline = time.monotonic() + (timeout or session.timeout)
    while True:
        state, _ = session.request("status")
        session.drain()
        if accept(state):
            return state
        if state.get("view") == "error":
            raise AssertionError("the device showed its error page while waiting for " + what)
        if time.monotonic() >= deadline:
            raise TimeoutError(what)


def act(session, command, accept, what, timeout=None):
    for _ in range(5):
        wait_for(session, lambda state: state.get("pending") == "0", "no pending frame before " + command, timeout)
        try:
            session.request(command)
            return wait_for(session, accept, what, timeout)
        except RuntimeError as exc:
            if str(exc) != "ERROR busy":
                raise
    raise TimeoutError("device stayed busy for " + command)


def tap_and_wait_for_view(session, point, view, timeout=None):
    return act(session, "tap %d %d" % point,
               lambda state: state.get("view") == view and state.get("pending") == "0", "view " + view, timeout)


class Unsuitable(Exception):
    pass


def turn(session, key, step):
    before, _ = session.request("status")
    after = act(session, "key " + key, lambda state: state.get("pending") == "0" and state.get("view") == "reading"
                and int(state["frame"]) > int(before["frame"]), key + " frame")
    moved = (int(after["page"]) - int(before["page"])) * step
    if after.get("frame_ok") != "1" or moved not in (0, 1, 2):
        raise AssertionError(f"{key} moved page {before['page']} to {after['page']} or its frame failed")
    if moved != 1:
        raise Unsuitable(f"{key} showed page {after['page']} after {before['page']}: the position is not aligned with "
                         "a counted page; start from a page reached by turning")
    return after


def memory(state):
    return {k: int(state[k]) for k in ("internal", "largest_internal", "psram", "largest_psram")}


def trend_check(samples, internal_limit=4096, block_limit=8192):
    drop = samples[0]["internal"] - min(sample["internal"] for sample in samples)
    block = samples[0]["largest_internal"] - min(sample["largest_internal"] for sample in samples)
    if drop > internal_limit or block > block_limit:
        raise AssertionError(f"internal RAM fell {drop} B and its largest free block {block} B across rounds")
    return drop


def reading_flow(session, book, rounds, turns):
    long_wait = max(session.timeout, LONG_WAIT_SECONDS)
    act(session, "open " + book, lambda s: s.get("view") == "reading" and s.get("pending") == "0", "book opened", long_wait)
    start = wait_for(session, lambda s: quiescent(s) and int(s["pages"]) > 0, "saved idle page with a page count", long_wait)
    if "anchor" not in start or "font_px" not in start:
        return dict(status="inconclusive", reason="This firmware does not report the saved anchor and font size", start=start)
    if int(start["page"]) + turns > int(start["pages"]):
        return dict(status="skip", reason="The saved position is fewer than --turns pages before the end of the book")
    try:
        return rounds_of_reading(session, book, rounds, turns, start, long_wait)
    except Unsuitable as exc:
        return dict(status="inconclusive", reason=str(exc), start=start)


def rounds_of_reading(session, book, rounds, turns, start, long_wait):
    def saved_after(previous, what):
        try:
            return wait_for(session, lambda s: settled(s) and int(s["revision"]) > int(previous["revision"]), what, long_wait)
        except TimeoutError:
            raise AssertionError(what + ": the saved revision did not advance") from None

    def resize(button):
        tap_and_wait_for_view(session, READING_BODY, "tools")
        tap_and_wait_for_view(session, TOOL_TYPOGRAPHY, "typography", long_wait)
        tap_and_wait_for_view(session, FONT_SIZE, "typography")
        tap_and_wait_for_view(session, button, "typography", long_wait)
        applied = act(session, "tap %d %d" % APPLY, lambda s: s.get("pending") == "0"
                      and s.get("view") in ("reading", "typography"), "typography applied", long_wait)
        if applied["view"] == "typography":
            tap_and_wait_for_view(session, PANEL_CLOSE, "reading", long_wait)
            return False
        after = wait_for(session, lambda s: settled(s) and int(s["pages"]) > 0, "repagination after typography change", long_wait)
        if after["font_px"] == size["font_px"]:
            raise AssertionError(f"applying a font-size step left the size at {after['font_px']} px")
        size.update(font_px=after["font_px"])
        return True

    size = dict(font_px=start["font_px"])

    samples, reopened = [], start
    for _ in range(rounds):
        for _ in range(turns):
            turn(session, "K3", 1)
        forward = saved_after(reopened, "forward position saved")
        if forward["anchor"] == start["anchor"]:
            raise AssertionError("the forward save advanced the revision but kept the starting anchor")
        for _ in range(turns):
            turn(session, "K1", -1)
        if saved_after(forward, "start position saved again")["anchor"] != start["anchor"]:
            raise AssertionError("turning back did not save the starting anchor again")
        if resize(PLUS):
            reverse = MINUS
        elif resize(MINUS):
            reverse = PLUS
        else:
            raise AssertionError("neither font-size step changed the typography")
        if not resize(reverse):
            raise AssertionError("the font size could not be restored")
        restored = wait_for(session, lambda s: settled(s) and int(s["pages"]) > 0, "restored typography", long_wait)
        same = ("font_px", "anchor", "page", "pages")
        if any(restored[k] != start[k] for k in same):
            raise AssertionError("reverting the font size left " + ", ".join(f"{k}={restored[k]}" for k in same)
                                 + "; expected " + ", ".join(f"{k}={start[k]}" for k in same))
        tap_and_wait_for_view(session, READING_BODY, "tools")
        tap_and_wait_for_view(session, HEADER_BACK, "library", long_wait)
        act(session, "open " + book, lambda s: s.get("view") == "reading" and s.get("pending") == "0", "book reopened", long_wait)
        reopened = wait_for(session, lambda s: quiescent(s) and int(s["pages"]) > 0, "reopened page quiescent", long_wait)
        same = ("anchor", "revision", "font_px", "page")
        if any(reopened[k] != restored[k] for k in same):
            raise AssertionError("reopening showed " + ", ".join(f"{k}={reopened[k]}" for k in same)
                                 + "; expected " + ", ".join(f"{k}={restored[k]}" for k in same))
        samples.append(memory(reopened))
    return dict(status="pass", scope="injected_input_app_state_and_driver_only", rounds=rounds, turns=turns,
                start=start, after=reopened, memory=samples, internal_drop=trend_check(samples))


def open_operations(entries):
    active = set()
    for entry in entries:
        (active.add if entry.get("begin") == "1" else active.discard)(entry.get("op"))
    return active


def image_sync(session, book):
    long_wait = max(session.timeout, LONG_WAIT_SECONDS)
    session.trace = []
    opened = lambda s: s.get("view") == "reading" and s.get("pending") == "0" and s.get("saved") == "1"
    before = act(session, "open " + book, opened, "book opened at its saved position", long_wait)

    def unchanged(step):
        after = wait_for(session, quiescent, "page saved and quiescent after " + step, long_wait)
        if any(before.get(k) != after.get(k) for k in ("revision", "anchor")):
            raise AssertionError(f"{step} changed saved revision {before['revision']} to {after['revision']} "
                                 f"or anchor {before.get('anchor')} to {after.get('anchor')}")
        return after

    def check():
        tap_and_wait_for_view(session, READING_BODY, "tools")
        tap_and_wait_for_view(session, TOOL_SYNC, "sync", long_wait)
        mark = len(session.trace)
        act(session, "tap %d %d" % SYNC_CHECK, lambda s: s.get("pending") == "0", "sync check accepted", long_wait)
        return mark

    def traced(mark, kind, begin, detail=None):
        return [e for e in session.trace[mark:] if e.get("kind") == kind and e.get("begin") == begin
                and detail in (None, e.get("detail"))]

    mark = check()
    wait_for(session, lambda s: traced(mark, "2", "0"), "sync request finished", long_wait)
    if traced(mark, "2", "0")[0]["result"] != "0":
        tap_and_wait_for_view(session, HEADER_BACK, "reading", long_wait)
        return dict(status="inconclusive", reason="The sync check did not succeed; network or account unavailable", before=before,
                    excused=[e["op"] for kind in ("2", "5") for e in traced(mark, kind, "0") if e["result"] in ("1", "3")])
    answer = wait_for(session, lambda s: s.get("view") == "sync" and s.get("pending") == "0" and s.get("content_busy") == "0"
                      and s.get("sync") != "unchecked" and not open_operations(session.trace[mark:]),
                      "sync answer applied", long_wait)["sync"]
    tap_and_wait_for_view(session, SYNC_STAY if answer == "different" else HEADER_BACK, "reading", long_wait)
    if answer not in ("no-record", "same", "different", "unmapped"):
        return dict(status="inconclusive", reason="The sync check did not produce a valid answer: " + str(answer), before=before)
    unchanged("the completed sync check")

    mark = len(session.trace)
    act(session, "open " + book, opened, "book reopened", long_wait)
    wait_for(session, lambda s: traced(mark, "3", "1", "2"), "image decode of the reopened session", long_wait)
    decode = traced(mark, "3", "1", "2")[0]["op"]
    act(session, "open " + book, opened, "book reopened during the image decode", long_wait)
    wait_for(session, lambda s: decode not in open_operations(session.trace[mark:]), "abandoned image decode ended", long_wait)
    if [e for e in traced(mark, "3", "0", "2") if e["op"] == decode][0]["result"] != "2":
        return dict(status="inconclusive", reason="The image decode finished before the second reopen", before=before)

    mark = check()
    wait_for(session, lambda s: traced(mark, "5", "1", "3") or traced(mark, "2", "0"), "HTTPS phase of the sync check", long_wait)
    if not traced(mark, "5", "1", "3"):
        tap_and_wait_for_view(session, HEADER_BACK, "reading", long_wait)
        return dict(status="inconclusive", reason="The sync request ended before its HTTPS phase", before=before)
    closing = act(session, "tap %d %d" % SYNC_CANCEL, lambda s: s.get("pending") == "0"
                  and s.get("view") in ("network_closing", "sync"), "sync cancelled", long_wait)
    tap_and_wait_for_view(session, CONTINUE_OFFLINE if closing["view"] == "network_closing" else HEADER_BACK, "reading", long_wait)
    unchanged("the cancelled sync check")
    wait_for(session, lambda s: not open_operations(session.trace[mark:]), "cancelled sync request finished", long_wait)
    outcome = traced(mark, "2", "0")[0]["result"]
    if outcome != "2":
        return dict(status="inconclusive", reason="The sync request ended as " + OUTCOMES[int(outcome)] + ", not cancelled",
                    before=before)
    observation = image_observation(session)
    return dict(observation, before=before, sync_answer=answer) if observation["status"] == "pass" else observation


def percentile(samples, p):
    return sorted(samples)[max(0, math.ceil(len(samples) * p) - 1)]


def image_evidence(operations):
    images = [op for op in operations if op["kind"] == "3" and op["detail"] == "2"]
    networks = [op for op in operations if op["kind"] == "2"]
    stages = [op for op in operations if op["kind"] == "5"]
    frames = [op for op in operations if op["kind"] == "0" and op["detail"] == "1"]
    chains = []
    for patch in (op for op in operations if op["kind"] == "4"):
        content = next((op for op in images if all(op[k] == patch[k] for k in ("session", "layout", "request"))
                        and op["result"] == "0" and int(op["time_us"]) <= patch["start_us"]), None)
        frame = next((op for op in frames if op["request"] == patch["frame"]
                      and op["start_us"] >= int(patch["time_us"])), None)
        during = lambda op: content is not None and (
            max(op["start_us"], content["start_us"]) < min(int(op["time_us"]), int(content["time_us"])))
        phases = [op for op in stages if during(op)]
        overlap = [op["op"] for op in networks if during(op)
                   and (op["result"] == "0" or any(phase["request"] == op["request"] for phase in phases))]
        confirmed = bool(content and frame and patch["result"] == frame["result"] == "0"
                         and int(patch["session"]) > 0 and int(patch["layout"]) > 0)
        chains.append(dict(content_op=content["op"] if content else None, patch_op=patch["op"],
                           frame_op=frame["op"] if frame else None, frame=frame["frame"] if frame else None,
                           session=patch["session"], layout=patch["layout"], generation=patch["request"],
                           decoded_images=int(patch["detail"]), network_ops=overlap,
                           network_stages=[op["op"] for op in phases], confirmed=confirmed))
    return chains


def stale_image_patches(operations):
    frames = [op for op in operations if op["kind"] == "0" and int(op["session"]) > 0]
    return [patch["op"] for patch in operations if patch["kind"] == "4" and any(
        int(frame["time_us"]) <= patch["start_us"]
        and (int(frame["session"]), int(frame["layout"])) > (int(patch["session"]), int(patch["layout"]))
        for frame in frames)]


def image_network_check(analysis, observation):
    if observation.get("status") != "pass":
        return observation
    chains = [chain for chain in analysis.get("image_chains", [])
              if chain["confirmed"] and chain["decoded_images"] > 0 and chain["network_ops"]
              and chain["frame"] == observation["after"]["frame"]]
    if not chains:
        return dict(status="inconclusive", reason="No decoded image/network overlap linked to the captured frame",
                    observation=observation)
    return dict(observation, scope="decoded_image_network_overlap_canvas_and_driver_only", evidence=chains)


def image_observation(session):
    after, _ = session.request("status")
    if (after.get("view") != "reading" or after.get("frame_ok") != "1" or after.get("saved") != "1"
            or any(after.get(k) != "0" for k in ("pending", "content_busy", "save_busy", "save_due"))):
        return dict(status="inconclusive", reason="End on a saved, idle image page with a successful frame", after=after)
    try:
        session.capture(session.output / "canvas.pgm")
    except RuntimeError as exc:
        if "frame" not in str(exc).lower():
            raise
        return dict(status="inconclusive", reason="Page changed during canvas capture: " + str(exc), after=after)
    confirmed, _ = session.request("status")
    if any(after.get(k) != confirmed.get(k) for k in ("frame", "crc32", "revision", "view")):
        return dict(status="inconclusive", reason="Page changed during canvas capture", after=confirmed)
    return dict(status="pass", after=after)


def analyze(output):
    active, counts, samples = {}, Counter(), defaultdict(list)
    gaps, last_seq, metric_active, failures = [], None, {}, 0
    failed_operations = []
    event_count = 0
    last_queued = 0
    last_device_sequence = None
    dropped_baseline = tx_baseline = 0
    cutoff = None
    composition = Counter()
    operations = []
    with (Path(output) / "events.jsonl").open() as stream:
        for line in stream:
            event = json.loads(line)
            if event["type"] == "baseline":
                active.clear()
                counts.clear()
                failed_operations.clear()
                samples.clear()
                metric_active.clear()
                composition.clear()
                operations.clear()
                cutoff = None
                last_seq = event["sequence"]
                event_count = failures = 0
                gaps = [g for g in gaps if g not in ("device_trace_dropped", "device_tx_failures",
                        "trace_sequence_gap", "operation_start_missing", "device_restart_or_reinitialize")]
                dropped_baseline = event["dropped"]
                tx_baseline = event["tx_failures"]
            if event["type"] == "cutoff":
                cutoff = event["sequence"]
            if event["type"] == "gap":
                gaps.append(event["reason"])
            if event["type"] == "command_timeout":
                counts["command_timeout"] += 1
            f = event.get("fields", {})
            if event["type"] == "reply":
                if event["line"] == "ERROR busy":
                    counts["busy"] += 1
                elif event["line"].startswith("ERROR "):
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
                    not 0 <= int(f["kind"]) < 6 or not 0 <= int(f["result"]) < len(OUTCOMES) or
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
                elif int(f["time_us"]) < int(start["time_us"]):
                    gaps.append("operation_time_reversed")
                else:
                    operations.append(dict(f, start_us=int(start["time_us"])))
                result = int(f["result"])
                counts[OUTCOMES[result]] += 1
                if result in (1, 3):
                    failed_operations.append(f["op"])
                composition[f'{f["kind"]}:{f["detail"]}:{result}'] += 1
                samples[f["kind"]].append(int(f["duration_us"]))
    if cutoff is None:
        gaps.append("device_cutoff_missing")
    elif last_device_sequence != cutoff or last_seq != cutoff:
        gaps.append("device_cutoff_not_drained")
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
    return dict(counts={k: counts[k] for k in (*OUTCOMES, "unfinished", "command_failure", "command_timeout", "busy")},
                failed_operations=failed_operations,
                gaps=sorted(set(gaps)), distributions=distributions, events=event_count,
                composition=dict(composition), frame_failures=failures,
                image_chains=image_evidence(operations), stale_image_patches=stale_image_patches(operations),
                failure_rate=counts["failure"] / completed if completed else None)


def finalize(output, metadata, result):
    output = Path(output)
    try:
        analysis = analyze(output)
    except (KeyError, ValueError, TypeError) as exc:
        analysis = dict(gaps=["malformed_structured_evidence"], error=str(exc), counts={}, distributions={}, events=0)
        result = dict(status="infra_error", reason="Cannot parse complete structured evidence")
    excused = set(result.get("excused", ()))
    failed = analysis["counts"] and (any(op not in excused for op in analysis["failed_operations"])
                                     or analysis["counts"]["command_failure"] or analysis["frame_failures"])
    incomplete = analysis["gaps"] or not analysis["events"] or metadata.get("missing")
    if result["status"] == "pass" and incomplete:
        result = dict(status="inconclusive", reason="Incomplete evidence or run conditions", assertion=result)
    elif result["status"] in ("pass", "inconclusive") and failed and not incomplete:
        result = dict(status="fail", reason="Failure observed during the run", assertion=result)
    if result["status"] != "infra_error" and analysis.get("stale_image_patches") and not incomplete:
        result = dict(status="fail", reason="An image from an earlier session or layout was adopted", assertion=result)
    if metadata.get("mode") in ("image-network", "image-sync"):
        result = image_network_check(analysis, result)
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
    current = source_state()
    metadata["checkout_source"] = current
    metadata["host_platform"] = sys.platform
    metadata["mode"] = args.mode
    book = getattr(args, "book", None)
    metadata["workload"] = {"refresh": "refresh-v1:K2x1", "image-sync": f"image-sync-v2:check,reopen,cancel-in-https:book={book}",
                            "reading-flow": "reading-flow-v1:rounds=%s,turns=%s:book=%s" % (
                                getattr(args, "rounds", None), getattr(args, "turns", None), book)}.get(args.mode)
    hashes, config = artifacts(args.firmware) if args.firmware else ({}, None)
    metadata["artifacts_sha256"] = hashes
    if config and config.is_file():
        metadata["effective_sdkconfig"] = config.read_text()
    metadata["missing"] = [k for k in REQUIRED_CONDITIONS if not metadata.get(k)]
    metadata["missing"] += [k for k in ("pico_page.bin", "pico_page.elf", "sdkconfig") if k not in hashes]
    manifest = args.firmware / "provenance.json" if args.firmware else None
    provenance = json.loads(manifest.read_text()) if manifest and manifest.is_file() else {}
    for key in ("source_commit", "source_dirty", "source_sha256"):
        metadata[key] = provenance.get(key)
    metadata["provenance_verified"] = bool(provenance and provenance.get("schema") == 1
        and provenance.get("artifacts_sha256") == hashes
        and all(provenance.get(k) == v for k, v in current.items()))
    if not metadata["provenance_verified"]:
        metadata["missing"].append("build_source_provenance")
    if not metadata.get("device_build_verified"):
        metadata["missing"].append("device_build_verified")
    return metadata


def workload_place(state):
    return ":page={page}/{pages}:anchor={anchor}:font_px={font_px}".format(**{k: state.get(k) for k in ("page", "pages", "anchor", "font_px")})


def compare(first, second):
    left, right = (json.loads(Path(p).read_text()) for p in (first, second))
    keys = (*REQUIRED_CONDITIONS, "source_dirty", "effective_sdkconfig", "mode", "workload")
    def conditions(run):
        m = run["metadata"].copy()
        mode_flags = ("CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE", "CONFIG_PICO_PAGE_PERF_METRICS",
                      "CONFIG_PICO_PAGE_TRACE_EVENTS", "CONFIG_PICO_PAGE_LEGACY_PERF_LOG")
        m["effective_sdkconfig"] = "\n".join(line for line in m.get("effective_sdkconfig", "").splitlines()
                                               if not any(flag in line for flag in mode_flags))
        return {k: m.get(k) for k in keys}
    comparable = (left.get("result", {}).get("status") == right.get("result", {}).get("status") == "pass"
                  and bool(left["metadata"].get("workload"))
                  and all(m.get("mode") not in ("image-sync", "reading-flow")
                          or (isinstance(m.get("workload"), str) and re.search(r":font_px=[1-9][0-9]*\Z", m["workload"]))
                          for m in (left["metadata"], right["metadata"]))
                  and left["metadata"].get("provenance_verified") is True
                  and right["metadata"].get("provenance_verified") is True
                  and bool(left["analysis"].get("composition"))
                  and left["analysis"].get("composition") == right["analysis"].get("composition")
                  and conditions(left) == conditions(right) and not left["metadata"].get("missing")
                  and not right["metadata"].get("missing") and not left["analysis"]["gaps"] and not right["analysis"]["gaps"]
                  and bool(left["analysis"]["distributions"]) and bool(right["analysis"]["distributions"])
                  and bool(left["metadata"].get("source_commit")) and bool(right["metadata"].get("source_commit"))
                  and left["metadata"].get("source_dirty") is False and right["metadata"].get("source_dirty") is False)
    return dict(comparable=comparable, reason="Conditions match" if comparable else "Conditions or evidence differ; do not infer a performance change",
                source_commits=[left["metadata"].get("source_commit"), right["metadata"].get("source_commit")],
                same_source=bool(left["metadata"].get("provenance_verified") and right["metadata"].get("provenance_verified")
                    and left["metadata"].get("source_sha256")
                    and left["metadata"].get("source_sha256") == right["metadata"].get("source_sha256")),
                first=left["analysis"], second=right["analysis"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare", nargs=2, metavar=("BASE_RUN", "NEW_RUN"))
    parser.add_argument("--port")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--metadata", type=Path, help="Private JSON: hardware, tf, books_sha256, cache, network, device_build_verified")
    parser.add_argument("--firmware", type=Path, help="Build directory containing bin, ELF and effective sdkconfig")
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--mode", choices=("manual", "refresh", "image-network", "image-sync", "reading-flow"), default="manual")
    parser.add_argument("--book", help="Card-relative path of the synthetic test EPUB that the automatic scenarios open")
    parser.add_argument("--rounds", type=int, default=3, help="reading-flow rounds")
    parser.add_argument("--turns", type=int, default=5, help="reading-flow page turns per direction")
    args = parser.parse_args()
    if args.compare:
        print(json.dumps(compare(*args.compare), ensure_ascii=False, indent=2))
        return 0
    if not args.port or not args.output or args.timeout <= 0:
        parser.error("--port, --output and a positive --timeout are required")
    if args.mode in ("image-sync", "reading-flow") and (not args.book or args.rounds < 1 or args.turns < 1):
        parser.error("--book and positive --rounds/--turns are required for automatic scenarios")
    if args.output.exists():
        parser.error("--output must be a new private directory")
    metadata = metadata_for(args)
    result = dict(status="inconclusive", reason="Manual observation has no automatic functional assertion")
    session = None
    try:
        session = Session(open_serial(args.port), args.output, args.timeout)
        session.event("start", mode=args.mode)
        session.request("status")
        baseline, _ = session.request("trace_start")
        if baseline is None:
            raise RuntimeError("Cannot establish trace baseline")
        session.event("baseline", dropped=int(baseline["dropped"]), tx_failures=int(baseline["tx_failures"]),
                      sequence=int(baseline["sequence"]))
        try:
            if args.mode == "refresh":
                result = refresh_scenario(session)
            elif args.mode == "image-sync":
                result = image_sync(session, args.book)
            elif args.mode == "reading-flow":
                result = reading_flow(session, args.book, args.rounds, args.turns)
        except AssertionError as exc:
            result = dict(status="fail", reason=str(exc))
        except TimeoutError as exc:
            result = dict(status="inconclusive", reason="Timeout: " + str(exc))
        if args.mode not in ("refresh", "image-sync", "reading-flow"):
            print("Recording. Enter 'mark short description' to mark a problem; 'end' to finish.")
            if args.mode == "image-network":
                print("Use the device to overlap image loading with network sync, then return to the saved image page. "
                      "Missing overlap is inconclusive; sync correctness and physical display require separate acceptance.")
            while True:
                ready, _, _ = select.select([sys.stdin], [], [], .25)
                if ready:
                    command = sys.stdin.readline()
                    if not command or command.strip() == "end":
                        break
                    if command.startswith("mark"):
                        session.mark(command[4:].strip())
                session.drain()
            if args.mode == "image-network":
                result = image_observation(session)
        place = result.get("start") or result.get("after")
        if args.mode in ("image-sync", "reading-flow") and place:
            metadata["workload"] += workload_place(place)
        session.cutoff()
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
