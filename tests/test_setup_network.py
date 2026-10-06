"""Checks that setup_network.py writes what the firmware parsers accept and refuses the rest."""

import os
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import setup_network as tool

# sync_test.cpp loads the card written here with the firmware parser and expects these values.
USERNAME = '读者 "a\\b"'
PASSWORD = "correct horse 电池"


class SetupNetworkTest(unittest.TestCase):
    def setUp(self):
        shared = os.environ.get("PICO_PAGE_TOOL_CARD")
        if shared:
            self.card = Path(shared)
        else:
            temp = tempfile.TemporaryDirectory()
            self.addCleanup(temp.cleanup)
            self.card = Path(temp.name)
        (self.card / "PicoPage").mkdir(parents=True, exist_ok=True)

    def test_sync_file_round_trips_and_keeps_device_identity(self):
        path = self.card / "PicoPage/sync.toml"
        tool.write(path, tool.sync_toml("https://sync.example.invalid/", USERNAME,
                                        tool.userkey(PASSWORD), "DEVICE-1", 20240114))
        parsed = tomllib.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(parsed, {
            "schema_version": 1, "endpoint": "https://sync.example.invalid", "username": USERNAME,
            "userkey": "55d2fddf8ba5768bbc44b083846cf439", "device_id": "DEVICE-1", "dom_version": 20240114})
        self.assertNotIn(PASSWORD, path.read_text(encoding="utf-8"))
        self.assertEqual(tool.existing_identity(path), ("DEVICE-1", 20240114))
        self.assertFalse(path.with_name("sync.toml.tmp").exists())
        self.assertEqual(tool.existing_identity(self.card / "PicoPage/missing.toml"), (None, None))

    def test_wifi_file_round_trips(self):
        for ssid, password in [('家 "2.4G"\\', "p\"a\\ss 密码"), ("open", ""), ("psk", "a" * 64)]:
            parsed = tomllib.loads(tool.wifi_toml(ssid, password))
            self.assertEqual(parsed, {"schema_version": 1, "wifi": {"ssid": ssid, "password": password}})

    def test_values_the_firmware_rejects_are_refused(self):
        for ssid, password in [("", "12345678"), ("x" * 33, "12345678"), ("家" * 11, "12345678"),
                               ("net", "short"), ("net", "g" * 64), ("net", "a" * 65), ("a\tb", "12345678")]:
            with self.assertRaises(ValueError, msg=(ssid, password)):
                tool.wifi_toml(ssid, password)
        for endpoint in ["http://sync.example", "https://", "https:///x", "https://user@host", "https://a b",
                         "https://host?x=1", "https://主机"]:
            with self.assertRaises(ValueError, msg=endpoint):
                tool.sync_toml(endpoint, "user", "key", "device", 20260812)
        for username, device, version in [("", "d", 20260812), ("u\n", "d", 20260812), ("u", "", 20260812),
                                          ("u", "d", 20200101)]:
            with self.assertRaises(ValueError):
                tool.sync_toml("https://sync.example", username, "key", device, version)


if __name__ == "__main__":
    unittest.main()
