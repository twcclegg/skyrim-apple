#!/usr/bin/env python3
"""Check macos/runtime.conf against the vendored DXVK that reads it.

DXVK silently ignores unknown keys and unparsable values, so a typo would leave
the game on the dxvk-native defaults without any log error.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "macos/runtime.conf"
# Only the DXVK components compiled into the native dxvk_d3d9 library.
DXVK_COMPONENTS = ("d3d9", "dxso", "dxvk", "util")
OPTION_READ = re.compile(r'getOption<\s*([A-Za-z_:0-9]+)\s*>\s*\(\s*"([A-Za-z0-9._]+)"')
KEY_CHARS = re.compile(r"[A-Za-z0-9._]*")
TRISTATE = {"true", "false", "auto"}


def parse_user_config(text):
    """Mirror dxvk::parseUserConfigLine for the global (no [app]) section."""
    options = {}
    active = True
    for line in text.splitlines():
        line = line.lstrip(" \t\r")
        if line.startswith("["):
            active = False  # app sections never match the native executable name
            continue
        key = KEY_CHARS.match(line).group(0)
        rest = line[len(key):].lstrip(" \t\r")
        if not rest.startswith("="):
            continue
        value, inside = [], False
        for character in rest[1:].lstrip(" \t\r"):
            if not inside and character in " \t\r":
                break
            if character == '"':
                inside = not inside
            else:
                value.append(character)
        if active:
            options[key] = "".join(value)
    return options


def dxvk_option_types():
    types = {}
    for component in DXVK_COMPONENTS:
        for path in (ROOT / "third_party/dxvk/src" / component).rglob("*.cpp"):
            for kind, name in OPTION_READ.findall(path.read_text(errors="replace")):
                types[name] = kind
    return types


class RuntimeConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.options = parse_user_config(CONFIG.read_text())
        cls.types = dxvk_option_types()

    def test_every_key_is_read_by_vendored_dxvk(self):
        self.assertTrue(self.options)
        for key in self.options:
            self.assertIn(key, self.types, f"{key} is not an option of the vendored DXVK")

    def test_values_parse_as_dxvk_types(self):
        for key, value in self.options.items():
            kind = self.types[key]
            if kind.endswith("Tristate"):
                self.assertIn(value.lower(), TRISTATE, key)
            elif kind == "bool":
                self.assertIn(value.lower(), {"true", "false"}, key)
            elif kind in {"int32_t", "uint32_t", "int"}:
                self.assertRegex(value, r"^-?[0-9]+$", key)

    def test_shader_compiler_workers_stay_bounded(self):
        self.assertIn(int(self.options["dxvk.numCompilerThreads"]), (1, 2))

    def test_d3d9_float_emulation_enabled(self):
        # dxvk-native defaults this off; Skyrim's HDR/DOF shaders need D3D9 Inf/NaN clamping.
        self.assertEqual(self.types["d3d9.floatEmulation"], "Tristate")
        self.assertEqual(self.options["d3d9.floatEmulation"].lower(), "true")

    def test_dxvk_frame_limiter_stays_off(self):
        # DXVK's limiter sleeps under the D3D9 device lock and stalls Skyrim's
        # main-thread loading while the loading screen presents; pacing lives in
        # runtime/d3d9_bridge.cpp instead.
        self.assertNotIn("d3d9.maxFrameRate", self.options)

    def test_parser_matches_dxvk_grammar(self):
        parsed = parse_user_config('# c\n  a.b = "x y"  # t\nbad line\n[TESV.exe]\nc.d = 1\n')
        self.assertEqual(parsed, {"a.b": "x y"})


if __name__ == "__main__":
    unittest.main()
