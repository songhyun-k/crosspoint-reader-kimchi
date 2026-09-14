"""Validate editable Korean translation input with the firmware's real parser."""

import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gen_i18n", ROOT / "scripts/gen_i18n.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
TRANSLATIONS = ROOT / "lib/I18n/translations"
PRINTF = re.compile(r"%(?:\d+\$)?[-+#0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%]")


class KoreanTranslationsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.en = GEN.parse_yaml_file(str(TRANSLATIONS / "english.yaml"))
        cls.ko = GEN.parse_yaml_file(str(TRANSLATIONS / "korean.yaml"))

    def test_current_keys_are_complete_without_stale_ko_keys(self):
        keys = lambda data: {k for k in data if k.startswith("STR_")}
        self.assertEqual(keys(self.en), keys(self.ko))
        for key in keys(self.ko):
            self.assertTrue(self.ko[key].strip(), key)
        source_keys = re.findall(r"^(STR_\w+):", (TRANSLATIONS / "korean.yaml").read_text(), re.M)
        self.assertEqual(len(source_keys), len(set(source_keys)), "duplicate translation keys")

    def test_printf_arguments_and_prefix_spacing_match(self):
        for key, value in self.en.items():
            if not key.startswith("STR_"):
                continue
            with self.subTest(key=key):
                self.assertEqual(PRINTF.findall(value), PRINTF.findall(self.ko[key]))
                self.assertEqual(value.endswith(" "), self.ko[key].endswith(" "))

    def test_ui_hangul_is_within_the_agreed_2350_syllables(self):
        ks2350 = {bytes((high, low)).decode("euc_kr") for high in range(0xB0, 0xC9) for low in range(0xA1, 0xFF)}
        self.assertEqual(len(ks2350), 2350)
        for key, value in self.ko.items():
            missing = {c for c in value if 0xAC00 <= ord(c) <= 0xD7A3 and c not in ks2350}
            self.assertFalse(missing, f"{key}: {missing}")

    def test_generator_loads_english_and_korean_without_fallback(self):
        codes, names, tags, _, _, inherited = GEN.load_translations(str(TRANSLATIONS))
        self.assertEqual(codes, ["EN", "KO"])
        self.assertEqual(names, ["English", "한국어"])
        self.assertEqual(tags, ["en", "ko"])
        self.assertEqual(inherited, [set(), set()])

    def test_default_cli_generates_only_english_and_korean(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/gen_i18n.py"), str(TRANSLATIONS), tmp],
                cwd=ROOT, capture_output=True, text=True, check=True,
            )
            self.assertIn("Languages: 2", result.stdout)
            keys = (Path(tmp) / "I18nKeys.h").read_text()
            strings = (Path(tmp) / "I18nStrings.cpp").read_text()
            self.assertIn("STRINGS_KO_DATA", keys)
            self.assertIn("STRINGS_EN_DATA", strings)
            self.assertNotIn("STRINGS_FR_DATA", strings)
            self.assertNotIn("Language::FR", keys)


if __name__ == "__main__":
    unittest.main()
