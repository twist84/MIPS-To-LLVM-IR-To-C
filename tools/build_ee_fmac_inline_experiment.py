#!/usr/bin/env python3
"""Bounded profile-guided EE optimization pilots over an authenticated O5 cache."""

import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

from ee_vector_discard import vector_discard

PILOT = (0x1068e0, 0x106a20, 0x106950, 0x106aa0)
DISCARD_PILOT = (0x108220, *PILOT)
HEADER = re.compile(rb"^define internal i32 @(ee_vu0_ir_fmac_s[0-9_]+(?:true|false))\([^\r\n]+\) \{", re.M)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inline_single_use(raw):
    """Only annotate a definition with exactly one direct call and no other use."""
    names = []
    def rewrite(match):
        name = match[1]
        ref = rb"@" + re.escape(name) + rb"(?![\w.$])"
        if len(re.findall(ref, raw)) != 2:
            return match[0]
        if len(re.findall(rb"\bcall i32 " + ref + rb"\(", raw)) != 1:
            return match[0]
        names.append(name.decode("ascii"))
        return match[0][:-2] + b" alwaysinline {"
    result = HEADER.sub(rewrite, raw)
    return result, names


def emit_hot_o3(raw, triple, symbol, object_output=None):
    """Use full O3 middle-end and native O3 backend, without altering source IR."""
    import time
    import llvmlite.binding as llvm
    from verify_ir import native_target_identity
    from ee_ownership_binary import native_executable_sections
    start = time.monotonic()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = native_target_identity(5)
    if target["triple"] != triple:
        raise ValueError("O3 target differs from native host")
    machine = llvm.Target.from_triple(triple).create_target_machine(
        cpu=target["cpu"], features=target["features"],
        opt=3, reloc="pic", codemodel="default")
    with llvm.create_context() as context, llvm.parse_assembly(
            raw.decode("utf-8"), context=context) as module:
        module.triple = triple
        module.data_layout = str(machine.target_data)
        module.verify()
        def public():
            return sorted(fn.name for fn in module.functions
                          if not fn.is_declaration and fn.linkage == llvm.Linkage.external)
        before = public()
        if symbol not in before:
            raise ValueError("guest entry missing before O3")
        with llvm.create_pipeline_tuning_options(3) as tuning:
            with llvm.create_pass_builder(machine, tuning) as passes:
                with passes.getModulePassManager() as pipeline:
                    pipeline.run(module, passes)
        module.verify()
        if public() != before:
            raise ValueError("O3 changed external definition inventory")
        data = machine.emit_object(module)
    executable, _ = native_executable_sections(data)
    if object_output is not None:
        with object_output.open("xb") as stream:
            stream.write(data)
    return {"object_bytes": len(data), "executable_bytes": executable,
            "sha256": sha(data), "seconds": time.monotonic() - start,
            "middle_end_opt": 3, "backend_opt": 3}


def hot_o3_trial(task):
    from benchmark_ee_native_size import emit_native
    name, raw, triple, symbol, expected, destination = task
    control = emit_native(raw, triple, symbol, "O5")
    if control["sha256"] != expected:
        raise ValueError("O5 control did not reproduce original: " + name)
    candidate = emit_hot_o3(raw, triple, symbol, destination)
    return name, control, candidate


def stage(selection, ir, profile, output, mode="inline", jobs=8):
    from benchmark_ee_native_size import emit_native
    from link_native_objects import load_inputs
    from verify_ir import native_target_identity, ir_pipeline_id
    from ee_ownership_binary import native_executable_sections

    ir, profile, output = (p.resolve() for p in (ir, profile, output))
    objects = profile / "objects"
    if (output.exists() or output.is_relative_to(ir.parent)
            or output.is_relative_to(profile)):
        raise ValueError("use a fresh output outside the translation cache")
    if os.name != "nt" or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
        raise ValueError("Windows native pilot requires disabled ALU certificates")
    meta_bytes = (ir / "ee_functions.json").read_bytes()
    translation = json.loads((profile / "translation-cache.json").read_bytes())
    baseline_library = profile / "game-translated.dll"
    if (translation["emit_opt"] != 5 or translation["metadata_sha256"] != sha(meta_bytes)
            or translation["native_target"] != native_target_identity(5)
            or translation["ir_pipeline"] != ir_pipeline_id(5)
            or translation["inputs"]["native_alu_certificate_mode"] != sha(b"0")
            or sha(baseline_library.read_bytes()) != translation["library_sha256"]):
        raise ValueError("baseline cache identity differs")
    with zipfile.ZipFile(selection) as archive:
        receipt = json.loads(archive.read("selection.json"))
        if (receipt["schema"] != "ee-hot-ir-selection-v1"
                or receipt["metadata_sha256"] != sha(meta_bytes)
                or archive.read("ee_functions.json") != meta_bytes):
            raise ValueError("hot selection metadata differs")
        selected = {}
        for name, digest in receipt["modules"].items():
            if Path(name).name != name or not name.endswith(".ll"):
                raise ValueError("invalid selected module name")
            data = archive.read("ir/" + name)
            if sha(data) != digest or translation["ir_hashes"].get(name) != digest:
                raise ValueError("captured IR differs from baseline: " + name)
            selected[name] = data

    metadata, manifest, functions, linked = load_inputs(ir / "ee_functions.json", objects)
    if (manifest.get("optimization_level") != 5
            or manifest.get("native_target") != translation["native_target"]
            or manifest.get("ir_pipeline") != translation["ir_pipeline"]
            or manifest.get("experimental_mixed_pipeline")):
        raise ValueError("expected production O5 object inventory")
    by_pc = {fn["vaddr"]: fn for fn in functions}
    records = {Path(item["input"]).name: item for item in manifest["objects"]}
    paths = {Path(fn["ir_path"]).name: path for fn, path in zip(functions, linked)}
    trials = {}
    replacements = {}
    if mode == "hot-o3":
        if not 1 <= len(selected) <= 1000 or not 1 <= jobs <= 16:
            raise ValueError("hot O3 pilot requires 1–1000 selected modules and 1–16 jobs")
        by_name = {Path(fn["ir_path"]).name: fn for fn in functions}
        pilot = [by_name[name] for name in sorted(selected)]
    else:
        pilot = [by_pc[pc] for pc in (DISCARD_PILOT if mode == "vector-discard" else PILOT)]
    for fn in pilot:
        name = Path(fn["ir_path"]).name
        raw = (ir / name).read_bytes()
        record = records[name]
        original = paths[name].read_bytes()
        if (raw != selected.get(name) or sha(raw) != record["ir_sha256"]
                or sha(original) != record["sha256"]):
            raise ValueError("hot source/object mismatch: " + name)
        if mode == "hot-o3":
            trials[name] = (raw, [], None, fn["host_symbol"])
        else:
            changed, helpers = (vector_discard(raw) if mode == "vector-discard"
                                else inline_single_use(raw))
            if not helpers:
                raise ValueError("no eligible helpers: " + name)
            control = emit_native(raw, manifest["target_triple"], fn["host_symbol"], "O5")
            if control["sha256"] != record["sha256"]:
                raise ValueError("O5 control did not reproduce original: " + name)
            trials[name] = (changed, helpers, control, fn["host_symbol"])
    output.mkdir(parents=True, exist_ok=False)
    staged = output / "objects"
    staged.mkdir()
    rows = []
    compiled = {}
    if mode == "hot-o3":
        tasks = [(name, raw, manifest["target_triple"], symbol, records[name]["sha256"],
                  staged / (Path(name).stem + "." + mode + ".o"))
                 for name, (raw, _, _, symbol) in trials.items()]
        # Bounded worker lifetimes, as LLVM can retain substantial per-process memory.
        for first in range(0, len(tasks), 64 * jobs):
            with ProcessPoolExecutor(max_workers=jobs) as pool:
                for name, control, candidate in pool.map(hot_o3_trial, tasks[first:first + 64 * jobs]):
                    compiled[name] = (control, candidate)
                    print(f"[{len(compiled)}/{len(tasks)}] O3 {name}", flush=True)
    for name, (changed, helpers, control, symbol) in trials.items():
        destination = staged / (Path(name).stem + "." + mode + ".o")
        if mode == "hot-o3":
            control, candidate = compiled[name]
        else:
            candidate = emit_native(changed, manifest["target_triple"], symbol,
                                    "O5", object_output=destination)
        # A speed-oriented discard branch can cost some static bytes.
        # Bound growth per module; keep the original stricter inline gate.
        size_percent = {"inline": 100, "vector-discard": 110, "hot-o3": 150}[mode]
        accepted = (candidate["sha256"] != control["sha256"]
                    and candidate["object_bytes"] * 100 <= control["object_bytes"] * size_percent
                    and candidate["executable_bytes"] * 100 <= control["executable_bytes"] * size_percent)
        rows.append({"module": name, "helpers": helpers, "control": control,
                     "candidate": candidate, "accepted_size_gate": accepted,
                     "transformed_ir_sha256": sha(changed)})
        if accepted:
            replacements[name] = dict(records[name], object=destination.name,
                                      size_bytes=candidate["object_bytes"],
                                      sha256=candidate["sha256"],
                                      transformed_ir_sha256=sha(changed),
                                      experimental_fmac_inline=(mode == "inline"),
                                      experimental_vector_discard=(mode == "vector-discard"),
                                      experimental_hot_o3=(mode == "hot-o3"))
        else:
            destination.unlink()
        print(f"{name}: {control['executable_bytes']} -> "
              f"{candidate['executable_bytes']} executable bytes; "
              f"{'selected' if accepted else 'retained baseline'}", flush=True)
    if not replacements:
        raise ValueError("no candidate passed the conservative size gate")
    entries = [replacements.get(name, dict(record, object=str(paths[name])))
               for name, record in records.items()]
    overlay = dict(manifest, objects=entries,
                   object_bytes=sum(item["size_bytes"] for item in entries),
                   experimental_mixed_pipeline={"inline": "O5-single-use-fmac-inline",
                                                "vector-discard": "O5-vector-discard",
                                                "hot-o3": "O5-profile-selected-O3"}[mode],
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")
    (output / "ee_functions.json").write_bytes(meta_bytes)
    library = output / baseline_library.name
    subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                    str(output / "ee_functions.json"), str(staged), str(library)], check=True)
    data = library.read_bytes()
    if sha(baseline_library.read_bytes()) != translation["library_sha256"]:
        raise ValueError("baseline library changed during pilot")
    plan = {
        "mode": {"inline": "experimental_single_use_fmac_inline",
                 "vector-discard": "experimental_vector_discard",
                 "hot-o3": "experimental_hot_o3"}[mode],
        "selection_profile_sha256": receipt["profile_sha256"],
        "selection_exclusive_ns": receipt["selected_exclusive_ns"],
        "profile_exclusive_ns": receipt["total_exclusive_ns"],
        "jobs": jobs if mode == "hot-o3" else 1,
        "size_gate_percent": size_percent,
        "metadata_sha256": sha(meta_bytes),
        "source_manifest_sha256": sha((objects / "objects.json").read_bytes()),
        "baseline_library_sha256": translation["library_sha256"],
        "native_target": manifest["native_target"],
        "source_functions": len(functions),
        "retained_functions": len(functions),
        "replaced_modules": len(replacements),
        "runtime_validation_required": True,
        "linked_library_bytes": len(data),
        "linked_library_sha256": sha(data),
        "linked_executable_raw_bytes": native_executable_sections(data)[0],
        "modules": rows,
    }
    (output / "helper-plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(f"Isolated pilot: {len(replacements)}/{len(pilot)} modules; {library}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("selection", type=Path)
    parser.add_argument("--ir", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--mode", choices=("inline", "vector-discard", "hot-o3"), default="inline")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    try:
        stage(args.selection, args.ir, args.profile, args.out, args.mode, args.jobs)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError,
            zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"EE optimization pilot refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
