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

    def test_explicit_selection_survives_edits(self):
        source = 'schema_version=2\nselected_ssid="home" # target\n[[wifi]]\nssid="home"\npassword="password"\n'
        edited = tool.edited_wifi_toml(source, "phone", "phone-password")
        self.assertEqual(tomllib.loads(edited)["selected_ssid"], "home")
        self.assertIn('selected_ssid="home" # target', edited)
        edited = tool.edited_wifi_toml(edited, "home", "new-password")
        self.assertEqual(tomllib.loads(edited)["selected_ssid"], "home")
        legacy = source.replace('selected_ssid="home" # target\n', '')
        self.assertEqual(tomllib.loads(tool.edited_wifi_toml(legacy, "phone", "password"))["selected_ssid"], "home")

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
            self.assertEqual(parsed, {"schema_version": 2, "wifi": [{"ssid": ssid, "password": password}]})

    def test_wifi_edits_preserve_source(self):
        path = self.card / "PicoPage/network.toml"
        source = ("# reader's note\r\nschema_version = 2 # keep\r\n\r\n"
                  "# home note\r\n[[wifi]] # home\r\nssid = 'home'\r\npassword = 'old-password' # keep\r\n"
                  "last_connected = 1791300000\r\n\r\n# office note\r\n[[wifi]]\r\n"
                  "ssid = 'office'\r\npassword = ''\r\n")
        path.write_bytes(source.encode())
        original, edited = tool.prepared_wifi_edit(path, "home", "new-password")
        self.assertIn("password = \"new-password\" # keep\r\n", edited)
        self.assertIn("# office note\r\n[[wifi]]\r\n", edited)
        tool.write(path, edited, original)
        original, edited = tool.prepared_wifi_edit(path, "guest", "")
        self.assertIn("# reader's note\r\nschema_version = 2 # keep", edited)
        self.assertIn("# office note\r\n[[wifi]]", edited)
        tool.write(path, edited, original)
        self.assertEqual([item["ssid"] for item in tool.existing_networks(path)], ["home", "office", "guest"])
        original, edited = tool.prepared_wifi_edit(path, "fourth", "")
        path.write_text("schema_version = 2\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            tool.write(path, edited, original)
        self.assertEqual(path.read_text(), "schema_version = 2\n")
        path.write_text("# original\nschema_version = 1 # keep\n[wifi] # keep\n"
                        "ssid = 'legacy'\npassword = 'old-password' # keep\n", encoding="utf-8")
        original, edited = tool.prepared_wifi_edit(path, "legacy", "new-password")
        self.assertIn("schema_version = 2 # keep\nselected_ssid = \"legacy\"\n[[wifi]] # keep", edited)
        self.assertIn("password = \"new-password\" # keep", edited)
        self.assertEqual(tomllib.loads(edited)["wifi"][0]["password"], "new-password")
        path.unlink()

    def test_wifi_networks_are_merged(self):
        path = self.card / "PicoPage/network.toml"
        path.unlink(missing_ok=True)
        self.assertEqual(tool.existing_networks(path), [])
        path.write_text('schema_version = 1\n[wifi]\nssid = "old"\npassword = "old-password"\n', encoding="utf-8")
        networks = tool.existing_networks(path)
        self.assertEqual(networks, [{"ssid": "old", "password": "old-password"}])
        tool.write(path, tool.wifi_toml("new", "", networks))
        networks = tool.existing_networks(path)
        self.assertEqual([n["ssid"] for n in networks], ["old", "new"])
        path.write_text(tool.wifi_toml("old", "changed-password", networks).replace(
            'password = "changed-password"\n', 'password = "changed-password"\nlast_connected = 1791300000\n'), encoding="utf-8")
        networks = tool.existing_networks(path)
        self.assertEqual(networks[0], {"ssid": "old", "password": "changed-password", "last_connected": 1791300000})
        self.assertIn("last_connected = 1791300000", tool.wifi_toml("new", "", networks))
        for name in ("n3", "n4", "n5"):
            networks = tomllib.loads(tool.wifi_toml(name, "", networks))["wifi"]
        self.assertEqual(len(networks), 5)
        self.assertEqual(len(tomllib.loads(tool.wifi_toml("n5", "password", networks))["wifi"]), 5)
        with self.assertRaises(ValueError):
            tool.wifi_toml("n6", "", networks)
        for text in ["schema_version = 3\n", "schema_version = true\n", "not toml", "schema_version = 2\nwifi = 1\n",
                     'schema_version = 2\n[[wifi]]\nssid = "x"\n',
                     'schema_version = 2\nextra = "discarded"\n[[wifi]]\nssid = "x"\npassword = ""\n',
                     'schema_version = 2\n[[wifi]]\nssid = "x"\npassword = "short"\n',
                     'schema_version = 2\n[[wifi]]\nssid = "x"\npassword = ""\n[[wifi]]\nssid = "x"\npassword = ""\n',
                     'schema_version = 2\n[[wifi]]\nssid = "x"\npassword = ""\nlast_connected = -1\n',
                     'schema_version = 2\n' + ''.join(
                         f'[[wifi]]\nssid = "x{i}"\npassword = ""\n' for i in range(6))]:
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(ValueError, msg=text):
                tool.existing_networks(path)
        path.unlink()

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
