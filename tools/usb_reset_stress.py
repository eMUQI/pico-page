#!/usr/bin/env python3
"""Reset a connected device repeatedly and stop at the first USB or boot failure."""
import argparse
import contextlib
from dataclasses import dataclass
import io
import os
from pathlib import Path
import sys
import time

from device_console import open_serial

BOOTLOADER_LAST_LINE = b"Disabling RNG early entropy source"
PANEL_RAILS_OFF_AFTER_BOOT_S = 12
BOOT_FAILURE_MARKERS = (
    b"ESP_ERROR_CHECK failed:", b"abort() was called", b"Guru Meditation Error:",
    b"assert failed:", b"Stack smashing protect failure!", b"CORRUPT HEAP:",
    b"Task watchdog got triggered", b"Interrupt wdt timeout", b"Brownout detector was triggered",
    b"Rebooting...", b"Restarting system",
)


def boot_failure(boot_output):
    for marker in BOOT_FAILURE_MARKERS:
        if marker in boot_output:
            return "boot failure: " + marker.decode()
    if boot_output.count(BOOTLOADER_LAST_LINE + b"...") > 1:
        return "multiple bootloader handoffs in one reset cycle"
    _, handoff, application_output = boot_output.partition(BOOTLOADER_LAST_LINE + b"...")
    if handoff and (b"ESP-ROM:" in application_output or b"rst:0x" in application_output):
        return "unexpected reset after the bootloader handoff"
    for marker in (b"app_init: App version:", b"main_task: Calling app_main()"):
        if boot_output.count(marker) > 1:
            return "multiple application starts in one reset cycle"
    return None


def application_started(boot_output):
    _, handoff, application_output = boot_output.partition(BOOTLOADER_LAST_LINE + b"...")
    return boot_failure(boot_output) is None and bool(handoff) and bool(application_output.strip())


@dataclass
class ResetCapture:
    reset_output: str
    boot_output: bytes
    failure: str | None = None


def open_serial_before_reset(port, port_wait_s):
    deadline = time.monotonic() + port_wait_s
    while True:
        try:
            return open_serial(port)
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def create_boot_serial(port):
    if os.name != "posix":
        raise RuntimeError("Boot capture without input flushing requires a POSIX host")
    import serial

    class BootSerial(serial.Serial):
        def _reset_input_buffer(self):
            # POSIX Serial.open flushes input before setting is_open; later explicit flushes remain available.
            if self.is_open:
                super()._reset_input_buffer()

    connection = BootSerial(port=None, baudrate=115200, timeout=0.2, write_timeout=2, exclusive=True)
    connection.rts = True
    connection.dtr = True
    connection.port = port
    return connection


def open_boot_serial(connection, port_wait_s):
    deadline = time.monotonic() + port_wait_s
    while True:
        try:
            connection.open()
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)
    connection.rts = False
    connection.dtr = False


def capture_reset_cycle(port, port_wait_s, capture_s):
    import esptool

    reset_output = io.StringIO()
    boot_output = bytearray()
    connection = None
    boot_connection = None
    original_read = None
    failure = None
    phase = "download connection"
    with contextlib.redirect_stdout(reset_output), contextlib.redirect_stderr(reset_output):
        try:
            print(f"esptool v{esptool.__version__}")
            boot_connection = create_boot_serial(port)
            connection = open_serial_before_reset(port, port_wait_s)
            esp = esptool.CHIP_DEFS["esp32s3"](connection)
            esp.connect()
            if esp.CHIP_NAME != "ESP32-S3" or esp.secure_download_mode:
                raise RuntimeError("Expected ESP32-S3 without secure download mode")
            esp = esptool.run_stub(esp)
            esptool.chip_id(esp)
            connection.timeout = 0.2
            original_read = connection.read

            def record_read(size=1):
                data = original_read(size)
                boot_output.extend(data)
                return data

            # esptool's SLIP reader may prefetch boot output with a reset-command reply.
            connection.read = record_read
            phase = "watchdog reset"
            reset_started_ns = time.monotonic_ns()
            print(f"watchdog_reset_start_monotonic_ns={reset_started_ns}")
            esptool.reset_chip(esp, "watchdog-reset")
            phase = "serial reopen"
            connection.read = original_read
            original_read = None
            connection.close()
            connection = None
            reopen_started_ns = time.monotonic_ns()
            open_boot_serial(boot_connection, port_wait_s)
            capture_ready_ns = time.monotonic_ns()
            print(f"capture_ready_monotonic_ns={capture_ready_ns} "
                  f"reopen_elapsed_ms={(capture_ready_ns - reopen_started_ns) / 1000000:.3f} "
                  f"reset_to_capture_ready_ms={(capture_ready_ns - reset_started_ns) / 1000000:.3f}")
            phase = "serial capture"
            deadline = time.monotonic() + capture_s
            first_data = True
            while time.monotonic() < deadline:
                data = boot_connection.read(4096)
                if data and first_data:
                    print(f"first_capture_data_monotonic_ns={time.monotonic_ns()}")
                    first_data = False
                boot_output.extend(data)
        except Exception as exc:
            failure = f"{phase} failed: {type(exc).__name__}: {exc}"
            print(failure)
        except KeyboardInterrupt:
            failure = f"{phase} interrupted by operator"
            print(failure)
        finally:
            if connection is not None:
                if original_read is not None:
                    connection.read = original_read
                connection.close()
            if boot_connection is not None:
                boot_connection.close()
    return ResetCapture(reset_output.getvalue(), bytes(boot_output), failure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=300)
    parser.add_argument("--output", type=Path, required=True, help="new directory for every cycle's reset and boot logs")
    parser.add_argument("--port-wait", type=float, default=15,
                        help="seconds to wait for each port open, before download and after the watchdog reset")
    parser.add_argument("--capture", type=float, default=15,
                        help="seconds of boot output to read per cycle; a reset while the panel rails "
                             "are still up after the boot refresh darkens the screen")
    arguments = parser.parse_args()
    if arguments.cycles < 1:
        parser.error("--cycles must be at least 1")
    if arguments.capture < PANEL_RAILS_OFF_AFTER_BOOT_S:
        parser.error(f"--capture must be at least {PANEL_RAILS_OFF_AFTER_BOOT_S} seconds")
    arguments.output.mkdir(parents=True)

    def fail(cycle, reason, evidence):
        (arguments.output / f"cycle-{cycle}.log").write_bytes(evidence)
        print(f"FAIL cycle {cycle}/{arguments.cycles}: {reason}; device left untouched")
        return 1

    for cycle in range(1, arguments.cycles + 1):
        capture = capture_reset_cycle(arguments.port, arguments.port_wait, arguments.capture)
        (arguments.output / f"cycle-{cycle}-reset.log").write_text(capture.reset_output)
        boot_output = capture.boot_output
        (arguments.output / f"cycle-{cycle}.log").write_bytes(boot_output)
        if capture.failure:
            return fail(cycle, capture.failure, boot_output)
        if not application_started(boot_output):
            return fail(cycle, boot_failure(boot_output) or "no application output after the bootloader handoff", boot_output)
        print(f"cycle {cycle}/{arguments.cycles} ok", flush=True)
    print(f"PASS {arguments.cycles} resets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
