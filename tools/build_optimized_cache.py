#!/usr/bin/env python3
"""Emit an independent optimized EE library from verified existing IR."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys

import run_game


def function_map(metadata):
    return [{key: value for key, value in fn.items() if key != "ir_path"}
            for fn in metadata["functions"]]


def verify_modules(work, metadata, hashes):
    ir_dir = (work / "ir").resolve()
    expected = {}
    for function in metadata["functions"]:
        module = Path(function["ir_path"]).resolve(strict=True)
        if module.parent != ir_dir or module.suffix != ".ll":
            raise ValueError("function references an unexpected IR path")
        if module.name in expected:
            raise ValueError("duplicate IR module")
        expected[module.name] = run_game.sha256_path(module)
    if not expected or expected != hashes:
        raise ValueError("verified IR hashes do not match the function map")
    if set(expected) != {path.name for path in ir_dir.glob("*.ll")}:
        raise ValueError("unowned IR modules in cache directory")


def prepare(elf, build, config, source_work, work, opt, jobs, source_opt=0):
    elf = elf.resolve(strict=True)
    source_work = source_work.resolve(strict=True)
    work = work.resolve()
    if (work == source_work or source_work in work.parents or
            work in source_work.parents):
        raise ValueError("optimized cache must be separate from the source")
    root = Path(__file__).resolve().parents[1]
    recomp = run_game.native_executable(build.resolve(), "ee_recomp", config)
    inputs = run_game.translation_inputs(elf, recomp, root)
    ir_sources = run_game.ir_generation_inputs(elf, root)
    library_name = ("game-translated.dll" if os.name == "nt" else
                    "libgame-translated.dylib" if sys.platform == "darwin" else
                    "libgame-translated.so")
    source_metadata_path = source_work / "ir" / "ee_functions.json"
    source_cache = source_work / "translation-cache.json"
    run_game.validate_translation_cache(
        source_cache, inputs, source_opt, source_metadata_path,
        source_work / library_name)
    source_snapshot = json.loads(source_cache.read_text(encoding="utf-8"))
    if source_snapshot.get("ir_sources") != ir_sources:
        raise ValueError("source lowering fingerprint is not current")
    hashes = source_snapshot.get("ir_hashes")
    metadata = json.loads(source_metadata_path.read_text(encoding="utf-8"))
    verify_modules(source_work, metadata, hashes)

    cache = work / "translation-cache.json"
    metadata_path = work / "ir" / "ee_functions.json"
    library = work / library_name
    if work.exists():
        # Retain functions discovered by the independent optimized replay.
        snapshot = json.loads(cache.read_text(encoding="utf-8"))
        if (snapshot.get("ir_sources") != ir_sources or snapshot.get("emit_opt") != opt or
                snapshot.get("ir_pipeline") != run_game.ir_pipeline_id(opt)):
            raise ValueError("optimized cache lowering or optimization changed")
        variant_metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        variant_hashes = snapshot.get("ir_hashes")
        if (not isinstance(variant_hashes, dict) or
                any(variant_hashes.get(name) != digest for name, digest in hashes.items()) or
                function_map(variant_metadata)[:len(metadata["functions"])] != function_map(metadata)):
            raise ValueError("optimized cache does not contain the verified source map")
        verify_modules(work, variant_metadata, variant_hashes)
        try:
            run_game.validate_translation_cache(
                cache, inputs, opt, metadata_path, library)
        except ValueError:
            result = run_game.main([
                str(elf), "--build-dir", str(build), "--config", config,
                "--work-dir", str(work), f"--emit-opt={opt}",
                f"--emit-jobs={jobs}", "--relink-existing", "--link-only"])
            if result != 0:
                raise ValueError("optimized cache relink failed")
            run_game.validate_translation_cache(
                cache, inputs, opt, metadata_path, library)
        print(f"Reusing verified O{opt} EE cache: {work}", flush=True)
        return

    # Only remove the newly created variant on failure. The source IR,
    # library, objects and active-cache pointer are not modified.
    work.mkdir(parents=True, exist_ok=False)
    try:
        ir_dir = work / "ir"
        ir_dir.mkdir()
        for function in metadata["functions"]:
            module = Path(function["ir_path"])
            destination = ir_dir / module.name
            shutil.copyfile(module, destination)
            function["ir_path"] = str(destination)
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n",
                                 encoding="utf-8")
        verify_modules(work, metadata, hashes)
        run_game.build_linked_library(
            root, work, metadata_path, work / "objects", library,
            opt, emit_jobs=jobs)
        run_game.write_translation_cache(
            cache, inputs, opt, metadata_path, library, ir_sources)
        run_game.validate_translation_cache(
            cache, inputs, opt, metadata_path, library)
        if run_game.translated_ir_hashes(metadata_path) != hashes:
            raise ValueError("optimized build changed the original IR")
    except BaseException:
        shutil.rmtree(work)
        raise
    print(f"Built O{opt} EE variant from {len(hashes)} unchanged IR modules: {work}",
          flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--source-work-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--emit-opt", type=int, choices=(1, 2, 3, 4), required=True)
    parser.add_argument("--source-opt", type=int, choices=(0, 2), default=0,
                        help="verified source cache profile; use 2 to seed profile 4 from O2")
    parser.add_argument("--emit-jobs", type=int, default=2)
    args = parser.parse_args(argv)
    if args.source_opt == 2 and args.emit_opt != 4:
        parser.error("O2 source is only supported for the independent IR pipeline profile")
    if not 1 <= args.emit_jobs <= 32:
        parser.error("--emit-jobs must be between 1 and 32")
    try:
        prepare(args.elf, args.build_dir, args.config, args.source_work_dir,
                args.work_dir, args.emit_opt, args.emit_jobs, args.source_opt)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(f"Optimized cache refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
