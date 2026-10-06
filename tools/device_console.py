#!/usr/bin/env python3
"""Operate a locally connected diagnostic build without resetting the device."""
import argparse
from pathlib import Path
import time
import zlib


class Console:
    def __init__(self, port, timeout):
        import serial
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
        # Match IDF Monitor's no-reset sequence; releasing DTR first resets USB Serial/JTAG.
        self.serial.rts = True
        self.serial.dtr = True
        self.serial.port = port
        self.serial.open()
        self.serial.rts = False
        self.serial.dtr = False
        self.timeout = timeout
        self.identifier = 0
        # USB can enumerate before the application installs its input driver.
        deadline = time.monotonic() + timeout
        self.timeout = min(timeout, 2)
        try:
            while True:
                try:
                    self.request("status")
                    break
                except TimeoutError:
                    if time.monotonic() >= deadline:
                        raise
        except BaseException:
            self.serial.close()
            raise
        finally:
            self.timeout = timeout

    def request(self, command):
        self.identifier += 1
        request = f"PP {self.identifier} {command}\n".encode("utf-8")
        if len(request) > 4224 or any(c < 32 or c == 127 for c in request[:-1]):
            raise ValueError("Command exceeds the protocol limit or contains control characters")
        self.serial.write(request)
        prefix = f"PP {self.identifier} "
        deadline = time.monotonic() + self.timeout
        entries = []
        pending = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if not chunk:
                continue
            pending.extend(chunk)
            if len(pending) > 4096:
                raise RuntimeError("Device response line exceeds the protocol limit")
            if chunk != b"\n":
                continue
            line = pending.decode("utf-8", errors="replace").strip()
            pending.clear()
            if not line.startswith(prefix):
                continue
            value = line[len(prefix):]
            if value.startswith("ERROR "):
                raise RuntimeError(value)
            if value.startswith("ENTRY "):
                entries.append(value)
                if len(entries) > 32:
                    raise RuntimeError("Device returned too many directory entries")
            elif value == "OK" or value.startswith("OK "):
                fields = dict(item.split("=", 1) for item in value.split()[1:] if "=" in item)
                return fields, entries
        raise TimeoutError("No diagnostic reply; verify that the diagnostic build is running")

    def settled(self):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            state, _ = self.request("status")
            if state.get("pending") == "0":
                return state
            time.sleep(0.1)
        raise TimeoutError("The application still has a pending frame")

    def capture(self, output):
        if output.exists():
            raise FileExistsError(output)
        state = self.settled()
        if state.get("frame_ok") != "1" or int(state.get("bytes", "0")) != 415872:
            raise RuntimeError("No confirmed 684 x 1216 frame is available")
        frame = int(state["frame"])
        packed = bytearray()
        while len(packed) < 415872:
            offset = len(packed)
            count = min(256, 415872 - offset)
            reply, _ = self.request(f"screen {frame} {offset} {count}")
            data = bytes.fromhex(reply.get("hex", ""))
            if (int(reply.get("frame", "-1")) != frame or int(reply.get("offset", "-1")) != offset
                    or int(reply.get("count", "-1")) != count or len(data) != count):
                raise RuntimeError("Frame chunk identity or length mismatch")
            packed.extend(data)
        final, _ = self.request("status")
        if (final.get("pending") != "0" or final.get("frame_ok") != "1"
                or final.get("frame") != state["frame"] or final.get("crc32") != state["crc32"]
                or zlib.crc32(packed) != int(state["crc32"], 16)):
            raise RuntimeError("Frame changed during capture or its checksum does not match")
        gray = bytearray(684 * 1216)
        for index, value in enumerate(packed):
            gray[index * 2] = (value & 15) * 17
            gray[index * 2 + 1] = (value >> 4) * 17
        with output.open("xb") as target:
            target.write(b"P5\n684 1216\n255\n")
            target.write(gray)
        return state

    def upload(self, source, destination):
        """Import a new resource, verifying the committed file without overwriting existing files."""
        size = source.stat().st_size
        if not source.is_file() or not 1 <= size <= 64 * 1024 * 1024:
            raise ValueError("Resource must be a regular file of 1 byte through 64 MiB")
        with source.open("rb") as data:
            crc = 0
            remaining = size
            while remaining:
                chunk = data.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise RuntimeError("Source file changed before upload")
                crc = zlib.crc32(chunk, crc)
                remaining -= len(chunk)
            if data.read(1):
                raise RuntimeError("Source file grew before upload")
            data.seek(0)
            started = False
            try:
                reply, _ = self.request(f"begin {size} {crc:08x} {destination}")
                started = True
                if reply.get("ready") != "1" or reply.get("block") != "2048":
                    raise RuntimeError("Device did not accept the import parameters")
                sent = 0
                while sent < size:
                    chunk = data.read(min(2048, size - sent))
                    if not chunk:
                        raise RuntimeError("Source file changed during upload")
                    reply, _ = self.request(f"data {sent} {chunk.hex()}")
                    sent += len(chunk)
                    if int(reply.get("received", "-1")) != sent:
                        raise RuntimeError("Device import offset differs from the submitted bytes")
                if data.read(1):
                    raise RuntimeError("Source file changed during upload")
                reply, _ = self.request("commit")
                started = False
                if reply.get("committed") != "1":
                    raise RuntimeError("Device did not confirm resource publication")
                actual, _ = self.request(f"file {destination}")
                if int(actual.get("bytes", "-1")) != size or int(actual.get("crc32", "-1"), 16) != crc:
                    raise RuntimeError("Committed resource length or CRC32 does not match")
                return actual
            except BaseException:
                if started:
                    try:
                        self.request("abort")
                    except Exception:
                        pass
                raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=30)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    listing = commands.add_parser("ls")
    listing.add_argument("path", nargs="?", default=".")
    listing.add_argument("--offset", type=int, default=0)
    key = commands.add_parser("key")
    key.add_argument("key", choices=("K1", "K2", "K3"))
    tap = commands.add_parser("tap")
    tap.add_argument("x", type=int)
    tap.add_argument("y", type=int)
    opening = commands.add_parser("open")
    opening.add_argument("path")
    inspection = commands.add_parser("file")
    inspection.add_argument("path", help="Inspect a permitted EPUB, font, license or image resource")
    uploading = commands.add_parser("upload")
    uploading.add_argument("source", type=Path)
    uploading.add_argument("destination", help="New card-relative resource path; existing files are preserved")
    commands.add_parser("abort")
    capture = commands.add_parser("capture")
    capture.add_argument("output", type=Path, help="New binary PGM file; existing files are preserved")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 120:
        parser.error("timeout must be between 1 and 120 seconds")
    console = Console(args.port, args.timeout)
    try:
        if args.command == "upload":
            state = console.upload(args.source, args.destination)
            print(f"Imported and verified {state['bytes']} bytes; CRC32 {state['crc32']}")
        elif args.command == "capture":
            state = console.capture(args.output)
            print(f"Saved confirmed application frame {state['frame']} to {args.output}")
        else:
            command = args.command
            if command == "ls": command += f" {args.offset} {args.path}"
            elif command == "key": command += f" {args.key}"
            elif command == "tap": command += f" {args.x} {args.y}"
            elif command in ("open", "file"): command += f" {args.path}"
            state, entries = console.request(command)
            for entry in entries:
                print(entry)
            if state.get("pending") == "1":
                state = console.settled()
            print(" ".join(f"{key}={value}" for key, value in state.items()))
    finally:
        console.serial.close()


if __name__ == "__main__":
    main()
