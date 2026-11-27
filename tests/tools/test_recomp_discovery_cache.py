#!/usr/bin/env python3
"""Synthetic-ELF regression for fast single-function LLVM IR generation.

The game ELF is not used. Cached discovery must emit exactly the same IR
and block-entry metadata as independent full ELF discovery.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def make_elf(path, change=False):
    image = bytearray(0x200)
    image[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into(
        "<HHIIIIIHHHHHH", image, 16,
        2, 8, 1, 0x00100008, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<IIIIIIII", image, 52,
        1, 0x100, 0x00100000, 0x00100000, 0x100, 0x100, 5, 16,
    )
    instructions = (
        (0x108, 0x0c040010),  # JAL 0x00100040
        (0x10c, 0x00000000),
        (0x110, 0x03e00008),  # JR RA
        (0x114, 0x00000000),
        (0x140, 0x24020007 if not change else 0x24020008),
        (0x144, 0x03e00008),
        (0x148, 0x00000000),
    )
    for offset, word in instructions:
        struct.pack_into("<I", image, offset, word)
    path.write_bytes(image)


def compile_ir(exe, elf, output, function, cache=None):
    command = [exe, str(elf), str(output), f"--function=0x{function:08x}"]
    if cache is not None:
        command.append(f"--discovery-cache={cache}")
    return subprocess.run(command, capture_output=True, text=True)


def test_discovery_cache(ee_recomp):
    with tempfile.TemporaryDirectory(prefix="ee-discovery-cache-") as tmp:
        root = Path(tmp)
        elf = root / "synthetic.elf"
        make_elf(elf)
        full = root / "full"
        cache = full / "ee_discovery_cache.bin"
        baseline = compile_ir(ee_recomp, elf, full, 0x00100040)
        assert baseline.returncode == 0, baseline.stdout + baseline.stderr
        assert cache.is_file() and cache.stat().st_size > 32, cache

        reused = root / "reused"
        fast = compile_ir(ee_recomp, elf, reused, 0x00100040, cache)
        assert fast.returncode == 0, fast.stdout + fast.stderr
        assert "discovery cache:" in fast.stdout, fast.stdout

        first_meta = json.loads((full / "ee_functions.json").read_text())
        second_meta = json.loads((reused / "ee_functions.json").read_text())
        assert len(first_meta["functions"]) == 1
        assert len(second_meta["functions"]) == 1
        # Discovery's denominator must be available before IR emission,
        # and must agree with the fully generated function metadata.
        for output_dir, metadata in ((full, first_meta), (reused, second_meta)):
            marker = output_dir / "ee_expected_ll_count.txt"
            assert marker.is_file(), marker
            assert int(marker.read_text(encoding="ascii").strip()) == (
                metadata["functions_discovered"]
            )

        first_fn = first_meta["functions"][0]
        second_fn = second_meta["functions"][0]
        assert first_fn["block_pcs"] == second_fn["block_pcs"]
        assert first_fn["insn_count"] == second_fn["insn_count"]
        assert first_fn["supported_count"] == second_fn["supported_count"]
        assert first_fn["host_symbol"] == second_fn["host_symbol"]
        assert Path(first_fn["ir_path"]).read_bytes() == Path(second_fn["ir_path"]).read_bytes()

        # Compare two independent target entries, including one that was
        # not in the static function entry set, against the cached result.
        dynamic_full = root / "dynamic-full"
        dynamic_fast = root / "dynamic-fast"
        result = compile_ir(ee_recomp, elf, dynamic_full, 0x00100044)
        assert result.returncode == 0, result.stdout + result.stderr
        result = compile_ir(ee_recomp, elf, dynamic_fast, 0x00100044, cache)
        assert result.returncode == 0, result.stdout + result.stderr
        a = json.loads((dynamic_full / "ee_functions.json").read_text())
        b = json.loads((dynamic_fast / "ee_functions.json").read_text())
        assert a["functions"][0]["block_pcs"] == b["functions"][0]["block_pcs"]
        assert (Path(a["functions"][0]["ir_path"]).read_bytes()
                == Path(b["functions"][0]["ir_path"]).read_bytes())

        # Never silently reuse discovery from a different executable.
        changed = root / "changed.elf"
        make_elf(changed, change=True)
        denied = compile_ir(ee_recomp, changed, root / "changed", 0x00100040, cache)
        assert denied.returncode != 0, denied.stdout
        assert "stale or invalid" in denied.stderr, denied.stderr

        # Truncation cannot produce partial functions or a false cache hit.
        cache.write_bytes(cache.read_bytes()[:32])
        denied = compile_ir(ee_recomp, elf, root / "truncated", 0x00100040, cache)
        assert denied.returncode != 0, denied.stdout
        assert "stale or invalid" in denied.stderr, denied.stderr
    print("PASS: fast EE discovery produces identical IR and rejects invalid caches")


def make_25_function_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x600 - len(image)))
    struct.pack_into("<I", image, 52 + 16, 0x500)  # PT_LOAD filesz
    struct.pack_into("<I", image, 52 + 20, 0x500)  # PT_LOAD memsz
    for i in range(25):
        offset = 0x180 + i * 0x10
        struct.pack_into("<IIII", image, offset,
                         0x24020000 + i,  # addiu v0,zero,i
                         0x03e00008,      # jr ra
                         0x00000000,      # delay slot
                         0x00000000)
    path.write_bytes(image)


def test_batch_discovery(ee_recomp):
    with tempfile.TemporaryDirectory(prefix="ee-recomp-batch-") as tmp:
        root = Path(tmp)
        elf = root / "functions25.elf"
        make_25_function_elf(elf)
        # Generate the immutable full-ELF discovery snapshot as an initial
        # single-function translation, just like run_game.py.
        first = compile_ir(ee_recomp, elf, root / "initial", 0x00100008)
        assert first.returncode == 0, first.stdout + first.stderr
        cache = root / "initial" / "ee_discovery_cache.bin"
        assert cache.is_file()

        addresses = [0x00100080 + i * 0x10 for i in range(25)]
        selection = "--functions=" + ",".join(
            f"0x{pc:08x}" for pc in addresses
        )
        cmd = [ee_recomp, str(elf), str(root / "batch"), selection,
               f"--discovery-cache={cache}"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.count("discovery cache:") == 1, result.stdout
        assert "(functions=25)" in result.stdout, result.stdout

        metadata = json.loads((root / "batch" / "ee_functions.json").read_text())
        assert metadata["function_count"] == 25, metadata
        assert [int(f["vaddr"], 0) for f in metadata["functions"]] == addresses
        symbols = [f["host_symbol"] for f in metadata["functions"]]
        assert len(set(symbols)) == 25, symbols
        assert all(int(f["vaddr"], 0) in
                   {int(pc, 0) for pc in f["block_pcs"]}
                   for f in metadata["functions"]), metadata
        for f in metadata["functions"]:
            assert Path(f["ir_path"]).is_file(), f

        # The batched cache path must match the same batch when full
        # discovery is performed from the original ELF.
        full = subprocess.run(
            [ee_recomp, str(elf), str(root / "full-batch"), selection],
            capture_output=True, text=True
        )
        assert full.returncode == 0, full.stdout + full.stderr
        other = json.loads(
            (root / "full-batch" / "ee_functions.json").read_text()
        )
        assert other["function_count"] == 25
        for a, b in zip(metadata["functions"], other["functions"]):
            assert a["vaddr"] == b["vaddr"]
            assert a["block_pcs"] == b["block_pcs"]
            assert Path(a["ir_path"]).read_bytes() == Path(b["ir_path"]).read_bytes()

        for invalid in (
            "--functions=0x00100080,0x00100080",  # duplicate
            "--functions=0x00100080,0x00100081",  # not aligned
            "--functions=0x00100080,",           # dangling comma
            "--functions=oops",                  # malformed
        ):
            denied = subprocess.run(
                [ee_recomp, str(elf), str(root / "denied"), invalid],
                capture_output=True, text=True
            )
            assert denied.returncode != 0, (invalid, denied.stdout, denied.stderr)

        denied = subprocess.run([
            ee_recomp, str(elf), str(root / "conflict"), selection,
            "--function=0x00100008"
        ], capture_output=True, text=True)
        assert denied.returncode != 0, denied.stdout + denied.stderr
    print("PASS: 25 functions emitted with one discovery; cache/full parity")


def test_persistent_session(ee_recomp):
    with tempfile.TemporaryDirectory(prefix="ee-recomp-session-") as tmp:
        root = Path(tmp)
        elf = root / "fixture.elf"
        make_25_function_elf(elf)
        seed = root / "seed"
        result = compile_ir(ee_recomp, elf, seed, 0x00100008)
        assert result.returncode == 0, result.stderr
        cache = seed / "ee_discovery_cache.bin"
        selections = [[0x00100080, 0x00100090], [0x00100084], [0x00100080, 0x00100090]]
        lines = [f"seed\t{root / 'session-seed'}\t0x00100008"]
        for i, pcs in enumerate(selections):
            values = ",".join(hex(pc) for pc in pcs)
            output = root / f"serial-{i}"
            result = subprocess.run([ee_recomp, str(elf), str(output),
                                     "--functions=" + values, f"--discovery-cache={cache}"],
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            lines.append(f"batch\t{root / f'session-{i}'}\t{values}")
        result = subprocess.run([ee_recomp, str(elf), "--serve-reconstruction"],
                                input="\n".join(lines + ["quit", ""]),
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.count("ee_recomp session result: ok") == 4
        for serial, session in [(seed, root / "session-seed")] + [
                (root / f"serial-{i}", root / f"session-{i}") for i in range(3)]:
            a = json.loads((serial / "ee_functions.json").read_text())
            b = json.loads((session / "ee_functions.json").read_text())
            for x, y in zip(a["functions"], b["functions"], strict=True):
                assert Path(x["ir_path"]).read_bytes() == Path(y["ir_path"]).read_bytes()
                x["ir_path"] = Path(x["ir_path"]).name
                y["ir_path"] = Path(y["ir_path"]).name
            assert a == b
        # Exercise the actual Python driver as well as the raw line protocol.
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import run_game
        with run_game.c_recomp_session(ee_recomp, elf, ir_jobs=4) as session:
            driver_seed = root / "driver-seed"
            session([ee_recomp, elf, driver_seed, "--function=0x00100008"])
            driver_batch = root / "driver-batch"
            session([ee_recomp, elf, driver_batch, "--functions=0x00100080,0x00100090",
                     f"--discovery-cache={driver_seed / 'ee_discovery_cache.bin'}"])
        a = json.loads((driver_batch / "ee_functions.json").read_text())
        b = json.loads((root / "serial-0" / "ee_functions.json").read_text())
        for x, y in zip(a["functions"], b["functions"], strict=True):
            assert Path(x["ir_path"]).read_bytes() == Path(y["ir_path"]).read_bytes()
            x["ir_path"] = Path(x["ir_path"]).name
            y["ir_path"] = Path(y["ir_path"]).name
        assert a == b

        # Repeated parallel batches must match serial IR and ordered metadata.
        parallel_lines = [f"seed\t{root / 'parallel-seed'}\t0x00100008"]
        for i, pcs in enumerate(selections):
            parallel_lines.append(f"batch\t{root / f'parallel-{i}'}\t" +
                                  ",".join(hex(pc) for pc in pcs))
        parallel = subprocess.run([ee_recomp, str(elf), "--serve-reconstruction", "--ir-jobs=4"],
                                  input="\n".join(parallel_lines + ["quit", ""]),
                                  capture_output=True, text=True)
        assert parallel.returncode == 0, parallel.stdout + parallel.stderr
        assert "ee_recomp parallel IR: workers=2 modules=2" in parallel.stdout
        for i in range(3):
            a = json.loads((root / f"serial-{i}" / "ee_functions.json").read_text())
            b = json.loads((root / f"parallel-{i}" / "ee_functions.json").read_text())
            for x, y in zip(a["functions"], b["functions"], strict=True):
                assert Path(x["ir_path"]).read_bytes() == Path(y["ir_path"]).read_bytes()
                x["ir_path"] = Path(x["ir_path"]).name
                y["ir_path"] = Path(y["ir_path"]).name
            assert a == b

        denied = subprocess.run([ee_recomp, str(elf), "--serve-reconstruction"],
                                input="batch\tbad\t0x00100080\n",
                                capture_output=True, text=True)
        assert denied.returncode != 0


if __name__ == "__main__":
    test_persistent_session(sys.argv[1])
    test_discovery_cache(sys.argv[1])
    test_batch_discovery(sys.argv[1])
