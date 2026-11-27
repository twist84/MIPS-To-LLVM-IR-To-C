#!/usr/bin/env python3
"""Check identity of generated instruction bodies and candidate inputs."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_ee_issue_bench import CASES, REPEATS, generate


def main():
    binary = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory)
        rows = generate(out)
        assert len(rows) == len(CASES) * len(REPEATS)
        assert rows == json.loads((out / "cases.json").read_text())
        assembly = (out / "bench.S").read_text()
        for row in rows:
            body = assembly.split(row["symbol"] + "_body:\n", 1)[1].split(
                ".globl " + row["symbol"] + "_body_end", 1)[0]
            words = [int(line.split()[1], 16) for line in body.splitlines()]
            assert words == list(CASES[row["case"]]) * row["repeat"]
            with (out / row["trace"]).open(newline="") as stream:
                events = list(csv.DictReader(line for line in stream
                                             if not line.startswith("#")))
            assert words == [int(event["opcode_word"], 16) for event in events]
            assert len(words) == row["instructions"]
            result = subprocess.run([str(binary), str(out / row["trace"]),
                                     "--manual-cache-hit"],
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert f"reached={len(words)} " in result.stdout
    print("PASS: exact generated benchmark body/trace identity")


if __name__ == "__main__":
    main()
