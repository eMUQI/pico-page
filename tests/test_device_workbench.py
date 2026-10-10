import json
from pathlib import Path
import queue
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from device_workbench import Session, analyze, compare, finalize, refresh_check, refresh_scenario


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


class WorkbenchTest(unittest.TestCase):
    def setUp(self):
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
        session = self.start(Serial(lambda p, _: p.feed(b"PP 1 ERROR busy\n")))
        with self.assertRaisesRegex(RuntimeError, "busy"):
            session.request("key K2")
        self.assertEqual(self.finish()["counts"]["command_failure"], 1)

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
                value = "accepted=1"
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
                value = "accepted=1"
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
        base["metadata"].update(source_commit="base", effective_sdkconfig="CONFIG_PM_ENABLE=y")
        base["analysis"]["distributions"] = {"0": {"n": 2, "p95_us": 100}}
        path.write_text(json.dumps(base))
        other = self.output / "other.json"
        base["metadata"]["source_commit"] = "candidate"
        other.write_text(json.dumps(base))
        self.assertTrue(compare(path, other)["comparable"])
        self.assertFalse(compare(path, other)["same_source"])
        base["metadata"]["hardware"] = "Different board"
        other.write_text(json.dumps(base))
        self.assertFalse(compare(path, other)["comparable"])


if __name__ == "__main__":
    unittest.main()
