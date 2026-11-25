#!/usr/bin/env python3
import importlib.util
import json
import subprocess
import threading
from types import SimpleNamespace
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_game

SPEC = importlib.util.spec_from_file_location(
    "autotests", Path(__file__).resolve().parents[2] / "tools/run_ps2autotests.py")
at = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(at)


class AutotestsHarness(unittest.TestCase):
    def test_live_request_transient_sharing_violation(self):
        request = Mock()
        request.read_text.side_effect = [PermissionError("sharing violation"),
                                         "3\n0x00114150\n"]
        with patch.object(run_game.time, "sleep") as sleep:
            self.assertEqual(run_game.read_live_request(request),
                             ["3", "0x00114150"])
            self.assertEqual(request.read_text.call_count, 2)
            sleep.assert_called_once_with(0.025)
        request.reset_mock()
        request.read_text.side_effect = PermissionError("persistent denial")
        with patch.object(run_game.time, "sleep") as sleep:
            with self.assertRaises(PermissionError):
                run_game.read_live_request(request)
            self.assertEqual(request.read_text.call_count, 40)
            self.assertEqual(sleep.call_count, 39)
        request.reset_mock()
        request.read_text.side_effect = ValueError("bad decoding")
        with patch.object(run_game.time, "sleep") as sleep:
            with self.assertRaises(ValueError):
                run_game.read_live_request(request)
            self.assertEqual(request.read_text.call_count, 1)
            sleep.assert_not_called()

    def test_format_stop(self):
        self.assertEqual(at.format_stop({"exit_name": "DISPATCH",
                         "final_pc": 0x110b88, "dispatches": 1000000,
                         "output_lines": 2}),
                         "DISPATCH pc=0x00110b88 dispatches=1000000 lines=2")
        self.assertEqual(at.format_stop({}), "- pc=- dispatches=- lines=-")

    def test_parallel_execution_keeps_manifest_order(self):
        cases = [{"name": name} for name in ("a", "b", "c", "d")]
        prepared = {case["name"]: (None, None, None) for case in cases}
        barrier = threading.Barrier(2)
        guard = threading.Lock()
        active = 0
        peak = 0

        def worker(case, args, manifest, run_dir, reference):
            nonlocal active, peak
            with guard:
                active += 1
                peak = max(peak, active)
            try:
                barrier.wait(timeout=10)
                return {"name": case["name"], "status": "PASS", "seconds": 0}
            finally:
                with guard:
                    active -= 1

        with patch.object(at, "run_case", side_effect=worker), patch("builtins.print"):
            results = at.run_cases(cases, SimpleNamespace(jobs=2), {}, None, prepared)
        self.assertEqual(peak, 2)
        self.assertEqual([result["name"] for result in results], ["a", "b", "c", "d"])

        def serial_worker(case, args, manifest, run_dir, reference):
            return {"name": case["name"], "status": "PASS", "seconds": 0}

        with patch.object(at, "run_case", side_effect=serial_worker), patch("builtins.print"):
            serial = at.run_cases(cases, SimpleNamespace(jobs=1), {}, None, prepared)
        self.assertEqual(results, serial)

    def test_suite_lock_excludes_other_processes_and_releases(self):
        program = (
            "import sys; from pathlib import Path; "
            "sys.path.insert(0, sys.argv[1]); "
            "from run_ps2autotests import suite_lock; "
            "lock = suite_lock(Path(sys.argv[2])); "
            "lock.__enter__(); lock.__exit__(None, None, None)"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = [sys.executable, "-c", program, str(at.ROOT / "tools"), directory]
            with at.suite_lock(root):
                blocked = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn("autotest directory is locked", blocked.stderr)
            released = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(released.returncode, 0, released.stderr)

    def test_reference_preflight_preserves_individual_errors(self):
        cases = [{"name": "a", "elf": {}, "expected": {}},
                 {"name": "b", "elf": {}, "expected": {}}]
        args = SimpleNamespace(cache=Path("cache"))
        manifest = {"repository": "owner/repo", "revision": "rev"}
        with patch.object(at, "verified_file",
                          side_effect=[ValueError("bad blob"), Path("b.elf"), Path("b.expected")]):
            prepared = at.prepare_references(cases, args, manifest)
        self.assertEqual(prepared["a"], (None, None, "bad blob"))
        self.assertEqual(prepared["b"], (Path("b.elf"), Path("b.expected"), None))
        with tempfile.TemporaryDirectory() as directory:
            cases[0].update(elf={"git_blob": "x"}, expected={"git_blob": "y"})
            with patch.object(at.subprocess, "run") as run:
                result = at.run_case(cases[0], args, manifest, Path(directory), prepared["a"])
            run.assert_not_called()
            self.assertEqual(result["status"], "ERROR")
            self.assertEqual(result["error"], "bad blob")

    def test_ci_full_matrix_covers_inventory(self):
        import re
        workflow = (at.ROOT / ".github/workflows/ps2autotests.yml").read_text()
        ctest_workflow = (at.ROOT / ".github/workflows/ci.yml").read_text()
        self.assertNotIn("autotests_full", ctest_workflow)
        self.assertNotIn("ee-ctest-build", workflow)
        self.assertIn("--target ee_recomp ee_game_runner", workflow)
        self.assertIn("--prebuilt-manifest build/autotest-tools.json", workflow)
        full, pilot = re.search(r"fromJSON\(inputs\.autotests_full && '([^']+)' \|\| '([^']+)'\)", workflow).groups()
        tests = json.loads(at.MANIFEST.read_text())["tests"]
        for selection, expected in ((full, 55), (pilot, 11)):
            groups = json.loads(selection)
            names = [case["name"] for group in groups
                     for case in at.select_tests(tests, group["patterns"].split())]
            self.assertEqual(len(names), expected)
            self.assertEqual(len(set(names)), expected)

    def test_prebuilt_tools_require_matching_revision_and_hashes(self):
        hashes = {"ee_recomp": "a", "ee_game_runner": "b"}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tools.json"
            path.write_text(json.dumps({"runtime_revision": "rev",
                                        "native_tool_sha256": hashes}), encoding="utf-8")
            at.verify_prebuilt_tools(path, "rev", hashes)
            with self.assertRaisesRegex(ValueError, "revision or SHA-256"):
                at.verify_prebuilt_tools(path, "other", hashes)
            with self.assertRaisesRegex(ValueError, "revision or SHA-256"):
                at.verify_prebuilt_tools(path, "rev", dict(hashes, ee_recomp="changed"))

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
