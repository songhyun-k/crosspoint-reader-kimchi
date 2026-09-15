"""Offline catalog and generated font regression checks; no published assets assumed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import unittest
import zlib

try:
    import yaml
except ImportError:
    yaml = None

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "lib/EpdFont/scripts"
sys.path.insert(0, str(SCRIPTS))
from cpfont_version import CPFONT_VERSION, FONTS_MANIFEST_VERSION, font_release_base_url


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GEN = load("font_manifest_generator", ROOT / "scripts/generate-font-manifest.py")
SOURCE = load("korean_font_sources", SCRIPTS / "convert-korean-fonts.py")


class KoreanFontCatalogTest(unittest.TestCase):
    def test_sources_are_pinned_and_keep_the_original_notices(self):
        metadata = json.loads((SOURCE.BUILTINS / "source/korean-font-sources.json").read_text())
        for name, expected in SOURCE.SOURCES.items():
            self.assertEqual(hashlib.sha256((SOURCE.BUILTINS / "source" / name).read_bytes()).hexdigest(), expected)
            self.assertEqual(metadata["fonts"][name]["sha256"], expected)
        body = metadata["fonts"]["KoPub-Batang/KoPub Batang Light.ttf"]
        self.assertEqual((body["hangulSyllables"], body["hanja"]), (11172, 4620))
        self.assertTrue(any("Copyright" in text for text in body["sourceNamesAndNotices"]))

    def test_ui_interval_generation_is_exact_and_lossless(self):
        points = SOURCE.ks_x_1001_syllables()
        self.assertEqual(len(points), 2350)
        expanded = [cp for start, end in SOURCE.intervals(points) for cp in range(start, end + 1)]
        self.assertEqual(expanded, points)

    def test_catalog_keeps_upstream_families_and_only_the_available_kopub_style(self):
        if yaml is None:
            self.skipTest("Install lib/EpdFont/scripts/requirements.txt to inspect the generation catalog")
        catalog = yaml.safe_load((SCRIPTS / "sd-fonts.yaml").read_text())
        families = {f["name"]: f for f in catalog["families"]}
        self.assertIn("Literata", families)
        self.assertIn("NotoSansExtended", families)
        kopub = families["KoPubBatang"]
        self.assertEqual(kopub["sizes"], [12, 14, 16, 18])
        self.assertEqual(set(kopub["styles"]), {"regular"})
        descriptions, scripts, groups = GEN.load_catalog_from_yaml(SCRIPTS / "sd-fonts.yaml")
        self.assertIn("hangul", scripts["KoPubBatang"])
        self.assertTrue(descriptions["KoPubBatang"])
        self.assertIn("hangul", dict(groups))

    def test_local_fixture_uses_original_manifest_schema_and_flat_release_assets(self):
        manifest = json.loads((Path(__file__).parent / "fixtures/fonts.json").read_text())
        self.assertEqual(manifest["version"], FONTS_MANIFEST_VERSION)
        self.assertEqual(manifest["baseUrl"], font_release_base_url())
        families = {f["name"]: f for f in manifest["families"]}
        self.assertEqual(set(families), {"KoPubBatang", "NotoSansExtended"})
        self.assertEqual(families["KoPubBatang"]["styles"], ["regular"])
        self.assertEqual(families["NotoSansExtended"]["styles"], ["regular", "bold", "italic", "bolditalic"])
        for family in families.values():
            self.assertEqual(len(family["files"]), 4)
            for file in family["files"]:
                self.assertNotIn("/", file["name"])

    def test_local_assets_match_fixture_sizes_crc_styles_and_version(self):
        directory = os.getenv("KIMCHI_SD_FONT_FIXTURES")
        if not directory:
            self.skipTest("Set KIMCHI_SD_FONT_FIXTURES after running build-sd-fonts.py")
        manifest = json.loads((Path(__file__).parent / "fixtures/fonts.json").read_text())
        for family in manifest["families"]:
            for file in family["files"]:
                path = Path(directory) / family["name"] / file["name"]
                raw = path.read_bytes()
                self.assertEqual(len(raw), file["size"])
                self.assertEqual(zlib.crc32(raw), file["crc32"])
                self.assertEqual(int.from_bytes(raw[8:10], "little"), CPFONT_VERSION)
                self.assertEqual(GEN.read_cpfont_styles(path), family["styles"])

    def test_firmware_version_macros_match_the_generator(self):
        font = (ROOT / "lib/EpdFont/SdCardFont.h").read_text()
        manifest = (ROOT / "src/activities/settings/FontDownloadActivity.h").read_text()
        self.assertEqual(int(re.search(r"#define CPFONT_VERSION (\d+)", font)[1]), CPFONT_VERSION)
        self.assertEqual(int(re.search(r"#define FONTS_MANIFEST_VERSION (\d+)", manifest)[1]), FONTS_MANIFEST_VERSION)


if __name__ == "__main__":
    unittest.main()
