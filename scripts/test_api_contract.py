"""Public command manifest and integration-suite contract."""

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class APIContractTests(unittest.TestCase):
    def test_single_unified_command(self):
        manifest = json.loads((ROOT / "get-icon/manifest.json").read_text())
        self.assertEqual(manifest["commands"], [{
            "theme": "Filesystem icons",
            "syntax": "Path Get icon(&J;&L;&L):P",
            "threadSafe": False,
        }])

    def test_integration_methods_use_only_unified_command(self):
        methods = ROOT / "get-icon/get-icon-test/Project/Sources/Methods"
        for path in methods.glob("*.4dm"):
            with self.subTest(method=path.name):
                source = path.read_text()
                self.assertNotIn("File Get icon(", source)
                self.assertNotIn("Folder Get icon(", source)
        self.assertIn("Path Get icon(", (methods / "test_icons.4dm").read_text())


if __name__ == "__main__":
    unittest.main()
