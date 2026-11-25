#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "autotests", Path(__file__).resolve().parents[2] / "tools/run_ps2autotests.py")
at = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(at)


class AutotestsHarness(unittest.TestCase):
    def test_inventory(self):
        manifest = json.loads(at.MANIFEST.read_text())
        self.assertEqual(len(manifest["tests"]), 55)
        self.assertEqual(len({x["name"] for x in manifest["tests"]}), 55)
        self.assertEqual(len(at.select_tests(manifest["tests"], ["cpu/ee/*"])), 6)
        self.assertEqual(len(at.select_tests(manifest["tests"], ["cpu-ee-alu"])), 1)

    def test_fragmented_byte_output(self):
        log = (b"EE KPUTS: -- TEST BE\nruntime: diagnostic\n"
               b"EE FILEIO: GIN\\x0afoo\\x5cx41\\x22\\xff\n"
               b"EE KPUTS: \\x0d\\x0a-- TEST END\\x0a\n")
        self.assertEqual(at.extract_guest(log), b'-- TEST BEGIN\nfoo\\x41"\xff\r\n-- TEST END\n')
        expected = b'-- TEST BEGIN\nfoo\\x41"\xff\n-- TEST END\n'
        self.assertEqual(at.judge(expected, at.extract_guest(log))[0], "PASS")

    def test_statuses(self):
        expected = b"-- TEST BEGIN\nvalue 1\n-- TEST END\n"
        self.assertEqual(at.judge(expected, b"")[0], "NO_OUTPUT")
        self.assertEqual(at.judge(expected, b"-- TEST BEGIN\nvalue 1\n")[0], "INCOMPLETE")
        status, _, diff = at.judge(expected, expected.replace(b"1", b"2"))
        self.assertEqual(status, "DIFF")
        self.assertIn("-value 1", diff)
        self.assertEqual(at.judge(expected, expected + b"\n")[0], "PASS")
        self.assertEqual(at.judge(expected, expected.replace(b"1", b"1 "))[0], "DIFF")
        with self.assertRaises(ValueError):
            at.judge(expected, expected + expected)
        with self.assertRaises(ValueError):
            at.extract_guest(b"EE KPUTS: \\zz\n")

    def test_minimal_disc_preserves_original_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "source.elf"
            payload = b"\x7fELF\x00\xff"
            elf.write_bytes(payload)
            disc = at.prepare_disc(elf, root / "disc")
            self.assertEqual((disc / "TEST.ELF").read_bytes(), payload)
            self.assertEqual(elf.read_bytes(), payload)
            self.assertIn(b"BOOT2 = cdrom0:\\TEST.ELF;1\r\n",
                          (disc / "SYSTEM.CNF").read_bytes())
            self.assertEqual({p.name for p in disc.iterdir()},
                             {"TEST.ELF", "SYSTEM.CNF"})

    def test_cache_verification(self):
        data = b"reference"
        entry = {"path": "tests/test.expected", "git_blob": at.blob_hash(data)}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / entry["path"]
            path.parent.mkdir()
            path.write_bytes(data)
            with patch.object(at.urllib.request, "urlopen") as download:
                self.assertEqual(at.verified_file(root, entry, "owner/repo", "rev"), path)
                download.assert_not_called()
                path.write_bytes(b"corrupt")
                download.return_value.__enter__.return_value.read.return_value = b"wrong"
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    at.verified_file(root, entry, "owner/repo", "rev")
                self.assertEqual(path.read_bytes(), b"corrupt")


if __name__ == "__main__":
    unittest.main()
