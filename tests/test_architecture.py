"""Keep board implementation dependencies out of application consumers."""
from pathlib import Path
import csv
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ROOT / "firmware/components"


class PlatformBoundaries(unittest.TestCase):
    def test_application_consumers_do_not_import_board_implementation(self):
        consumers = [ROOT / "firmware/main"] + [
            path for path in COMPONENTS.iterdir()
            if path.is_dir() and path.name != "platform_read_pico"
        ]
        for directory in consumers:
            for path in directory.rglob("*"):
                if "third_party" in path.parts or not path.is_file():
                    continue
                if path.name != "CMakeLists.txt" and path.suffix not in {".h", ".hpp", ".c", ".cc", ".cpp", ".cxx"}:
                    continue
                with self.subTest(path=path.relative_to(ROOT)):
                    source = path.read_text(encoding="utf-8")
                    self.assertNotRegex(source, r"\bplatform_read_pico\b|\bread_pico\.h\b|\bepd_highlevel\.h\b")
                    self.assertNotRegex(source, r'"read_pico"', "board identity belongs to the selected build profile")
                    if path.name == "CMakeLists.txt":
                        self.assertNotRegex(source, r"\b(read_pico|read_pico_pmu|epdiy)\b", "BSP dependencies belong to the provider")

    def test_contract_has_no_sdk_or_board_headers(self):
        headers = list((COMPONENTS / "platform_contract/include").rglob("*.h"))
        self.assertTrue(headers, "platform contract must be independently consumable")
        for path in headers:
            for include in re.findall(r'^#include\s+[<"]([^>"]+)', path.read_text(encoding="utf-8"), re.M):
                with self.subTest(header=path.name, include=include):
                    self.assertFalse(include.startswith(("esp_", "freertos/", "driver/", "sdkconfig", "read_pico")))
                    self.assertNotIn("display_network_gate", include)

    def test_provider_does_not_depend_on_application_input_policy(self):
        provider = COMPONENTS / "platform_read_pico"
        self.assertNotRegex((provider / "CMakeLists.txt").read_text(encoding="utf-8"), r"\breader_input\b")
        for path in provider.rglob("*.cpp"):
            self.assertNotRegex(path.read_text(encoding="utf-8"), r'pico_page/(released_tap|sleep_input)\.h')

    def test_update_limit_matches_both_shipped_ota_slots(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            script = directory / "profile.cmake"
            script.write_text(
                f'include("{(ROOT / "boards/read_pico/profile.cmake").as_posix()}")\n'
                'file(WRITE "slot_bytes.txt" "${PICO_PAGE_UPDATE_SLOT_BYTES}")\n', encoding="utf-8")
            subprocess.run([os.environ.get("PICO_PAGE_CMAKE", "cmake"), "-P", str(script)], cwd=directory, check=True, capture_output=True)
            limit = int((directory / "slot_bytes.txt").read_text(encoding="utf-8"))
        self.assertGreater(limit, 0)
        lines = (ROOT / "firmware/partitions.build.csv").read_text(encoding="utf-8").splitlines()
        slots = {}
        for row in csv.reader(line for line in lines if line.strip() and not line.lstrip().startswith("#")):
            _, kind, subtype, _, size, *_ = (field.strip() for field in row)
            if kind == "app" and subtype in {"ota_0", "ota_1"}:
                suffix = size[-1].upper()
                value = int(size[:-1], 0) * {"K": 1024, "M": 1024 * 1024}[suffix] if suffix in "KM" else int(size, 0)
                self.assertNotIn(subtype, slots)
                slots[subtype] = value
        self.assertEqual(slots, {"ota_0": limit, "ota_1": limit})


if __name__ == "__main__":
    unittest.main()
