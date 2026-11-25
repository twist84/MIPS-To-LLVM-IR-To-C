#!/usr/bin/env python3
"""Build both VU0 trace producers and compare the pinned PCSX2 interpreter."""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path


PCSX2_REVISION = "82cac38b2af19d593dedfd9ddaa63448e7ec493c"
GTEST_FILTER = "VU0IssueOrder.EmitDeterministicPerPairTrace:VU0IssueOrder.DualIssueDestinationPriority"


def run(command: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def cmake_configure(
    source: Path,
    build: Path,
    build_type: str,
    generator: str | None,
    extra_args: list[str],
) -> None:
    command = [
        "cmake",
        "-S", str(source),
        "-B", str(build),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        *extra_args,
    ]
    if generator:
        command.extend(["-G", generator])
    run(command)


def find_executable(build_dir: Path, names: tuple[str, ...]) -> Path:
    for name in names:
        candidate = build_dir / name
        if candidate.is_file():
            return candidate

    wanted = {Path(name).name.lower() for name in names}
    matches = sorted(
        path for path in build_dir.rglob("*")
        if path.is_file() and path.name.lower() in wanted
    )
    if len(matches) == 1:
        return matches[0]
    if matches:
        raise RuntimeError(
            "multiple matching executables found:\n" +
            "\n".join(str(path) for path in matches)
        )
    raise RuntimeError(
        f"could not find any of {', '.join(names)} under {build_dir}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pcsx2-root",
        type=Path,
        required=True,
        help="PCSX2 Git checkout at the pinned interpreter revision",
    )
    parser.add_argument(
        "--pcsx2-build-dir",
        type=Path,
        help="PCSX2 build directory (default: <pcsx2-root>/build-vu0-diff)",
    )
    parser.add_argument(
        "--project-build-dir",
        type=Path,
        help="This project's build directory (default: <repo>/build-vu0-diff)",
    )
    parser.add_argument(
        "--pcsx2-prefix-path",
        type=Path,
        help="Optional CMAKE_PREFIX_PATH for PCSX2 third-party dependencies",
    )
    parser.add_argument(
        "--generator",
        help="Optional CMake generator, for example Ninja or Visual Studio 18 2026",
    )
    parser.add_argument("--build-type", default="Release")
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, min(os.cpu_count() or 2, 4)),
        help="parallel build jobs (default: min(cpu count, 4))",
    )
    parser.add_argument(
        "--keep-traces",
        action="store_true",
        help="retain the generated JSONL traces (they are retained on failure)",
    )
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parents[2]
    pcsx2_root = args.pcsx2_root.resolve()
    if not (pcsx2_root / ".git").exists():
        print("error: --pcsx2-root is not a Git checkout", file=sys.stderr)
        return 2

    pcsx2_build = (
        args.pcsx2_build_dir.resolve()
        if args.pcsx2_build_dir
        else pcsx2_root / "build-vu0-diff"
    )
    project_build = (
        args.project_build_dir.resolve()
        if args.project_build_dir
        else project_root / "build-vu0-diff"
    )
    project_build.mkdir(parents=True, exist_ok=True)

    try:
        revision = subprocess.run(
            ["git", "-C", str(pcsx2_root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if revision != PCSX2_REVISION:
            raise RuntimeError(
                f"PCSX2 revision mismatch: expected {PCSX2_REVISION}, got {revision}"
            )

        installer = project_root / "tools" / "vu0_diff" / "integrate_pcsx2_test.py"
        run([
            sys.executable, str(installer),
            "--pcsx2-root", str(pcsx2_root),
        ])

        cmake_configure(
            project_root,
            project_build,
            args.build_type,
            args.generator,
            [],
        )
        run([
            "cmake", "--build", str(project_build),
            "--target", "ee_vu0_trace_candidate",
            "--parallel", str(args.jobs),
        ])

        pcsx2_cmake_args = [
            "-DENABLE_TESTS=ON",
            "-DENABLE_QT_UI=OFF",
            "-DDISABLE_ADVANCE_SIMD=ON",
            "-DENABLE_SETCAP=OFF",
            "-DUSE_LINKED_FFMPEG=ON",
            "-DUSE_OPENGL=OFF",
            "-DUSE_VULKAN=OFF",
            "-DWAYLAND_API=OFF",
            "-DUSE_BACKTRACE=OFF",
        ]
        if platform.system() == "Linux":
            # PCSX2 common/CMakeLists.txt always links X11 imported targets
            # on Linux, so keep FindX11 enabled even with graphics backends off.
            pcsx2_cmake_args.append("-DX11_API=ON")

        pcsx2_prefix = (
            args.pcsx2_prefix_path.resolve()
            if args.pcsx2_prefix_path
            else (pcsx2_root / "deps")
        )
        if pcsx2_prefix.is_dir():
            pcsx2_cmake_args.append(f"-DCMAKE_PREFIX_PATH={pcsx2_prefix}")

        cmake_configure(
            pcsx2_root,
            pcsx2_build,
            args.build_type,
            args.generator,
            pcsx2_cmake_args,
        )
        run([
            "cmake", "--build", str(pcsx2_build),
            "--target", "core_test",
            "--parallel", str(args.jobs),
        ])

        candidate = find_executable(
            project_build,
            ("ee_vu0_trace_candidate", "ee_vu0_trace_candidate.exe"),
        )
        core_test = find_executable(
            pcsx2_build,
            ("core_test", "core_test.exe"),
        )

        candidate_trace = project_build / "candidate-vu0.jsonl"
        pcsx2_trace = project_build / "pcsx2-vu0.jsonl"

        run([str(candidate), str(candidate_trace)])

        environment = os.environ.copy()
        environment["VU0_TRACE_OUTPUT"] = str(pcsx2_trace)
        run([str(core_test), f"--gtest_filter={GTEST_FILTER}"], env=environment)

        comparator = project_root / "tools" / "compare_vu0_traces.py"
        run([
            sys.executable,
            str(comparator),
            str(pcsx2_trace),
            str(candidate_trace),
        ])

        print("Live PCSX2 VU0 differential trace matched exactly.", flush=True)
        if not args.keep_traces:
            candidate_trace.unlink(missing_ok=True)
            pcsx2_trace.unlink(missing_ok=True)
            print("Trace files removed; use --keep-traces to retain them.")
        return 0

    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        print(
            f"Trace files, if generated, are retained under {project_build}",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
