import contextlib
import io
import itertools
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import usb_reset_stress as stress


HANDOFF = b"I (787) boot: Disabling RNG early entropy source...\r\n"
STARTED = HANDOFF + b"I (789) octal_psram: vendor id\r\n"


class UsbResetStressTests(unittest.TestCase):
    def test_application_output_requires_bootloader_handoff(self):
        for output in (b"", HANDOFF, HANDOFF + b" \r\n", b"I (789) octal_psram: vendor id\r\n"):
            with self.subTest(output=output):
                self.assertFalse(stress.application_started(output))
        self.assertTrue(stress.application_started(STARTED))

    def test_crash_followed_by_success_is_rejected(self):
        for marker in stress.BOOT_FAILURE_MARKERS:
            with self.subTest(marker=marker):
                output = STARTED + marker + b"\r\n" + STARTED
                self.assertFalse(stress.application_started(output))
                self.assertIn(marker.decode(), stress.boot_failure(output))

    def test_restarts_without_a_captured_panic_are_rejected(self):
        outputs = (
            STARTED + STARTED,
            STARTED + b"ESP-ROM:esp32s3-20210327\r\n",
            STARTED + b"rst:0xc (RTC_SW_CPU_RST),boot:0x8\r\n",
            STARTED + b"app_init: App version: 0.6.0\r\n" * 2,
            STARTED + b"main_task: Calling app_main()\r\n" * 2,
        )
        for output in outputs:
            with self.subTest(output=output):
                self.assertFalse(stress.application_started(output))
                self.assertIsNotNone(stress.boot_failure(output))
        self.assertTrue(stress.application_started(b"ESP-ROM:esp32s3-20210327\r\n" + STARTED))

    def capture_cycle(self, reset_action=None, chunks=(), connect_error=None, reopen_error=None):
        events = []
        download = Mock()
        download.read = Mock(return_value=b"")
        download.close.side_effect = lambda: events.append("download close")
        original_read = download.read
        boot = Mock()
        pending_chunks = iter(chunks)

        def read(size):
            events.append("boot read")
            data = next(pending_chunks, b"")
            if isinstance(data, Exception):
                raise data
            return data

        boot.read.side_effect = read
        boot.open.side_effect = reopen_error or (lambda: events.append("boot open"))
        esp = Mock(CHIP_NAME="ESP32-S3", secure_download_mode=False)
        if connect_error:
            esp.connect.side_effect = connect_error
        esptool = Mock(__version__="5.3.1")
        esptool.CHIP_DEFS = {"esp32s3": Mock(return_value=esp)}
        esptool.run_stub.side_effect = lambda loader: loader
        esptool.reset_chip.side_effect = reset_action(download) if reset_action else None
        with patch.dict(sys.modules, {"esptool": esptool}), \
                patch.object(stress, "create_boot_serial", return_value=boot), \
                patch.object(stress, "open_serial", return_value=download) as opened, \
                patch.object(stress.time, "sleep"), \
                patch.object(stress.time, "monotonic", side_effect=itertools.count(step=.25)):
            result = stress.capture_reset_cycle("unused", 1, 2)
        return result, SimpleNamespace(download=download, original_read=original_read, boot=boot,
                                       esp=esp, esptool=esptool, opened=opened, events=events)

    def test_failure_split_across_reads_retains_the_full_capture(self):
        chunks = [STARTED + b"ab", b"ort() was called\r\nBacktrace: test\r\n" + STARTED]
        result, fixture = self.capture_cycle(chunks=chunks)
        self.assertIsNone(result.failure)
        self.assertEqual(result.boot_output, b"".join(chunks))
        self.assertFalse(stress.application_started(result.boot_output))
        fixture.opened.assert_called_once_with("unused")
        fixture.esptool.reset_chip.assert_called_once()
        fixture.download.close.assert_called_once()
        fixture.boot.open.assert_called_once()
        fixture.boot.close.assert_called_once()
        self.assertIs(fixture.download.read, fixture.original_read)
        self.assertEqual(fixture.download.timeout, .2)
        self.assertEqual(fixture.events[:3], ["download close", "boot open", "boot read"])
        for field in ("watchdog_reset_start_monotonic_ns=", "capture_ready_monotonic_ns=",
                      "reopen_elapsed_ms=", "reset_to_capture_ready_ms=", "first_capture_data_monotonic_ns="):
            self.assertIn(field, result.reset_output)

    def test_reset_response_read_ahead_keeps_startup_bytes_before_reopen(self):
        response = b"\xc0reply\xc0" + HANDOFF

        def reset_action(connection):
            connection.read.return_value = response

            def reset(esp, mode):
                self.assertEqual(mode, "watchdog-reset")
                self.assertEqual(connection.timeout, .2)
                data = connection.read(4096)
                self.assertEqual(data.split(b"\xc0")[1], b"reply")
                print("watchdog reset completed")
            return reset

        result, fixture = self.capture_cycle(reset_action, [b"late boot log\r\n"])
        self.assertIsNone(result.failure)
        self.assertEqual(result.boot_output, response + b"late boot log\r\n")
        self.assertTrue(stress.application_started(result.boot_output))
        self.assertIn("watchdog reset completed", result.reset_output)
        fixture.esptool.CHIP_DEFS["esp32s3"].assert_called_once_with(fixture.download)
        fixture.original_read.assert_called_once_with(4096)
        fixture.download.reset_input_buffer.assert_not_called()
        fixture.boot.reset_input_buffer.assert_not_called()
        self.assertIs(fixture.download.read, fixture.original_read)

    def test_download_failure_closes_without_reset_or_capture(self):
        result, fixture = self.capture_cycle(connect_error=RuntimeError("no download reply"))
        self.assertIn("download connection failed", result.failure)
        self.assertIn("no download reply", result.reset_output)
        self.assertEqual(result.boot_output, b"")
        fixture.opened.assert_called_once()
        fixture.esp.connect.assert_called_once()
        fixture.esptool.reset_chip.assert_not_called()
        fixture.original_read.assert_not_called()
        fixture.download.close.assert_called_once()
        fixture.boot.open.assert_not_called()
        fixture.boot.read.assert_not_called()
        fixture.boot.close.assert_called_once()

    def test_capture_disconnect_keeps_bytes_and_does_not_reopen_or_reset(self):
        result, fixture = self.capture_cycle(chunks=[STARTED, OSError("disconnected")])
        self.assertIn("serial capture failed", result.failure)
        self.assertEqual(result.boot_output, STARTED)
        fixture.opened.assert_called_once()
        fixture.esptool.reset_chip.assert_called_once()
        fixture.download.close.assert_called_once()
        fixture.boot.open.assert_called_once()
        fixture.boot.close.assert_called_once()
        self.assertIs(fixture.download.read, fixture.original_read)

    def test_watchdog_failure_keeps_prefetched_bytes_without_reopening_or_resetting(self):
        def reset_action(connection):
            connection.read.return_value = STARTED

            def reset(esp, mode):
                connection.read(4096)
                raise RuntimeError("watchdog command failed")
            return reset

        result, fixture = self.capture_cycle(reset_action)
        self.assertIn("watchdog reset failed", result.failure)
        self.assertEqual(result.boot_output, STARTED)
        fixture.opened.assert_called_once()
        fixture.esptool.reset_chip.assert_called_once()
        fixture.download.close.assert_called_once()
        fixture.boot.open.assert_not_called()
        fixture.boot.close.assert_called_once()
        self.assertIs(fixture.download.read, fixture.original_read)

    def test_reopen_timeout_stops_without_another_reset(self):
        result, fixture = self.capture_cycle(reopen_error=OSError("port absent"))
        self.assertIn("serial reopen failed", result.failure)
        fixture.esptool.reset_chip.assert_called_once()
        fixture.boot.read.assert_not_called()
        self.assertGreater(fixture.boot.open.call_count, 1)
        fixture.download.close.assert_called_once()
        fixture.boot.close.assert_called_once()

    def test_expected_reopen_retries_until_available_then_captures(self):
        result, fixture = self.capture_cycle(chunks=[STARTED], reopen_error=[OSError("port absent"), None])
        self.assertIsNone(result.failure)
        self.assertEqual(result.boot_output, STARTED)
        self.assertEqual(fixture.boot.open.call_count, 2)
        fixture.esptool.reset_chip.assert_called_once()
        fixture.download.close.assert_called_once()
        fixture.boot.close.assert_called_once()

    def test_boot_open_preserves_input_but_explicit_flush_remains_available(self):
        class Serial:
            def __init__(self, **kwargs):
                self.is_open = False
                self.buffer = STARTED
                self.flushes = 0
                self.options = kwargs

            def open(self):
                self._reset_input_buffer()
                self.is_open = True

            def _reset_input_buffer(self):
                self.buffer = b""
                self.flushes += 1

            def reset_input_buffer(self):
                self._reset_input_buffer()

        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=Serial)}), \
                patch.object(stress.os, "name", "posix"):
            connection = stress.create_boot_serial("unused")
        self.assertEqual(connection.options["timeout"], .2)
        self.assertTrue(connection.options["exclusive"])
        self.assertTrue(connection.rts)
        self.assertTrue(connection.dtr)
        stress.open_boot_serial(connection, 1)
        self.assertEqual(connection.buffer, STARTED)
        self.assertEqual(connection.flushes, 0)
        self.assertFalse(connection.rts)
        self.assertFalse(connection.dtr)
        connection.reset_input_buffer()
        self.assertEqual(connection.buffer, b"")
        self.assertEqual(connection.flushes, 1)

    def test_unsupported_host_fails_before_opening_or_resetting_device(self):
        esptool = Mock(__version__="5.3.1")
        with patch.dict(sys.modules, {"esptool": esptool}), \
                patch.object(stress.os, "name", "nt"), \
                patch.object(stress, "open_serial") as opened:
            result = stress.capture_reset_cycle("unused", 1, 2)
        self.assertIn("requires a POSIX host", result.failure)
        self.assertEqual(result.boot_output, b"")
        opened.assert_not_called()
        esptool.reset_chip.assert_not_called()

    def run_cycles(self, output, captures, cycles=3):
        arguments = ["usb_reset_stress.py", "--port", "unused", "--cycles", str(cycles),
                     "--output", str(output)]
        with patch.object(sys, "argv", arguments), \
                patch.object(stress, "capture_reset_cycle", side_effect=captures) as capture, \
                contextlib.redirect_stdout(io.StringIO()) as printed:
            result = stress.main()
        return result, capture, printed.getvalue()

    def test_first_crashed_cycle_stops_before_next_reset_and_saves_both_cycles(self):
        crashed = STARTED + b"abort() was called\r\nRebooting...\r\n" + STARTED
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "capture"
            result, capture, printed = self.run_cycles(output, [
                stress.ResetCapture("download connected\n", STARTED),
                stress.ResetCapture("download connected\n", crashed),
            ])
            self.assertEqual(result, 1)
            self.assertEqual(capture.call_count, 2)
            self.assertIn("FAIL cycle 2/3", printed)
            self.assertNotIn("PASS", printed)
            self.assertEqual((output / "cycle-1.log").read_bytes(), STARTED)
            self.assertEqual((output / "cycle-2.log").read_bytes(), crashed)
            self.assertEqual((output / "cycle-2-reset.log").read_text(), "download connected\n")

    def test_io_failure_stops_even_with_application_output(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "capture"
            result, capture, printed = self.run_cycles(output, [
                stress.ResetCapture("connect\nno response\n", STARTED, "serial capture failed"),
            ])
            self.assertEqual(result, 1)
            capture.assert_called_once()
            self.assertIn("FAIL cycle 1/3", printed)
            self.assertEqual((output / "cycle-1-reset.log").read_text(), "connect\nno response\n")
            self.assertEqual((output / "cycle-1.log").read_bytes(), STARTED)

    def test_success_keeps_every_log_and_does_not_add_another_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "capture"
            result, capture, printed = self.run_cycles(output, [stress.ResetCapture("reset", STARTED)] * 3)
            self.assertEqual(result, 0)
            self.assertEqual(capture.call_count, 3)
            self.assertEqual(len(list(output.iterdir())), 6)
            self.assertIn("PASS 3 resets", printed)


if __name__ == "__main__":
    unittest.main()
