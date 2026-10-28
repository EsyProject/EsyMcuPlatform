"""Keep mathematical punctuation distinct from Han-script source text."""
from pathlib import Path
import subprocess
import tempfile
import unittest

CHECK = Path(__file__).with_name("check_source_language.sh")


class LanguageTests(unittest.TestCase):
    def test_math_is_accepted_and_han_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sample.c"
            for text, accepted in (("/* A \u00b7 x; \u03c6(x) */\n", True),
                                   ("/* \u4e2d\u6587 */\n", False)):
                source.write_text(text)
                result = subprocess.run([str(CHECK), directory], capture_output=True, text=True)
                self.assertEqual(accepted, result.returncode == 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
