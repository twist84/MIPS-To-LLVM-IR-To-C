#!/usr/bin/env python3
"""Exercise complete candidate-event export and strict missing-time fail-closed."""

import csv
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_ee_issue_trace_export.py binary fixture.csv")
    binary = Path(sys.argv[1])
    fixture = Path(sys.argv[2])

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "candidate.csv"
        result = subprocess.run(
            [str(binary), str(fixture), "1", "3",
             f"--candidate-events={output}"],
            text=True, capture_output=True, check=False
        )
        assert result.returncode == 0, result.stdout + result.stderr
        with output.open("r", encoding="utf-8", newline="") as stream:
            events = list(csv.DictReader(stream))
        assert len(events) == 7, events
        assert [int(row["candidate_issue_cycle"]) for row in events] == [
            0, 0, 1, 4, 5, 6, 7
        ], events
        assert [int(row["reference_slot"]) for row in events] == list(
            range(1, 8)
        ), events
        assert events[5]["kind"] == "1", events[5]

        manual_output = root / "manual.csv"
        manual = subprocess.run(
            [str(binary), str(fixture), "--manual-cache-hit",
             f"--candidate-events={manual_output}"],
            text=True, capture_output=True, check=False
        )
        assert manual.returncode == 0, manual.stdout + manual.stderr
        assert "profile_source=ee_manual_v6_cache_hit" in manual.stdout
        assert "NOT validated R5900 cycles" in manual.stdout
        with manual_output.open("r", encoding="utf-8", newline="") as stream:
            manual_events = list(csv.DictReader(stream))
        assert [int(row["candidate_issue_cycle"]) for row in manual_events] == [
            0, 0, 1, 2, 3, 4, 5
        ], manual_events
        conflict = subprocess.run(
            [str(binary), str(fixture), "--manual-cache-hit",
             "--throughput=2,37,2,1,7,7,13"],
            text=True, capture_output=True, check=False
        )
        assert conflict.returncode == 2, conflict.stdout + conflict.stderr

        # Completion is a *separate* candidate result deadline. Annulled
        # likely slots do not have an instruction result to timestamp.
        completion_output = root / "completions.csv"
        completion_run = subprocess.run(
            [str(binary), str(fixture), "1", "3",
             f"--candidate-completions={completion_output}"],
            text=True, capture_output=True, check=False
        )
        assert completion_run.returncode == 0, (
            completion_run.stdout, completion_run.stderr
        )
        with completion_output.open("r", encoding="utf-8",
                                    newline="") as stream:
            completion_rows = list(csv.DictReader(stream))
        assert len(completion_rows) == len(events)
        assert completion_rows[5]["candidate_completion_cycle"] == ""
        for row in completion_rows:
            if row["kind"] != "1":
                assert int(row["candidate_completion_cycle"]) >= (
                    int(row["candidate_issue_cycle"])
                ), row

        # Optional COP1 control/compare and separate SQRT/RSQRT groups
        # must be accepted without changing the old 4-argument CLI.
        numeric = ["1", "3", "4", "37", "1", "4",
                   "2", "4", "8", "4", "2", "4"]
        for extension in ([], ["8", "14"]):
            extended_path = root / f"extended-{len(extension)}.csv"
            extended = subprocess.run(
                [str(binary), str(fixture), *numeric, *extension,
                 f"--candidate-events={extended_path}"],
                text=True, capture_output=True, check=False
            )
            assert extended.returncode == 0, (
                extended.stdout, extended.stderr
            )
            assert extended_path.exists(), extended_path

        for values in (
            numeric[:-1],  # incomplete FCR31 group
            numeric[:-2] + ["0", "4"],  # unconfigured FCR31 control
            numeric + ["8", "0"],  # unconfigured RSQRT.S
        ):
            bad = subprocess.run(
                [str(binary), str(fixture), *values],
                text=True, capture_output=True, check=False
            )
            assert bad.returncode == 2, (
                values, bad.stdout, bad.stderr
            )

        # Strict throughput is a separate, explicit offline candidate mode.
        # It must preserve the old no-unit trace without silently
        # installing hardware cycle calibration.
        strict_option = "--throughput=2,37,2,1,7,7,13"
        strict_output = root / "throughput.csv"
        strict = subprocess.run(
            [str(binary), str(fixture), "1", "3", strict_option,
             f"--candidate-events={strict_output}"],
            text=True, capture_output=True, check=False
        )
        assert strict.returncode == 0, strict.stdout + strict.stderr
        assert "throughput_mode=explicit" in strict.stdout, strict.stdout
        assert strict_output.read_text(encoding="utf-8") == (
            output.read_text(encoding="utf-8")
        )

        for invalid in (
            "--throughput=2,37,2,1,7,7",
            "--throughput=2,37,2,1,7,7,0",
            "--throughput=2,37,2,1,7,7,-1",
            "--throughput=2,37,2,1,7,7,13,9",
        ):
            rejected = subprocess.run(
                [str(binary), str(fixture), "1", "3", invalid],
                text=True, capture_output=True, check=False
            )
            assert rejected.returncode == 2, (
                invalid, rejected.stdout, rejected.stderr
            )

        mac_fixture = root / "mac.csv"
        mac_fixture.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            "1,0x00001000,0x00853018,0,1,0\n"
            "# truncated=false range=0x00001000:0x00001000\n",
            encoding="utf-8"
        )
        for argv_values in (
            ["1", "3", "7", "11", "4", "8"],
            ["1", "3", "7", "11", "4", "8", strict_option],
        ):
            result = subprocess.run(
                [str(binary), str(mac_fixture), *argv_values],
                text=True, capture_output=True, check=False
            )
            assert result.returncode == 0, (
                argv_values, result.stdout, result.stderr
            )

        # Repeated writes are the distinguishing fixture: conservative WAW
        # waiting sees 0,7, while explicit ordered overlap sees 0,2.
        two_mac = root / "two_mac.csv"
        two_mac.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            "1,0x00001000,0x00853018,0,1,0\n"
            "2,0x00001004,0x00853018,0,2,0\n"
            "# truncated=false range=0x00001000:0x00001004\n",
            encoding="utf-8"
        )
        for overlap, expected in (
            ([], [0, 7]),
            (["--ordered-waw-overlap"], [0, 2]),
        ):
            path = root / f"mac-overlap-{len(overlap)}.csv"
            candidate = subprocess.run(
                [str(binary), str(two_mac), "1", "3", "7", "11", "4",
                 "8", strict_option, *overlap, f"--candidate-events={path}"],
                text=True, capture_output=True, check=False
            )
            assert candidate.returncode == 0, (
                candidate.stdout, candidate.stderr
            )
            with path.open("r", encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            assert [int(x["candidate_issue_cycle"]) for x in rows] == (
                expected
            ), rows
            assert "last_candidate_completion=" in candidate.stdout
            assert "max_candidate_completion=" in candidate.stdout
            assert (
                "ordered_waw_overlap=enabled" if overlap
                else "ordered_waw_overlap=disabled"
            ) in candidate.stdout, candidate.stdout

        completion_mac = root / "overlap-completions.csv"
        completed = subprocess.run(
            [str(binary), str(two_mac), "1", "3", "7", "11", "4",
             "8", strict_option, "--ordered-waw-overlap",
             f"--candidate-completions={completion_mac}"],
            text=True, capture_output=True, check=False
        )
        assert completed.returncode == 0, completed.stderr
        with completion_mac.open("r", encoding="utf-8",
                                 newline="") as stream:
            rows = list(csv.DictReader(stream))
        assert [int(row["candidate_issue_cycle"]) for row in rows] == (
            [0, 2]
        ), rows
        assert [int(row["candidate_completion_cycle"]) for row in rows] == (
            [7, 9]
        ), rows
        assert "max_candidate_completion=9" in completed.stdout

        invalid_overlap = subprocess.run(
            [str(binary), str(two_mac), "1", "3", "7", "11",
             "4", "8", "--ordered-waw-overlap"],
            text=True, capture_output=True, check=False
        )
        assert invalid_overlap.returncode == 2, (
            invalid_overlap.stdout, invalid_overlap.stderr
        )

        # Decoder-aligned COP0 and VU0 architectural transfers must be
        # explicitly opted in; the guest's actual system transitions,
        # VU0 macros and memory effects remain unmodeled.
        extended = numeric + ["8", "14", "6", "5", "7"]
        transfer_cases = (
            ("cop0", "0x40886000", "0x40096000", [0, 6], [6, 12]),
            ("vu0-vf", "0x48a82000", "0x48292000", [0, 5], [5, 10]),
            ("vu0-vi", "0x48c82000", "0x48492000", [0, 7], [7, 14]),
        )
        for label, producer, consumer, issued, completed in transfer_cases:
            transfer_trace = root / f"{label}.csv"
            transfer_trace.write_text(
                "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
                f"1,0x00001000,{producer},0,1,0\n"
                f"2,0x00001004,{consumer},0,2,0\n"
                "# truncated=false range=0x00001000:0x00001004\n",
                encoding="utf-8"
            )
            omitted = subprocess.run(
                [str(binary), str(transfer_trace), "1", "3"],
                text=True, capture_output=True, check=False
            )
            assert omitted.returncode == 3, (
                label, omitted.stdout, omitted.stderr
            )
            completions = root / f"{label}-completions.csv"
            result = subprocess.run(
                [str(binary), str(transfer_trace), *extended,
                 f"--candidate-completions={completions}"],
                text=True, capture_output=True, check=False
            )
            assert result.returncode == 0, (
                label, result.stdout, result.stderr
            )
            with completions.open("r", encoding="utf-8",
                                  newline="") as stream:
                rows = list(csv.DictReader(stream))
            assert [int(row["candidate_issue_cycle"]) for row in rows] == (
                issued
            ), (label, rows)
            assert [int(row["candidate_completion_cycle"])
                    for row in rows] == completed, (label, rows)

        for invalid in (
            extended[:-1],
            extended[:-3] + ["6", "0", "7"],
            extended[:-3] + ["6", "5", "-1"],
        ):
            rejected = subprocess.run(
                [str(binary), str(fixture), *invalid],
                text=True, capture_output=True, check=False
            )
            assert rejected.returncode == 2, (
                invalid, rejected.stdout, rejected.stderr
            )

        # Packed MMI uses its own explicit result-latency option. The
        # synthetic profile tests a dependency, not physical issue cycles.
        def make_mmi(rs, rt, rd, sa, funct):
            return ((0x1C << 26) | (rs << 21) | (rt << 16) |
                    (rd << 11) | (sa << 6) | funct)

        packed_trace = root / "mmi-full-128.csv"
        packed_trace.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            f"1,0x00001000,0x{make_mmi(8, 9, 10, 0, 8):08x},0,1,0\n"
            f"2,0x00001004,0x{make_mmi(10, 11, 12, 5, 8):08x},0,2,0\n"
            "# truncated=false range=0x00001000:0x00001004\n",
            encoding="utf-8",
        )
        unconfigured = subprocess.run(
            [str(binary), str(packed_trace), "1", "3"],
            text=True, capture_output=True, check=False,
        )
        assert unconfigured.returncode == 3, (
            unconfigured.stdout, unconfigured.stderr
        )
        mmi_result = root / "mmi-full-128-results.csv"
        candidate = subprocess.run(
            [str(binary), str(packed_trace), "1", "3",
             "--packed-mmi-latency=5",
             f"--candidate-completions={mmi_result}"],
            text=True, capture_output=True, check=False,
        )
        assert candidate.returncode == 0, (
            candidate.stdout, candidate.stderr
        )
        with mmi_result.open("r", encoding="utf-8",
                             newline="") as stream:
            mmi_rows = list(csv.DictReader(stream))
        assert [int(row["candidate_issue_cycle"]) for row in mmi_rows] == [
            0, 5
        ], mmi_rows
        assert [int(row["candidate_completion_cycle"])
                for row in mmi_rows] == [5, 10], mmi_rows

        for invalid in ("--packed-mmi-latency=0",
                        "--packed-mmi-latency=-1",
                        "--packed-mmi-latency=abc"):
            rejected = subprocess.run(
                [str(binary), str(packed_trace), "1", "3", invalid],
                text=True, capture_output=True, check=False,
            )
            assert rejected.returncode == 2, (
                invalid, rejected.stdout, rejected.stderr
            )

        qfsrv_trace = root / "mmi-qfsrv.csv"
        qfsrv_trace.write_text(
            "attempt,pc,opcode_word,kind,reference_slot,hle_service\n"
            f"1,0x00001000,0x{make_mmi(8, 9, 10, 27, 0x28):08x},0,1,0\n"
            "# truncated=false range=0x00001000:0x00001000\n",
            encoding="utf-8",
        )
        qfsrv = subprocess.run(
            [str(binary), str(qfsrv_trace), "1", "3",
             "--packed-mmi-latency=5"],
            text=True, capture_output=True, check=False,
        )
        assert qfsrv.returncode == 3, (
            qfsrv.stdout, qfsrv.stderr
        )

        # The replay must refuse HLE transitions and a partial trace,
        # without producing a deceptively complete candidate CSV.
        original = fixture.read_text(encoding="utf-8")
        for suffix, damaged in (
            ("hle", original.replace(
                "# truncated=false",
                "7,0x00001018,0x0000000c,3,7,118\n# truncated=false",
            )),
            ("truncated", original.replace(
                "# truncated=false", "# truncated=true"
            )),
        ):
            input_path = root / f"{suffix}.csv"
            rejected = root / f"{suffix}-output.csv"
            input_path.write_text(damaged, encoding="utf-8")
            failure = subprocess.run(
                [str(binary), str(input_path), "1", "3",
                 f"--candidate-events={rejected}"],
                text=True, capture_output=True, check=False
            )
            assert failure.returncode == 3, (
                suffix, failure.stdout, failure.stderr
            )
            assert not rejected.exists(), suffix

    print("test_ee_issue_trace_export: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
