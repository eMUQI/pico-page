import argparse
import json
from pathlib import Path
import queue
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from device_workbench import (Session, analyze, compare, finalize, refresh_check, refresh_scenario,
                              metadata_for, image_observation, image_sync, reading_flow, trend_check,
                              stale_image_patches, READING_BODY, HEADER_BACK, TOOL_TYPOGRAPHY, TOOL_SYNC,
                              SYNC_CHECK, SYNC_CANCEL, CONTINUE_OFFLINE, SYNC_STAY, FONT_SIZE, MINUS, PLUS, APPLY)


class Serial:
    def __init__(self, respond=None):
        self.queue = queue.Queue()
        self.respond = respond
        self.closed = False
        self.writes = []

    @property
    def in_waiting(self):
        return 1

    def read(self, _):
        try:
            item = self.queue.get(timeout=.01)
        except queue.Empty:
            return b""
        if isinstance(item, Exception):
            raise item
        return item

    def feed(self, data):
        self.queue.put(data)

    def write(self, data):
        self.writes.append(data)
        if self.respond:
            self.respond(self, data.decode().strip().split(" ", 2))
        return len(data)

    def close(self):
        self.closed = True


def trace(seq, op=1, begin=1, result=0):
    return f"PP 1 ENTRY seq={seq} time_us={seq * 100} op={op} duration_us=100 task=7 kind=0 begin={begin} result={result} session=1 layout=2 frame=3 request=4 detail=0\n"


class Reader:
    """Scripted stand-in for the diagnostic console: views, page state and queued trace entries."""
    def __init__(self, **faults):
        self.state = dict(view="reading", pending=0, content_busy=0, save_busy=0, save_due=0, saved=1, revision=10,
                          page=4, pages=20, count="idle", frame=1, frame_ok=1, crc32="abcd", internal=90000, largest_internal=31744,
                          psram=4000000, largest_psram=3000000)
        self.faults, self.font, self.draft, self.busy, self.sequence, self.entries, self.taps = faults, 0, 0, 0, 0, [], []
        self.opens = 0

    def emit(self, op, kind, begin, time_us, result=0, frame=0, request=0, detail=0, session=1, layout=1):
        self.sequence += 1
        self.entries.append(f"ENTRY seq={self.sequence} time_us={time_us} op={op} duration_us=10 task=7 kind={kind} "
                            f"begin={begin} result={result} session={session} layout={layout} frame={frame} "
                            f"request={request} detail={detail}")

    def tap(self, point):
        s, view = self.state, self.state["view"]
        self.taps.append(point)
        if (view, point) == ("reading", READING_BODY): s["view"] = "tools"
        elif (view, point) == ("tools", TOOL_TYPOGRAPHY): s["view"], self.draft = "typography", self.font
        elif (view, point) == ("tools", TOOL_SYNC): s["view"] = "sync"
        elif (view, point) == ("tools", HEADER_BACK): s["view"] = "library"
        elif (view, point) == ("typography", PLUS): self.draft += 1
        elif (view, point) == ("typography", MINUS): self.draft -= 1
        elif (view, point) == ("typography", APPLY):
            self.font = self.draft
            s.update(view="reading", pages=20 - 4 * self.font, page=4 - self.font + self.faults.get("drift", 0) * (self.font == 0))
        elif (view, point) == ("sync", SYNC_CHECK) and SYNC_CHECK not in self.taps[:-1]:
            # First check: completes, then maps the remote position; the answer shows differing positions.
            self.emit(20, 2, 1, 10, request=7); self.emit(21, 5, 1, 11, request=7, detail=3)
            self.emit(21, 5, 0, 20, request=7, detail=3)
            self.emit(20, 2, 0, 21, result=self.faults.get("offline", 0), request=7)
            self.emit(22, 3, 1, 30, request=3, detail=13); self.emit(22, 3, 0, 40, request=3, detail=13)
        elif (view, point) == ("sync", SYNC_STAY):
            s.update(view="sync" if self.faults.get("same") else "reading", revision=s["revision"] + self.faults.get("adopted", 0))
        elif (view, point) == ("sync", HEADER_BACK): s["view"] = "reading"
        elif (view, point) == ("sync", SYNC_CHECK):
            self.emit(2, 2, 1, 100, request=8)
            if not self.faults.get("no_https"):
                self.emit(3, 5, 1, 110, request=8, detail=3)
            else:
                self.emit(2, 2, 0, 120, result=1, request=8)
        elif (view, point) == ("sync", SYNC_CANCEL): s["view"] = "network_closing"
        elif (view, point) == ("network_closing", CONTINUE_OFFLINE):
            s.update(view="reading", frame=21, revision=s["revision"] + self.faults.get("moved", 0))  # 22 once this tap is counted
            # The image decodes while the cancelled request is still inside its HTTPS phase.
            self.emit(4, 3, 1, 200, request=4, detail=2); self.emit(3, 5, 0, 260, result=2, request=8, detail=3)
            self.emit(2, 2, 0, 270, result=2, request=8); self.emit(4, 3, 0, 300, request=4, detail=2)
            self.emit(5, 4, 1, 310, frame=9, request=4, detail=1); self.emit(5, 4, 0, 320, frame=9, request=4, detail=1)
            self.emit(6, 0, 1, 330, frame=22, request=9, detail=1); self.emit(6, 0, 0, 400, frame=22, request=9, detail=1)
        elif (view, point) == ("typography", FONT_SIZE): pass
        else: raise AssertionError((view, point))
        s["frame"] += 1

    def __call__(self, port, command):
        _, ident, verb = command
        s, value = self.state, "accepted"
        if verb == "status":
            value = " ".join(f"{k}={v}" for k, v in s.items())
        elif verb == "trace":
            batch, self.entries = self.entries[:8], self.entries[8:]
            for entry in batch: port.feed(f"PP {ident} {entry}\n".encode())
            value = f"schema=1 queued={len(self.entries)} sequence={self.sequence} dropped=0 tx_failures=0"
        elif self.busy:
            self.busy -= 1
            port.feed(f"PP {ident} ERROR busy\n".encode())
            return
        elif verb in ("key K3", "key K1"):
            step = 1 if verb == "key K3" else -1
            s.update(page=s["page"] + step * (1 + self.faults.pop("skip", 0)), frame=s["frame"] + 1,
                     revision=s["revision"] + (0 if self.faults.get("unsaved") else 1))
        elif verb.startswith("tap "):
            self.tap(tuple(map(int, verb.split()[1:])))
        elif verb.startswith("open "):
            self.opens += 1
            s.update(view="reading", frame=s["frame"] + 1, internal=s["internal"] - self.faults.get("leak", 0))
        else:
            raise AssertionError(verb)
        port.feed(f"PP {ident} OK {value}\n".encode())


class WorkbenchTest(unittest.TestCase):
    def test_reading_flow_checks_turns_typography_reopen_and_memory(self):
        reader = Reader()
        reader.busy = 1  # the first input is refused once and retried through the same route
        result = reading_flow(self.start(Serial(reader)), "Books/t.epub", 2, 3)
        self.assertEqual((result["status"], result["rounds"], len(result["memory"]), result["internal_drop"]), ("pass", 2, 2, 0))
        self.assertEqual(reader.state["page"], 4)
        self.assertEqual(reader.font, 0)
        self.assertEqual(reader.taps.count(PLUS), 2)
        self.assertEqual(self.finish()["counts"]["busy"], 1)

    def test_reading_flow_rejects_skips_unsaved_turns_drift_and_leaks(self):
        for fault, message in ((dict(skip=1), "moved page"), (dict(unsaved=1), "saved revision did not advance"),
                               (dict(drift=1), "reverting the font size"), (dict(leak=5000), "internal RAM fell")):
            with self.subTest(fault=fault):
                self.output = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).iterdir()))))
                with self.assertRaisesRegex(AssertionError, message):
                    reading_flow(self.start(Serial(Reader(**fault))), "Books/t.epub", 2, 2)
                self.finish()
        self.output = Path(self.temp.name) / "library"
        reader = Reader()
        reader.state["view"] = "library"
        self.assertEqual(reading_flow(self.start(Serial(reader)), "Books/t.epub", 1, 1)["status"], "skip")
        self.assertEqual(trend_check([dict(internal=9000), dict(internal=5000), dict(internal=9000)]), 4000)

    def test_image_sync_cancels_in_https_and_requires_chain_and_position(self):
        reader = Reader()
        session = self.start(Serial(reader))
        with patch.object(session, "capture"):
            observation = image_sync(session, "Books/t.epub")
        self.assertEqual(observation["status"], "pass")
        self.assertEqual(reader.taps[-3:], [SYNC_CHECK, SYNC_CANCEL, CONTINUE_OFFLINE])
        self.assertEqual((reader.taps.count(SYNC_STAY), reader.taps.count(HEADER_BACK), reader.opens), (1, 0, 3))
        session._line(f"PP 50 OK stopped=1 queued=0 sequence={reader.sequence} dropped=0 tx_failures=0")
        session.event("cutoff", sequence=reader.sequence)
        self.finish()
        run = finalize(self.output, dict(mode="image-sync"), observation)
        self.assertEqual(run["result"]["status"], "pass")
        self.assertEqual(run["result"]["evidence"][0]["network_stages"], ["3"])

    def test_image_sync_position_change_fails_and_early_end_is_inconclusive(self):
        for fault, step in ((dict(moved=1), "the cancelled sync check"), (dict(adopted=1), "the completed sync check")):
            with self.subTest(fault=fault), self.assertRaisesRegex(AssertionError, step + " changed saved revision"):
                self.output = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).iterdir()))))
                session = self.start(Serial(Reader(**fault)))
                with patch.object(session, "capture"):
                    image_sync(session, "Books/t.epub")
            self.finish()
        for fault in (dict(no_https=1), dict(offline=1)):
            self.output = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).iterdir()))))
            self.assertEqual(image_sync(self.start(Serial(Reader(**fault))), "Books/t.epub")["status"], "inconclusive")
            self.finish()
        # An answer without a choice page is left through the header control.
        self.output = Path(self.temp.name) / "same"
        reader = Reader(same=1)
        session = self.start(Serial(reader))
        with patch.object(session, "capture"):
            self.assertEqual(image_sync(session, "Books/t.epub")["status"], "pass")
        self.assertEqual(reader.taps.count(HEADER_BACK), 1)

    def test_stale_image_patch_after_newer_session_frame_fails(self):
        frame = dict(kind="0", session="2", layout="1", time_us="100", start_us=50, op="7")
        patch_op = dict(kind="4", session="1", layout="1", time_us="130", start_us=120, op="8")
        self.assertEqual(stale_image_patches([frame, patch_op]), ["8"])
        self.assertEqual(stale_image_patches([dict(frame, session="1"), patch_op]), [])
        self.assertEqual(stale_image_patches([dict(frame, time_us="125"), patch_op]), [])

    def test_optional_prefetch_failure_does_not_fail_the_run(self):
        session = self.start()
        for seq, (op, begin, result, detail) in enumerate(((1, 1, 0, 258), (1, 0, 1, 258), (2, 1, 0, 2), (2, 0, 0, 2)), 1):
            session._line(f"PP 1 ENTRY seq={seq} time_us={seq * 10} op={op} duration_us=10 task=7 kind=3 begin={begin} "
                          f"result={result} session=0 layout=0 frame=0 request=4 detail={detail}")
        self.cutoff(4)
        self.finish()
        run = finalize(self.output, {}, dict(status="pass"))
        self.assertEqual((run["analysis"]["counts"]["optional_failure"], run["analysis"]["counts"]["failure"]), (1, 0))
        self.assertEqual(run["result"]["status"], "pass")

    def image_trace(self, *, overlap=True, content_result=0, patch_result=0, frame_result=0,
                    decoded=1, generation=4, frame_request=9, network_result=0, stage=None):
        session = self.start()
        # Arrival order follows device monotonic time, including the independent network worker.
        events = [(1, 3, 1, 100, 0, 0, 4, 2), (2, 2, 1, 150 if overlap else 210, 0, 0, 8, 0),
                  (1, 3, 0, 200, content_result, 0, 4, 2), (2, 2, 0, 300, network_result, 0, 8, 0),
                  (3, 4, 1, 310, 0, 9, generation, decoded), (3, 4, 0, 320, patch_result, 9, generation, decoded),
                  (4, 0, 1, 330, 0, 22, frame_request, 1), (4, 0, 0, 400, frame_result, 22, frame_request, 1)]
        if stage:  # (begin_us, end_us, request): one NetworkStage operation of the station phase
            events += [(5, 5, 1, stage[0], 0, 0, stage[2], 1), (5, 5, 0, stage[1], network_result, 0, stage[2], 1)]
        for seq, (op, kind, begin, timestamp, result, frame, request, detail) in enumerate(sorted(events, key=lambda e: e[3]), 1):
            session._line(f"PP 1 ENTRY seq={seq} time_us={timestamp} op={op} duration_us=10 task=7 "
                          f"kind={kind} begin={begin} result={result} session=1 layout=2 frame={frame} request={request} detail={detail}")
        self.cutoff(len(events))
        self.finish()

    def test_image_network_chain_requires_decode_adoption_and_matching_frame(self):
        self.image_trace()
        run = finalize(self.output, dict(mode="image-network"), dict(status="pass", after=dict(frame="22")))
        self.assertEqual(run["result"]["status"], "pass")
        self.assertEqual(run["result"]["evidence"][0]["network_ops"], ["2"])
        self.assertEqual(finalize(self.output, dict(mode="image-network"),
                                 dict(status="pass", after=dict(frame="23")))["result"]["status"], "inconclusive")

    def test_image_network_missing_failed_or_stale_evidence_cannot_pass(self):
        for changes in (dict(overlap=False), dict(content_result=2), dict(patch_result=1),
                        dict(frame_result=1), dict(decoded=0), dict(generation=5), dict(frame_request=10)):
            with self.subTest(changes=changes):
                self.output = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).iterdir()))))
                self.image_trace(**changes)
                run = finalize(self.output, dict(mode="image-network"), dict(status="pass", after=dict(frame="22")))
                self.assertNotEqual(run["result"]["status"], "pass")

    def test_cancelled_network_overlaps_only_with_a_recorded_phase(self):
        # result 2 = cancelled. Without a phase the request may have stopped at its entry guard.
        cases = ((None, "inconclusive", []), ((160, 190, 8), "pass", ["5"]),
                 ((160, 190, 9), "inconclusive", ["5"]), ((205, 290, 8), "inconclusive", []))
        for stage, expected, stages in cases:
            with self.subTest(stage=stage):
                self.output = Path(self.temp.name) / ("run-" + str(len(list(Path(self.temp.name).iterdir()))))
                self.image_trace(network_result=2, stage=stage)
                run = finalize(self.output, dict(mode="image-network"), dict(status="pass", after=dict(frame="22")))
                self.assertEqual(run["result"]["status"], expected)
                self.assertEqual(run["analysis"]["image_chains"][0]["network_stages"], stages)

    def test_recorded_failure_outranks_inconclusive_observation(self):
        self.image_trace(frame_result=1)
        observation = dict(status="inconclusive", reason="End on a saved, idle image page with a successful frame")
        run = finalize(self.output, dict(mode="image-network"), observation)
        self.assertEqual(run["result"]["status"], "fail")
        self.assertEqual(run["result"]["assertion"], observation)
        # Missing run conditions or evidence gaps still leave the run undecided; a skipped precondition is not a failure.
        self.assertEqual(finalize(self.output, dict(mode="image-network", missing=["device_build_verified"]),
                                  observation)["result"]["status"], "inconclusive")
        self.output = Path(self.temp.name) / "gap"
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(3, begin=0, result=1).strip())
        self.cutoff(3)
        self.finish()
        self.assertEqual(finalize(self.output, {}, observation)["result"]["status"], "inconclusive")
        self.output = Path(self.temp.name) / "skip"
        self.image_trace(frame_result=1)
        self.assertEqual(finalize(self.output, {}, dict(status="skip", reason="x"))["result"]["status"], "skip")

    def test_capture_race_is_inconclusive_but_transport_fault_propagates(self):
        state = dict(view="reading", frame_ok="1", saved="1", pending="0", content_busy="0", save_busy="0",
                     save_due="0", frame="22", revision="3", crc32="abcd", bytes="415872")
        def reply(port, command):
            _, ident, verb = command
            if verb.startswith("screen"):
                port.feed(f"PP {ident} ERROR frame_not_confirmed_or_changed\n".encode())
            else:
                port.feed((f"PP {ident} OK " + " ".join(f"{k}={v}" for k, v in state.items()) + "\n").encode())
        session = self.start(Serial(reply))
        result = image_observation(session)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("frame_not_confirmed_or_changed", result["reason"])
        with patch.object(session, "capture", side_effect=RuntimeError("Device response line exceeds the protocol limit")):
            with self.assertRaises(RuntimeError):
                image_observation(session)

    def test_image_observation_requires_idle_and_unchanged_capture(self):
        state = dict(view="reading", frame_ok="1", saved="1", pending="0", content_busy="0",
                     save_busy="0", save_due="0", frame="22", revision="3", crc32="abcd")
        def reply(port, command):
            _, ident, verb = command
            self.assertEqual(verb, "status")
            port.feed((f"PP {ident} OK " + " ".join(f"{k}={v}" for k, v in state.items()) + "\n").encode())
        session = self.start(Serial(reply))
        with patch.object(session, "capture"):
            self.assertEqual(image_observation(session)["status"], "pass")
        state["content_busy"] = "1"
        with patch.object(session, "capture") as capture:
            self.assertEqual(image_observation(session)["status"], "inconclusive")
            capture.assert_not_called()
        state["content_busy"] = "0"
        with patch.object(session, "capture", side_effect=lambda _: state.update(frame="23")):
            self.assertEqual(image_observation(session)["status"], "inconclusive")

    def setUp(self):
        waits = patch("device_workbench.LONG_WAIT", .3)
        waits.start()
        self.addCleanup(waits.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.output = Path(self.temp.name) / "run"
        self.session = None

    def tearDown(self):
        if self.session and not self.session.serial.closed:
            self.session.close()
        self.temp.cleanup()

    def start(self, serial=None, timeout=.1):
        self.session = Session(serial or Serial(), self.output, timeout)
        return self.session

    def cutoff(self, sequence):
        self.session._line(f"PP 50 OK stopped=1 queued=0 sequence={sequence} dropped=0 tx_failures=0")
        self.session.event("cutoff", sequence=sequence)

    def finish(self):
        self.session.close()
        return analyze(self.output)

    def test_interleaved_raw_and_reply(self):
        raw = b"I normal log\r\nPP 0 FRAME number=1 success=1\nPP 999 OK late=1\nPP 1 ENTRY one=2\nPP 1 OK pending=0\n"
        def reply(port, _):
            for chunk in (raw[:9], raw[9:39], raw[39:]):
                port.feed(chunk)
        session = self.start(Serial(reply))
        state, entries = session.request("status")
        self.assertEqual(state, {"pending": "0"})
        self.assertEqual(entries, ["ENTRY one=2"])
        self.finish()
        self.assertEqual((self.output / "serial.bin").read_bytes(), raw)
        chunks = [json.loads(s) for s in (self.output / "chunks.jsonl").read_text().splitlines()]
        self.assertEqual(sum(c["size"] for c in chunks), len(raw))
        self.assertEqual([c["offset"] for c in chunks], [0, 9, 39])
        events = (self.output / "events.jsonl").read_text()
        self.assertIn('"type": "device"', events)
        self.assertIn('"type": "log"', events)

    def test_timeout_and_late_reply_do_not_satisfy_next_request(self):
        session = self.start()
        with self.assertRaises(TimeoutError):
            session.request("status")
        session.serial.respond = lambda p, cmd: p.feed(b"PP 1 OK stale=1\n" + f"PP {cmd[1]} OK fresh=1\n".encode())
        self.assertEqual(session.request("status")[0], {"fresh": "1"})
        result = self.finish()
        self.assertEqual(result["counts"]["command_timeout"], 1)
        self.assertIn("command_timeout", result["gaps"])

    def test_error_is_not_success(self):
        session = self.start(Serial(lambda p, c: p.feed(f"PP {c[1]} ERROR {'busy' if c[1] == '1' else 'invalid_key'}\n".encode())))
        with self.assertRaisesRegex(RuntimeError, "busy"):
            session.request("key K2")
        with self.assertRaisesRegex(RuntimeError, "invalid_key"):
            session.request("key K9")
        counts = self.finish()["counts"]
        # A refused input while a frame is pending is the device's guard, reported apart from command faults.
        self.assertEqual((counts["busy"], counts["command_failure"]), (1, 1))

    def test_disconnect_wakes_request(self):
        session = self.start(Serial(lambda p, _: p.feed(OSError("USB disconnected"))))
        with self.assertRaisesRegex(OSError, "USB disconnected"):
            session.request("status")
        self.assertTrue(any("disconnect" in g for g in self.finish()["gaps"]))

    def test_overflow_keeps_raw_and_reports_gap(self):
        payload = b"x" * 9000 + b"\nPP 1 OK recovered=1\n"
        session = self.start(Serial(lambda p, _: p.feed(payload)))
        self.assertEqual(session.request("status")[0]["recovered"], "1")
        self.assertIn("serial_line_overflow", self.finish()["gaps"])
        self.assertEqual((self.output / "serial.bin").read_bytes(), payload)

    def test_all_outcomes_and_unfinished(self):
        session = self.start()
        for i in range(4):
            session._line(trace(i * 2 + 1, i + 1).strip())
            session._line(trace(i * 2 + 2, i + 1, 0, i).strip())
        session._line(trace(9, 5).strip())
        summary = self.finish()
        for key in ("success", "failure", "cancelled", "timeout", "unfinished"):
            self.assertEqual(summary["counts"][key], 1)
        self.assertIsNone(summary["distributions"]["0"]["p95_us"])

    def test_complete_distribution_and_failure_rate(self):
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(2, begin=0, result=1).strip())
        self.cutoff(2)
        summary = self.finish()
        self.assertFalse(summary["gaps"])
        self.assertEqual(summary["distributions"]["0"]["p95_us"], 100)
        self.assertEqual(summary["failure_rate"], 1)
        report = finalize(self.output, {}, {"status": "pass"})
        self.assertEqual(report["result"]["status"], "fail")

    def test_missing_or_dropped_event_is_not_green(self):
        session = self.start()
        session._line(trace(4, begin=0).strip())
        session._line("PP 1 OK dropped=2 tx_failures=1")
        self.finish()
        report = finalize(self.output, {}, {"status": "pass"})
        self.assertEqual(report["result"]["status"], "inconclusive")
        self.assertIn("operation_start_missing", report["analysis"]["gaps"])

    def test_empty_manual_and_marks_are_reviewable(self):
        session = self.start()
        session.mark("<script>problem</script>")
        session._line("PP 0 FRAME number=1 success=0")
        self.finish()
        report = finalize(self.output, {}, {"status": "inconclusive"})
        self.assertEqual(report["analysis"]["frame_failures"], 1)
        page = (self.output / "report.html").read_text()
        self.assertNotIn("<script>", page)
        self.assertIn("&lt;script&gt;", page)
        self.assertTrue(json.loads((self.output / "mark-windows.json").read_text())[0]["events"])

    def test_context_mismatch_and_sequence_gap(self):
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(3, begin=0).replace("layout=2", "layout=4").strip())
        summary = self.finish()
        self.assertIn("trace_sequence_gap", summary["gaps"])
        self.assertIn("operation_context_changed", summary["gaps"])

    def test_refresh_independent_assertions(self):
        before = dict(view="reading", revision="9", page="3", crc32="abcd")
        after = dict(before, frame_ok="1", purpose="2", clean="1")
        refresh_check(before, after)
        for key, value in (("revision", "10"), ("frame_ok", "0"), ("page", "4"), ("crc32", "0000")):
            with self.subTest(key=key), self.assertRaises(AssertionError):
                refresh_check(before, dict(after, **{key: value}))

    def test_refresh_closed_loop_uses_normal_route(self):
        frame = 1
        def reply(port, command):
            nonlocal frame
            _, ident, verb = command
            if verb == "status":
                value = f"view=reading pending=0 content_busy=0 save_busy=0 save_due=0 saved=1 revision=9 page=3 crc32=abcd frame={frame} frame_ok=1 purpose=2 clean=1"
            elif verb == "trace":
                value = "schema=1 queued=0 dropped=0"
            elif verb == "key K2":
                frame += 1
                port.feed(f"PP 0 FRAME number={frame} success=1 purpose=2 clean=1\n".encode())
                value = "accepted pending=1"
            else:
                self.fail(verb)
            port.feed(f"PP {ident} OK {value}\n".encode())
        session = self.start(Serial(reply))
        with patch.object(session, "capture") as capture:
            self.assertEqual(refresh_scenario(session)["status"], "pass")
            capture.assert_called_once()
        self.finish()
        # Fake transport and no trace evidence cannot be promoted to hardware acceptance.
        self.assertEqual(finalize(self.output, {}, {"status": "pass"})["result"]["status"], "inconclusive")

    def test_prior_drops_baseline_and_new_loss(self):
        session = self.start()
        session._line("PP 1 OK dropped=4 tx_failures=2")
        session.event("baseline", dropped=4, tx_failures=2, sequence=11)
        session._line(trace(12).strip())
        session._line(trace(13, begin=0).strip())
        session._line("PP 2 OK dropped=4 tx_failures=2")
        session._line("PP 3 OK stopped=1 queued=0 sequence=13 dropped=4 tx_failures=2")
        session.event("cutoff", sequence=13)
        self.finish()
        self.assertFalse(analyze(self.output)["gaps"])

    def test_malformed_trace_still_produces_report(self):
        session = self.start()
        session._line("PP 1 ENTRY seq=broken op=1")
        session.close()
        run = finalize(self.output, {}, {"status": "pass"})
        self.assertEqual(run["result"]["status"], "infra_error")
        self.assertTrue((self.output / "report.html").exists())

    def test_pending_zero_without_new_frame_times_out(self):
        def reply(port, command):
            _, ident, verb = command
            if verb == "status":
                value = "view=reading pending=0 content_busy=0 save_busy=0 save_due=0 saved=1 frame=1"
            elif verb == "trace":
                value = "schema=1 queued=0 dropped=0"
            elif verb == "key K2":
                value = "accepted pending=1"
            else:
                self.fail(verb)
            port.feed(f"PP {ident} OK {value}\n".encode())
        session = self.start(Serial(reply), timeout=.1)
        with self.assertRaises(TimeoutError):
            refresh_scenario(session)

    def test_completed_but_undrained_tail_is_not_green(self):
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(2, begin=0).strip())
        session._line("PP 2 ENTRY kind=0 active=0")
        session._line("PP 2 OK queued=2 dropped=0 tx_failures=0")
        summary = self.finish()
        self.assertIn("undrained_events_at_end", summary["gaps"])
        self.assertEqual(finalize(self.output, {}, {"status": "pass"})["result"]["status"], "inconclusive")

    def test_entire_terminal_pair_lost_at_tail_is_detected(self):
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(2, begin=0).strip())
        session._line("PP 2 OK queued=0 dropped=0 tx_failures=0 sequence=4")
        self.assertIn("trace_tail_sequence_gap", self.finish()["gaps"])

    def test_compare_conditions(self):
        session = self.start()
        self.finish()
        base = finalize(self.output, {"missing": ["hardware"], "source_dirty": False}, {"status": "inconclusive"})
        path = self.output / "run.json"
        self.assertFalse(compare(path, path)["comparable"])
        base["metadata"].update(missing=[], hardware="Read Pico", source_dirty=False)
        path.write_text(json.dumps(base))
        # Empty sample runs are not a latency comparison.
        self.assertFalse(compare(path, path)["comparable"])
        base["metadata"].update(source_commit="base", source_sha256="basehash", provenance_verified=True,
                                workload="refresh-v1:K2x1", effective_sdkconfig="CONFIG_PM_ENABLE=y")
        base["analysis"].update(gaps=[], composition={"0:2:0": 2})
        base["analysis"]["distributions"] = {"0": {"n": 2, "p95_us": 100}}
        path.write_text(json.dumps(base))
        other = self.output / "other.json"
        base["metadata"]["source_commit"] = "candidate"
        base["metadata"]["source_sha256"] = "candidatehash"
        other.write_text(json.dumps(base))
        self.assertTrue(compare(path, other)["comparable"])
        self.assertFalse(compare(path, other)["same_source"])
        for key, value in (("workload", None), ("workload", "different-script"), ("provenance_verified", False)):
            changed = json.loads(json.dumps(base))
            changed["metadata"][key] = value
            other.write_text(json.dumps(changed))
            self.assertFalse(compare(path, other)["comparable"])
        changed = json.loads(json.dumps(base))
        changed["analysis"]["composition"] = {"2:0:1": 1}
        other.write_text(json.dumps(changed))
        self.assertFalse(compare(path, other)["comparable"])
        base["metadata"]["hardware"] = "Different board"
        other.write_text(json.dumps(base))
        self.assertFalse(compare(path, other)["comparable"])

    def test_cutoff_freezes_then_drains_tail(self):
        def reply(port, command):
            _, ident, verb = command
            if verb == "trace_stop":
                port.feed(f"PP {ident} ENTRY kind=0 active=0\nPP {ident} OK stopped=1 queued=2 sequence=2 dropped=0 tx_failures=0\n".encode())
            elif verb == "trace":
                port.feed((trace(1).replace("PP 1", f"PP {ident}") + trace(2, begin=0).replace("PP 1", f"PP {ident}") +
                           f"PP {ident} OK schema=1 stopped=1 queued=0 sequence=2 dropped=0 tx_failures=0\n").encode())
            else:
                self.fail(verb)
        session = self.start(Serial(reply))
        session.cutoff()
        self.assertFalse(self.finish()["gaps"])

    def test_quiet_metrics_without_device_cutoff_is_inconclusive(self):
        session = self.start()
        session._line(trace(1).strip())
        session._line(trace(2, begin=0).strip())
        session._line("PP 2 ENTRY kind=0 active=0")
        session._line("PP 2 OK queued=0 sequence=2 dropped=0 tx_failures=0")
        self.assertIn("device_cutoff_missing", self.finish()["gaps"])

    def test_stale_sdkconfig_is_reported_against_layered_defaults(self):
        from firmware_provenance import unapplied_defaults
        root = Path(self.temp.name)
        (root / "base").write_text("# note\nCONFIG_A=y\nCONFIG_B=y\nCONFIG_NAME=\"x\"\nCONFIG_OFF=n\n# CONFIG_UNSET is not set\n")
        (root / "mode").write_text("CONFIG_B=n\nCONFIG_C=y\n")
        (root / "sdkconfig").write_text("CONFIG_A=y\n# CONFIG_B is not set\nCONFIG_NAME=\"x\"\nCONFIG_UNSET=y\n")
        self.assertEqual(unapplied_defaults(root / "sdkconfig", [root / "base", root / "mode"]), ["CONFIG_C", "CONFIG_UNSET"])
        (root / "sdkconfig").write_text("CONFIG_A=y\nCONFIG_C=y\nCONFIG_NAME=\"x\"\n")
        self.assertEqual(unapplied_defaults(root / "sdkconfig", [root / "base", root / "mode"]), [])

    def test_build_provenance_rejects_stale_binary_and_source(self):
        from firmware_provenance import artifacts
        build = Path(self.temp.name) / "firmware"
        build.mkdir()
        for name in ("pico_page.bin", "pico_page.elf", "sdkconfig"):
            (build / name).write_bytes(b"built artifact")
        source = dict(source_commit="commit", source_dirty=False, source_sha256="source")
        manifest = dict(schema=1, **source, artifacts_sha256=artifacts(build)[0])
        args = argparse.Namespace(metadata=None, firmware=build, mode="refresh")
        with patch("device_workbench.source_state", return_value=source):
            self.assertIn("build_source_provenance", metadata_for(args)["missing"])
            (build / "provenance.json").write_text(json.dumps(manifest))
            self.assertTrue(metadata_for(args)["provenance_verified"])
            (build / "pico_page.bin").write_bytes(b"other build")
            self.assertFalse(metadata_for(args)["provenance_verified"])
            (build / "pico_page.bin").write_bytes(b"built artifact")
        with patch("device_workbench.source_state", return_value=dict(source, source_sha256="modified")):
            result = metadata_for(args)
            self.assertFalse(result["provenance_verified"])
            self.assertEqual(result["source_sha256"], "source")


if __name__ == "__main__":
    unittest.main()
