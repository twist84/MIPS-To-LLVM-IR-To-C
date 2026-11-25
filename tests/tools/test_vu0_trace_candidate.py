#!/usr/bin/env python3
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL_ARGS = sys.argv[1:]
sys.argv = [sys.argv[0]]

EXPECTED_PROGRAM_SHA256 = (
    "dcdea3b75c30dc0275e3a40d6466c411838cda40733841cb33646d3de11bc65f"
)
EXPECTED_PAIR_PCS = [
    0, 8, 16, 24, 32, 40, 48, 56, 64,
    80, 88, 104, 112, 120, 128, 136,
    168, 176, 192, 200, 208, 216, 224, 232, 240, 248,
    256, 264, 280, 288,
    296, 304, 312, 320, 328, 336, 344, 352, 360, 368, 376,
    392, 400, 408, 416, 424, 440, 448, 456,
    464, 472, 480, 488, 496, 504, 512, 520, 528, 536, 544, 552, 560, 568,
]


def executed_operation(lower):
    primary = (lower >> 25) & 0x7f
    if primary == 0x00:
        return "LQ"
    if primary == 0x01:
        return "SQ"
    if primary == 0x04:
        return "ILW"
    if primary == 0x05:
        return "ISW"
    if primary == 0x08:
        return "IADDIU"
    if primary == 0x09:
        return "ISUBIU"
    if primary == 0x40:
        secondary = lower & 0x3f
        if (lower >> 6) & 0x1f == 13:
            return {0x3c: "LQI", 0x3d: "SQI", 0x3e: "LQD", 0x3f: "SQD"}.get(
                secondary, "UNKNOWN"
            )
        return {
            0x30: "IADD",
            0x31: "ISUB",
            0x32: "IADDI",
            0x34: "IAND",
            0x35: "IOR",
        }.get(secondary, "UNKNOWN")
    return {
        0x20: "B",
        0x21: "BAL",
        0x24: "JR",
        0x25: "JALR",
        0x28: "IBEQ",
        0x29: "IBNE",
        0x2c: "IBLTZ",
        0x2d: "IBGTZ",
        0x2e: "IBLEZ",
        0x2f: "IBGEZ",
    }.get(primary, "UNKNOWN")


class VU0CandidateTraceTests(unittest.TestCase):
    def test_candidate_trace_covers_fixture_and_comparator(self):
        if len(TOOL_ARGS) != 2:
            self.fail("expected candidate executable and comparator script paths")

        candidate_exe = Path(TOOL_ARGS[0]).resolve()
        comparator = Path(TOOL_ARGS[1]).resolve()
        self.assertTrue(candidate_exe.is_file(), candidate_exe)
        self.assertTrue(comparator.is_file(), comparator)

        with tempfile.TemporaryDirectory() as directory:
            trace_path = Path(directory) / "candidate.jsonl"
            result = subprocess.run(
                [str(candidate_exe), str(trace_path)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(trace_path.is_file())

            records = [
                json.loads(line)
                for line in trace_path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]

            self.assertGreaterEqual(len(records), 2)
            header = records[0]
            steps = records[1:]
            self.assertEqual(header["schema"], "mips-to-llvm-vu0-trace-v1")
            self.assertEqual(header["kind"], "header")
            self.assertEqual(header["program_sha256"], EXPECTED_PROGRAM_SHA256)
            self.assertEqual(len(header["initial_state"]["vi"]), 32)
            self.assertEqual(len(header["initial_state"]["vf"]), 32)
            self.assertEqual(header["initial_state"]["vf"][0], [0, 0, 0, 0x3f800000])
            self.assertEqual(header["initial_state"]["vi"][29], 1)
            self.assertEqual(
                header["initial_state"]["data_sha256"],
                hashlib.sha256(bytes(4096)).hexdigest(),
            )
            self.assertEqual(len(steps), len(EXPECTED_PAIR_PCS))
            self.assertEqual([item["pc"] for item in steps], EXPECTED_PAIR_PCS)
            self.assertEqual(
                [item["step"] for item in steps],
                list(range(len(EXPECTED_PAIR_PCS))),
            )

            operations = {executed_operation(item["lower"]) for item in steps}
            self.assertNotIn("UNKNOWN", operations)
            self.assertEqual(
                operations,
                {
                    "IADDIU", "IADDI", "IADD", "ISUB", "IAND", "IOR",
                    "ISUBIU", "ILW", "ISW", "LQ", "SQ", "LQI", "SQI", "LQD", "SQD", "B", "BAL", "JR", "JALR", "IBEQ", "IBNE",
                    "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ",
                },
            )

            self.assertTrue(steps[4]["state"]["m_bit"])
            self.assertFalse(steps[5]["state"]["m_bit"])
            self.assertEqual(steps[7]["state"]["branch"], 1)
            self.assertEqual(steps[7]["state"]["branch_pc"], 80)

            # The extra pairs must execute, not just appear in the fixture.
            self.assertEqual(steps[30]["state"]["vi"][9], 0x7fff)
            self.assertEqual(steps[31]["state"]["vi"][9], 0x7fef)
            self.assertEqual(steps[33]["state"]["vi"][11], 0xc001)
            self.assertEqual(steps[38]["state"]["vi"][0], 0)

            # Both sign predicates and their delay/fallthrough paths.
            self.assertEqual(steps[39]["state"]["branch"], 1)
            self.assertEqual(steps[39]["state"]["branch_pc"], 392)
            self.assertEqual(steps[40]["state"]["vi"][8], 0x101)
            self.assertEqual(steps[41]["state"]["branch"], 0)
            self.assertEqual(steps[43]["state"]["vi"][8], 0x103)
            self.assertEqual(steps[44]["state"]["branch"], 1)
            self.assertEqual(steps[44]["state"]["branch_pc"], 440)
            self.assertEqual(steps[45]["state"]["vi"][8], 0x104)
            self.assertEqual(steps[46]["state"]["branch"], 0)
            self.assertEqual(steps[48]["state"]["vi"][8], 0x106)

            final = steps[-1]["state"]
            self.assertEqual(final["vi"][1], 5)
            self.assertEqual(final["vi"][3], 0x333)
            self.assertEqual(final["vi"][4], 0xffff)
            self.assertEqual(final["vi"][5], 1)
            self.assertEqual(final["vi"][6], 5)
            self.assertEqual(final["vi"][7], 4)
            self.assertEqual(final["vi"][8], 51)
            self.assertEqual(final["vi"][9], 0x7fef)
            self.assertEqual(final["vi"][10], 0xbfed)
            self.assertEqual(final["vi"][11], 0xc001)
            self.assertEqual(final["vi"][12], 0x4001)
            self.assertEqual(final["vi"][13], 16)
            self.assertEqual(final["vi"][14], 0x7fef)
            self.assertEqual(final["vi"][15], 18)
            self.assertEqual(final["vi"][2], 0x7fef)
            self.assertEqual(final["vi"][26], 576)
            self.assertEqual(final["vi"][29], 0)
            self.assertEqual(final["micro_pc"], 576)
            self.assertEqual(final["tpc"], 72)
            # PCSX2 does not stall an ISW on ILW's IALU destination.
            self.assertEqual(steps[50]["state"]["cycle"], 51)
            self.assertEqual(steps[51]["state"]["cycle"], 52)
            # PCSX2 flushes the four-cycle lower FMAC pipeline at E,
            # including the final LQ destination reservation.
            self.assertEqual(final["cycle"], 71)
            self.assertNotEqual(
                steps[38]["state"]["data_sha256"],
                steps[49]["state"]["data_sha256"],
            )
            self.assertEqual(
                steps[49]["state"]["data_sha256"],
                steps[50]["state"]["data_sha256"],
            )
            self.assertNotEqual(
                steps[50]["state"]["data_sha256"],
                steps[51]["state"]["data_sha256"],
            )
            self.assertEqual(
                steps[51]["state"]["data_sha256"],
                steps[52]["state"]["data_sha256"],
            )
            self.assertEqual(
                steps[54]["state"]["vf"][3],
                [0x11223344, 0x05060708, 0x99aabbcc, 0x0d0e0f10],
            )
            self.assertEqual(
                steps[56]["state"]["vf"][4],
                [0x11223344, 0x05060708, 0, 0],
            )
            self.assertEqual(final["vi"][1], 5)
            self.assertEqual(final["vf"][6][0], 0x11223344)
            self.assertEqual(final["e_bit"], 0)
            self.assertFalse(final["running"])

            comparison = subprocess.run(
                [
                    sys.executable,
                    str(comparator),
                    str(trace_path),
                    str(trace_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                comparison.returncode,
                0,
                comparison.stdout + comparison.stderr,
            )
            self.assertIn("match exactly", comparison.stdout)


if __name__ == "__main__":
    unittest.main()
