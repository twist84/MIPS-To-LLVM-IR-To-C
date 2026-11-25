#!/usr/bin/env python3
"""Link emitted per-function host objects with the EE runtime and PC dispatcher.

The input metadata is ee_functions.json from ee_recomp. The generated shared
library exports ee_game_run(cpu, runtime, budget, out_count, out_pc, out_status).
It embeds the guest block-PC ownership map and a parallel array of native
function pointers, then delegates to ee_dispatch_run().

This produces a linked translated-code library. It does not provide BIOS, IOP,
GS, DMA, TLB/MMIO devices, or a complete console host.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def select_native_link_compiler(requested=None, platform=None, environment=None):
    """Choose a driver compatible with this linker's Clang/GNU-style options.

    The CMake CXX compiler may be MSVC cl.exe. That is a valid host-project
    compiler, but it cannot compile our generated dispatcher/runtime objects
    with -std, -O, -c/-o or link LLVM's Windows MSVC-target objects this way.
    Keep guest translation and provenance untouched: select only the native
    *link* toolchain here.
    """
    platform = sys.platform if platform is None else platform
    environment = os.environ if environment is None else environment
    override = requested if requested is not None else environment.get("CXX")

    if platform != "win32":
        return override or "c++"

    if override:
        candidates = [override]
    else:
        candidates = ["clang++.exe", "clang++"]
        llvm_dir = environment.get("LLVM_INSTALL_DIR")
        if llvm_dir:
            candidates.append(str(Path(llvm_dir) / "bin" / "clang++.exe"))
        for variable in ("ProgramFiles", "ProgramW6432"):
            program_files = environment.get(variable)
            if program_files:
                candidates.append(str(Path(program_files) / "LLVM" / "bin" / "clang++.exe"))
        candidates.append(str(Path("C:/Program Files/LLVM/bin/clang++.exe")))

    for candidate in candidates:
        # Distinguish clang++ (GNU-style driver for MSVC-target objects) from
        # clang-cl/cl.exe (MSVC-style command-line parser). No silent fallback.
        if Path(candidate).name.lower() not in ("clang++", "clang++.exe"):
            raise ValueError(
                f"native Windows linker requires clang++.exe, not {candidate!r}; "
                "set CXX or --cxx to the LLVM clang++.exe executable"
            )
        resolved = shutil.which(candidate) or candidate
        if Path(resolved).is_file():
            return str(Path(resolved).resolve())

    raise ValueError(
        "LLVM clang++.exe not found for native Windows linking; "
        "install LLVM, add clang++ to PATH, or set CXX/--cxx to "
        "the absolute path of clang++.exe"
    )


def cpp_string(value):
    return (
        '"'
        + str(value).replace("\\", "\\\\").replace('"', '\\"')
        .replace("\n", "\\n").replace("\r", "\\r")
        + '"'
    )


def parse_u32(value, field):
    try:
        number = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"invalid {field}: {value!r}") from exc
    if number < 0 or number > 0xFFFFFFFF:
        raise ValueError(f"{field} is outside uint32: {value!r}")
    return number


def build_dispatch_source(metadata, functions):
    lines = [
        '#include "ee_dispatcher.h"',
        '#include "ee_dispatch_index.h"',
        "#include <cstdint>",
        "#include <utility>",
        "",
        "extern \"C\"",
        "{",
    ]
    for fn in functions:
        lines.append(
            f"    std::uint32_t {fn['host_symbol']}("
            "ee::s_ee_cpu_state*, ee::s_ee_runtime*);"
        )
    lines += [
        "}",
        "",
        "namespace",
        "{",
        "ee::s_recompiled_elf make_compiled_metadata()",
        "{",
        "    ee::s_recompiled_elf compiled{};",
        f"    compiled.entry_point = 0x{parse_u32(metadata.get('entry_point'), 'entry_point'):08x}u;",
        f"    compiled.function_count = {len(functions)}u;",
        f"    compiled.functions_discovered = {parse_u32(metadata.get('functions_discovered', len(functions)), 'functions_discovered')}u;",
    ]

    for fn in functions:
        block_values = ", ".join(f"0x{pc:08x}u" for pc in fn["block_pcs"])
        lines += [
            "    {",
            "        ee::s_recompiled_fn function{};",
            f"        function.name = {cpp_string(fn.get('name', ''))};",
            f"        function.host_symbol = {cpp_string(fn['host_symbol'])};",
            f"        function.vaddr = 0x{fn['vaddr']:08x}u;",
            f"        function.block_count = {len(fn['block_pcs'])}u;",
            f"        function.block_pcs = {{{block_values}}};",
            f"        function.ir_path = {cpp_string(fn.get('ir_path', ''))};",
            "        compiled.functions.push_back(std::move(function));",
            "    }",
        ]

    lines += [
        "    return compiled;",
        "}",
        "",
        "const ee::s_recompiled_elf g_compiled = make_compiled_metadata();",
        "const ee::t_ee_translated_entry g_entries[] =",
        "{",
    ]
    for fn in functions:
        lines.append(f"    &{fn['host_symbol']},")
    lines += [
        "};",
        "const ee::s_ee_dispatch_index g_index = ee::ee_build_dispatch_index(g_compiled);",
        "const ee::s_ee_dispatch_table g_dispatch_table",
        "{",
        "    &g_compiled,",
        "    g_entries,",
        "    sizeof(g_entries) / sizeof(g_entries[0])",
        "};",
        "} // namespace",
        "",
        "#if defined(_WIN32)",
        "#define EE_GAME_EXPORT __declspec(dllexport)",
        "#else",
        "#define EE_GAME_EXPORT",
        "#endif",
        'extern "C" EE_GAME_EXPORT ee::t_ee_translated_entry ee_game_resolve_entry(',
        "    std::uint32_t guest_pc)",
        "{",
        "    return ee::ee_resolve_dispatch_entry(g_dispatch_table, g_index, guest_pc);",
        "}",
        "",
        'extern "C" EE_GAME_EXPORT std::uint32_t ee_game_resolve_owner(',
        "    std::uint32_t guest_pc, ee::t_ee_translated_entry entry)",
        "{",
        "    return ee::ee_resolve_dispatch_owner_vaddr(g_dispatch_table, g_index, guest_pc, entry);",
        "}",
        "",
        'extern "C" EE_GAME_EXPORT std::uint32_t ee_game_run(',
        "    ee::s_ee_cpu_state* cpu,",
        "    ee::s_ee_runtime* runtime,",
        "    std::uint32_t max_dispatches,",
        "    std::uint32_t* out_dispatch_count,",
        "    std::uint32_t* out_guest_pc,",
        "    std::uint32_t* out_status)",
        "{",
        "    const ee::s_ee_dispatch_result result =",
        "        ee::ee_dispatch_run_indexed(",
        "            cpu, runtime, g_dispatch_table, g_index, max_dispatches);",
        "    if (out_dispatch_count) *out_dispatch_count = result.dispatch_count;",
        "    if (out_guest_pc) *out_guest_pc = result.guest_pc;",
        "    if (out_status) *out_status = static_cast<std::uint32_t>(result.status);",
        "    return result.exit_reason;",
        "}",
        "",
    ]
    return "\n".join(lines)


def build_dispatch_data_source(metadata, functions):
    """Function table as a header-free C translation unit.

    Linked with src/runtime/ee_dispatch_data_glue.cpp, which rebuilds the
    s_recompiled_elf metadata at load and exports ee_game_run.
    """
    if not functions:
        raise ValueError("data dispatch requires at least one function")
    offsets = [0]
    block_pcs = []
    for fn in functions:
        block_pcs.extend(fn["block_pcs"])
        offsets.append(len(block_pcs))

    def array(values):
        return ",\n".join(
            "    " + ", ".join(f"0x{value:08x}u" for value in values[i:i + 8])
            for i in range(0, len(values), 8)
        )

    lines = ["/* Generated by link_native_objects.py --data-dispatch. */"]
    for fn in functions:
        lines.append(f"extern unsigned int {fn['host_symbol']}(void*, void*);")
    lines += [
        f"const unsigned int ee_dispatch_data_function_count = {len(functions)}u;",
        "const unsigned int ee_dispatch_data_entry_point = "
        f"0x{parse_u32(metadata.get('entry_point'), 'entry_point'):08x}u;",
        "const unsigned int ee_dispatch_data_functions_discovered = "
        f"{parse_u32(metadata.get('functions_discovered', len(functions)), 'functions_discovered')}u;",
        "const unsigned int ee_dispatch_data_vaddrs[] = {",
        array([fn["vaddr"] for fn in functions]),
        "};",
        "const unsigned int ee_dispatch_data_block_offsets[] = {",
        array(offsets),
        "};",
        "const unsigned int ee_dispatch_data_block_pcs[] = {",
        array(block_pcs),
        "};",
        "unsigned int (*const ee_dispatch_data_entries[])(void*, void*) = {",
    ]
    lines += [f"    {fn['host_symbol']}," for fn in functions]
    lines += ["};", ""]
    return "\n".join(lines)


# A full game's metadata contains thousands of functions. Keep it in fixed
# chunks: appending one function then changes only the final chunk's object,
# rather than recompiling a multi-megabyte dispatcher on every replay.
DISPATCH_CHUNK_SIZE = 256


def build_dispatch_chunk_source(functions, first, index):
    lines = [
        '#include "ee_dispatcher.h"',
        '#include <cstdint>',
        '#include <utility>',
        'extern "C"',
        '{',
    ]
    for fn in functions:
        lines.append(
            f"    std::uint32_t {fn['host_symbol']}("
            "ee::s_ee_cpu_state*, ee::s_ee_runtime*);"
        )
    lines += [
        '}',
        f'extern "C" void ee_dispatch_chunk_{index:04d}(',
        '    ee::s_recompiled_elf* compiled,',
        '    ee::t_ee_translated_entry* entries)',
        '{',
    ]
    for offset, fn in enumerate(functions):
        block_values = ', '.join(
            f"0x{pc:08x}u" for pc in fn['block_pcs']
        )
        lines += [
            '    {',
            '        ee::s_recompiled_fn function{};',
            f"        function.name = {cpp_string(fn.get('name', ''))};",
            f"        function.host_symbol = {cpp_string(fn['host_symbol'])};",
            f"        function.vaddr = 0x{fn['vaddr']:08x}u;",
            f"        function.block_count = {len(fn['block_pcs'])}u;",
            f"        function.block_pcs = {{{block_values}}};",
            f"        function.ir_path = {cpp_string(fn.get('ir_path', ''))};",
            '        compiled->functions.push_back(std::move(function));',
            f"        entries[{first + offset}] = &{fn['host_symbol']};",
            '    }',
        ]
    lines.append('}')
    lines.append('')
    return '\n'.join(lines)


def build_chunked_dispatch_source(metadata, functions, chunk_count):
    lines = [
        '#include "ee_dispatcher.h"',
        '#include "ee_dispatch_index.h"',
        '#include <cstdint>',
        '#include <vector>',
        'extern "C"',
        '{',
    ]
    for index in range(chunk_count):
        lines += [
            f'    void ee_dispatch_chunk_{index:04d}(',
            '        ee::s_recompiled_elf*, ee::t_ee_translated_entry*);',
        ]
    lines += [
        '}',
        'namespace',
        '{',
        'struct s_compiled_dispatch',
        '{',
        '    ee::s_recompiled_elf compiled{};',
        '    std::vector<ee::t_ee_translated_entry> entries;',
        '    ee::s_ee_dispatch_index index;',
        '    s_compiled_dispatch()',
        '    {',
        f"        compiled.entry_point = 0x{parse_u32(metadata.get('entry_point'), 'entry_point'):08x}u;",
        f'        compiled.function_count = {len(functions)}u;',
        f"        compiled.functions_discovered = {parse_u32(metadata.get('functions_discovered', len(functions)), 'functions_discovered')}u;",
        f'        compiled.functions.reserve({len(functions)}u);',
        f'        entries.resize({len(functions)}u);',
    ]
    for index in range(chunk_count):
        lines.append(
            f'        ee_dispatch_chunk_{index:04d}(&compiled, entries.data());'
        )
    lines += [
        '        // Built once at load: O(log n) guest-PC owner lookup.',
        '        index = ee::ee_build_dispatch_index(compiled);',
        '    }',
        '};',
        'const s_compiled_dispatch g_dispatch{};',
        'const ee::s_ee_dispatch_table g_dispatch_table',
        '{',
        '    &g_dispatch.compiled,',
        '    g_dispatch.entries.data(),',
        '    g_dispatch.entries.size()',
        '};',
        '} // namespace',
        '#if defined(_WIN32)',
        '#define EE_GAME_EXPORT __declspec(dllexport)',
        '#else',
        '#define EE_GAME_EXPORT',
        '#endif',
        'extern "C" EE_GAME_EXPORT ee::t_ee_translated_entry ee_game_resolve_entry(',
        '    std::uint32_t guest_pc)',
        '{',
        '    return ee::ee_resolve_dispatch_entry(g_dispatch_table, g_dispatch.index, guest_pc);',
        '}',
        '',
        'extern "C" EE_GAME_EXPORT std::uint32_t ee_game_resolve_owner(',
        '    std::uint32_t guest_pc, ee::t_ee_translated_entry entry)',
        '{',
        '    return ee::ee_resolve_dispatch_owner_vaddr(g_dispatch_table, g_dispatch.index, guest_pc, entry);',
        '}',
        '',
        'extern "C" EE_GAME_EXPORT std::uint32_t ee_game_run(',
        '    ee::s_ee_cpu_state* cpu,',
        '    ee::s_ee_runtime* runtime,',
        '    std::uint32_t max_dispatches,',
        '    std::uint32_t* out_dispatch_count,',
        '    std::uint32_t* out_guest_pc,',
        '    std::uint32_t* out_status)',
        '{',
        '    const ee::s_ee_dispatch_result result =',
        '        ee::ee_dispatch_run_indexed(',
        '            cpu, runtime, g_dispatch_table, g_dispatch.index, max_dispatches);',
        '    if (out_dispatch_count) *out_dispatch_count = result.dispatch_count;',
        '    if (out_guest_pc) *out_guest_pc = result.guest_pc;',
        '    if (out_status) *out_status = static_cast<std::uint32_t>(result.status);',
        '    return result.exit_reason;',
        '}',
        '',
    ]
    return '\n'.join(lines)


def cached_dispatch_objects(output, metadata, functions, cxx,
                            source_root, include_args, compile_flags):
    # The generated source, compiler signature, runtime headers, and flags
    # all participate in cache validation. Never reuse stale native code.
    compiler_path = Path(shutil.which(cxx) or cxx).resolve()
    if not compiler_path.is_file():
        raise ValueError(f'dispatch compiler unavailable: {cxx}')
    compiler_stat = compiler_path.stat()
    compiler_info = {
        'path': str(compiler_path),
        'size': compiler_stat.st_size,
        'mtime_ns': compiler_stat.st_mtime_ns,
    }
    dependency_key = runtime_cache_key(
        [], source_root, compiler_info, compile_flags + include_args
    )
    wrapper_flags = [
        "-O2" if flag in ("-O0", "-O1") else flag for flag in compile_flags
    ]
    wrapper_key = runtime_cache_key(
        [], source_root, compiler_info, wrapper_flags + include_args
    )
    cache_dir = output.parent / (output.name + '.dispatch-cache')
    cache_dir.mkdir(parents=True, exist_ok=True)
    chunk_count = (len(functions) + DISPATCH_CHUNK_SIZE - 1) // DISPATCH_CHUNK_SIZE
    units = [(output.with_suffix(output.suffix + '.dispatch.cpp'),
              output.with_suffix(output.suffix + '.dispatch.o'),
              build_chunked_dispatch_source(metadata, functions, chunk_count))]
    for index, first in enumerate(range(0, len(functions), DISPATCH_CHUNK_SIZE)):
        name = f'chunk-{index:04d}'
        units.append((
            cache_dir / (name + '.cpp'),
            cache_dir / (name + '.o'),
            build_dispatch_chunk_source(
                functions[first:first + DISPATCH_CHUNK_SIZE], first, index
            ),
        ))
    object_paths = []
    reused = 0
    for unit_index, (source, obj, generated) in enumerate(units):
        # The first unit exports ee_game_run, called at every EE boundary.
        # Optimize just that wrapper; leave large metadata chunks at the
        # selected cheap construction optimization level.
        flags = wrapper_flags if unit_index == 0 else compile_flags
        unit_key = wrapper_key if unit_index == 0 else dependency_key
        encoded = generated.encode('utf-8')
        key = hashlib.sha256(
            unit_key.encode('ascii') + b'\0' + encoded
        ).hexdigest()
        record_path = obj.with_suffix(obj.suffix + '.json')
        try:
            record = json.loads(record_path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            record = {}
        valid = (
            obj.is_file() and record.get('key') == key and
            record.get('object_sha256') == hashlib.sha256(obj.read_bytes()).hexdigest()
        )
        if not valid:
            source.write_bytes(encoded)
            temporary = obj.with_suffix(obj.suffix + '.tmp')
            try:
                subprocess.run([
                    cxx, *flags, *include_args,
                    '-c', str(source), '-o', str(temporary)
                ], check=True)
                os.replace(temporary, obj)
                record_path.write_text(json.dumps({
                    'key': key,
                    'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
                }, indent=2) + '\n', encoding='utf-8')
            finally:
                if temporary.exists():
                    temporary.unlink()
        else:
            reused += 1
        object_paths.append(obj)
    print(
        f'link_native_objects: dispatcher {len(object_paths)} units, '
        f'reused {reused}, compiled {len(object_paths)-reused}',
        flush=True,
    )
    return object_paths


def load_inputs(metadata_path, object_dir):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    object_manifest_path = object_dir / "objects.json"
    if not object_manifest_path.is_file():
        raise ValueError(f"object manifest not found: {object_manifest_path}")

    object_manifest = json.loads(object_manifest_path.read_text(encoding="utf-8"))
    if (object_manifest.get("optimization_level") == 5 or
            object_manifest.get("native_target") is not None):
        from verify_ir import native_target_identity
        if (object_manifest.get("optimization_level") != 5 or
                object_manifest.get("native_target") != native_target_identity(5)):
            raise ValueError("host-native object manifest CPU target mismatch")
    if not object_manifest.get("complete"):
        raise ValueError("objects.json does not report a complete emission")
    if object_manifest.get("target_triple") != object_manifest.get("target_triple", "").strip():
        raise ValueError("object manifest has an invalid target triple")

    functions = metadata.get("functions")
    objects = object_manifest.get("objects")
    if not isinstance(functions, list) or not functions:
        raise ValueError("ee_functions.json contains no functions")
    if not isinstance(objects, list) or len(objects) != len(functions):
        raise ValueError("object count does not match function metadata")

    object_by_input = {}
    for item in objects:
        input_name = Path(item["input"]).name
        if input_name in object_by_input:
            raise ValueError(f"duplicate object manifest input: {input_name}")
        object_by_input[input_name] = item

    linked_objects = []
    validated_functions = []
    seen_symbols = set()
    resolved = []

    for function in functions:
        symbol = function.get("host_symbol", "")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol):
            raise ValueError(f"invalid or missing host_symbol for {function.get('name')!r}")
        if symbol in seen_symbols:
            raise ValueError(f"duplicate native function symbol: {symbol}")
        seen_symbols.add(symbol)

        ir_name = Path(function.get("ir_path", "")).name
        item = object_by_input.get(ir_name)
        if item is None:
            raise ValueError(f"no object manifest entry for IR module {ir_name}")
        if symbol not in item.get("defined_symbols", []):
            raise ValueError(f"object {item['object']} does not define {symbol}")

        object_path = object_dir / item["object"]
        if not object_path.is_file():
            raise ValueError(f"object file missing: {object_path}")
        resolved.append((function, symbol, item, object_path))

    # Every object is still fully read and SHA-256 verified on every link.
    # Hash in parallel: a full SDBZ image has ~8,000 objects (~100 MB) and
    # a live cache-miss relink otherwise spends ~3 s here single-threaded.
    def measure(path):
        data = path.read_bytes()
        return len(data), hashlib.sha256(data).hexdigest()

    workers = min(32, (os.cpu_count() or 1) + 4)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        measured = list(pool.map(measure, [entry[3] for entry in resolved]))

    for (function, symbol, item, object_path), (size, digest) in zip(resolved, measured):
        if size != item["size_bytes"]:
            raise ValueError(f"object size mismatch: {object_path}")
        if digest != item["sha256"]:
            raise ValueError(f"object digest mismatch: {object_path}")

        block_pcs = [
            parse_u32(pc, f"{symbol}.block_pcs") for pc in function.get("block_pcs", [])
        ]
        if not block_pcs:
            raise ValueError(f"function {symbol} has no dispatchable block PCs")
        if len(block_pcs) != len(set(block_pcs)):
            raise ValueError(f"function {symbol} has duplicate block PCs")

        validated = {
            "name": function.get("name", ""),
            "host_symbol": symbol,
            "vaddr": parse_u32(function.get("vaddr"), f"{symbol}.vaddr"),
            "block_pcs": block_pcs,
            "ir_path": function.get("ir_path", ""),
        }
        validated_functions.append(validated)
        linked_objects.append(object_path)

    if len(functions) != parse_u32(metadata.get("function_count", len(functions)), "function_count"):
        raise ValueError("function_count does not match the function array")

    return metadata, object_manifest, validated_functions, linked_objects


def runtime_cache_key(sources, source_root, compiler, flags):
    """Fingerprint the compilation ABI and all in-tree C++ include inputs.

    This intentionally invalidates on any project header change rather than
    risk stale inline definitions in cached runtime objects. The fingerprint
    does not depend on guest IR, function count or generated dispatcher code.
    """
    info = {
        "schema": "ee-runtime-objects-v1",
        "compiler": compiler,
        "flags": flags,
        "platform": sys.platform,
        "environment": {
            name: os.environ.get(name, "")
            for name in ("INCLUDE", "CPATH", "CPLUS_INCLUDE_PATH",
                         "CXXFLAGS", "SDKROOT")
        },
    }
    hasher = hashlib.sha256(json.dumps(info, sort_keys=True).encode("utf-8"))
    paths = list(sources)
    for directory in ("include", "src/runtime", "src/ee"):
        paths.extend(
            p for p in (source_root / directory).rglob("*")
            if p.is_file() and p.suffix in (".h", ".hpp", ".inl")
        )
    for path in sorted(set(paths)):
        relative = path.relative_to(source_root).as_posix()
        hasher.update(relative.encode("utf-8"))
        hasher.update(b"\\0")
        hasher.update(hashlib.sha256(path.read_bytes()).digest())
    return hasher.hexdigest()


def cached_runtime_objects(output, sources, source_root, cxx,
                           include_args, compile_flags):
    """Compile fixed EE runtime objects once; verify every reuse by hash."""
    started = time.monotonic()
    cache_dir = output.parent / (output.name + ".runtime-cache")
    cache_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = cache_dir / "runtime-objects.json"

    compiler_path = shutil.which(cxx) or cxx
    compiler_file = Path(compiler_path)
    if not compiler_file.is_file():
        raise ValueError(f"runtime compiler executable unavailable: {cxx}")
    stat = compiler_file.stat()
    compiler = {
        "path": str(compiler_file.resolve()),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
    }
    key = runtime_cache_key(
        sources, source_root, compiler, compile_flags + include_args
    )
    object_paths = [
        cache_dir / (source.stem + "-" + hashlib.sha256(
            str(source.relative_to(source_root)).encode("utf-8")
        ).hexdigest()[:12] + ".o")
        for source in sources
    ]

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        manifest = {}
    recorded = manifest.get("objects", [])
    reuse = (
        manifest.get("key") == key and
        len(recorded) == len(object_paths) and
        all(
            item.get("path") == path.name and path.is_file() and
            item.get("sha256") == hashlib.sha256(path.read_bytes()).hexdigest()
            for item, path in zip(recorded, object_paths)
        )
    )
    if reuse:
        print(
            "link_native_objects: reused "
            f"{len(object_paths)} cached runtime objects "
            f"({time.monotonic() - started:.2f}s)",
            flush=True,
        )
        return object_paths

    print(
        "link_native_objects: compiling "
        f"{len(sources)} runtime sources (cache miss)",
        flush=True,
    )
    for source, obj in zip(sources, object_paths):
        temporary = obj.with_suffix(".tmp.o")
        try:
            subprocess.run(
                [cxx, *compile_flags, *include_args,
                 "-c", str(source), "-o", str(temporary)],
                check=True,
            )
            temporary.replace(obj)
        finally:
            if temporary.exists():
                temporary.unlink()

    manifest = {
        "key": key,
        "objects": [
            {"path": path.name, "sha256": hashlib.sha256(
                path.read_bytes()
            ).hexdigest()}
            for path in object_paths
        ],
    }
    temporary_manifest = manifest_path.with_suffix(".tmp")
    temporary_manifest.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    temporary_manifest.replace(manifest_path)
    print(
        "link_native_objects: compiled runtime objects "
        f"({time.monotonic() - started:.2f}s)",
        flush=True,
    )
    return object_paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("functions_json", type=Path, help="ee_functions.json from ee_recomp")
    parser.add_argument("object_dir", type=Path, help="directory containing .o files and objects.json")
    parser.add_argument("output", type=Path, help="output shared library path")
    parser.add_argument("--cache-base", type=Path,
                        help="Stable output pathname used only for dispatcher/runtime object caches")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cxx", default=None,
                        help="native C++ compiler (Windows requires clang++.exe)")
    parser.add_argument(
        "--pdb", action="store_true",
        help="Generate a matching Windows PDB using Clang, CodeView and LLD",
    )
    parser.add_argument(
        "--data-dispatch", action="store_true",
        help="Emit the function table as plain C data linked with cached "
             "generic glue (fast small libraries, e.g. live overlays)",
    )
    parser.add_argument(
        "--dispatcher-opt",
        type=int,
        choices=range(0, 4),
        default=0,
        help="metadata chunk optimisation level (default: 0); entry wrapper at least O2",
    )
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    try:
        # Prefer an explicitly supplied CXX. Otherwise discover Windows
        # LLVM clang++ rather than using CMake's potentially MSVC cl.exe.
        args.cxx = select_native_link_compiler(args.cxx)
        if args.pdb and sys.platform != "win32":
            raise ValueError("--pdb requires a Windows host and a Clang/LLD MinGW toolchain")
        pdb_path = args.output.with_suffix(".pdb") if args.pdb else None
        if args.pdb:
            # GNU/binutils and GCC cannot emit a matching CodeView/PDB pair.
            # Do not silently write a partial or unrelated .pdb file.
            compiler_info = subprocess.run(
                [args.cxx, "--version"], capture_output=True, text=True, check=True
            )
            if "clang" not in (compiler_info.stdout + compiler_info.stderr).lower():
                raise ValueError("--pdb requires clang++ (with lld), not GCC")
        metadata, object_manifest, functions, objects = load_inputs(
            args.functions_json.resolve(), args.object_dir.resolve()
        )

        required_sources = [
            source_root / "src/runtime/ee_memory.cpp",
            source_root / "src/runtime/ee_unaligned.cpp",
            source_root / "src/runtime/ee_exceptions.cpp",
            source_root / "src/runtime/ee_vu0.cpp",
            source_root / "src/ee/recompiled_functions.cpp",
            source_root / "src/ee/ee_dispatcher.cpp",
        ]
        for source in required_sources:
            if not source.is_file():
                raise ValueError(f"required runtime source not found: {source}")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        dispatch_source = args.output.with_suffix(args.output.suffix + ".dispatch.cpp")
        dispatch_object = args.output.with_suffix(args.output.suffix + ".dispatch.o")
        include_args = [
            "-I" + str(source_root / "include"),
            "-I" + str(source_root / "include/ee"),
            "-I" + str(source_root / "src/runtime"),
        ]
        compile_flags = ["-std=c++17", f"-O{args.dispatcher_opt}"]
        if sys.platform in ("linux", "darwin"):
            compile_flags.append("-fPIC")
        if args.pdb:
            compile_flags += ["-g", "-gcodeview"]
        cache_base = args.cache_base.resolve() if args.cache_base else args.output

        # Runtime sources do not depend on the number of translated guest
        # functions. Cache their compiled objects so boot auto-expansion does
        # not rebuild the same six translation units on every relink.
        runtime_flags = ["-std=c++17", "-O2"]
        if sys.platform in ("linux", "darwin"):
            runtime_flags.append("-fPIC")
        if args.pdb:
            runtime_flags += ["-g", "-gcodeview"]

        if args.data_dispatch:
            # Plain C table + cached generic glue: no C++ headers to parse
            # per library, which dominated small overlay links (~1.1 s).
            data_source = args.output.with_suffix(args.output.suffix + ".dispatch-data.c")
            data_object = args.output.with_suffix(args.output.suffix + ".dispatch-data.o")
            data_source.write_text(
                build_dispatch_data_source(metadata, functions), encoding="utf-8"
            )
            data_flags = ["-x", "c", "-O0"]
            if sys.platform in ("linux", "darwin"):
                data_flags.append("-fPIC")
            subprocess.run(
                [args.cxx, *data_flags, "-c", str(data_source), "-o", str(data_object)],
                check=True,
            )
            glue_objects = cached_runtime_objects(
                cache_base.with_name(cache_base.name + ".data-dispatch"),
                [source_root / "src/runtime/ee_dispatch_data_glue.cpp"],
                source_root, args.cxx, include_args, runtime_flags,
            )
            dispatch_objects = [data_object, *glue_objects]
        else:
            dispatch_objects = cached_dispatch_objects(
                cache_base, metadata, functions, args.cxx, source_root,
                include_args, compile_flags
            )

        runtime_objects = cached_runtime_objects(
            cache_base, required_sources, source_root, args.cxx,
            include_args, runtime_flags
        )

        # Use a response file: the complete game emits thousands of per-function
        # objects and can exceed the operating system's command-line length.
        response_path = args.output.with_suffix(args.output.suffix + ".objects.rsp")
        response_path.write_text(
            "\n".join('"' + str(path).replace("\\", "/").replace('"', '\\"') + '"' for path in objects)
            + "\n",
            encoding="utf-8",
        )

        command = [
            args.cxx,
            "-std=c++17",
            "-O2",
        ]
        if sys.platform == "linux":
            command += ["-shared", "-fPIC", "-Wl,--no-undefined"]
        elif sys.platform == "darwin":
            command += ["-dynamiclib", "-fPIC"]
        elif sys.platform == "win32":
            command += ["-shared"]
            if args.pdb:
                command += [
                    "-g", "-gcodeview", "-fuse-ld=lld",
                    "-Wl,--pdb=" + str(pdb_path).replace("\\", "/"),
                ]
        else:
            raise ValueError(f"unsupported host platform for shared-library linking: {sys.platform}")

        command += include_args
        command.append("@" + str(response_path))
        command += [str(path) for path in runtime_objects]
        command += [str(path) for path in dispatch_objects]
        command += ["-o", str(args.output)]

        if pdb_path is not None and pdb_path.exists():
            # Never accept a PDB left over from an earlier, unrelated DLL.
            pdb_path.unlink()

        print(
            f"link_native_objects: linking {len(objects)} guest objects "
            f"for {object_manifest['target_triple']} -> {args.output}",
            flush=True,
        )
        link_started = time.monotonic()
        subprocess.run(command, check=True)
        print(
            f"link_native_objects: guest DLL linked "
            f"({time.monotonic() - link_started:.2f}s)",
            flush=True,
        )
        if args.pdb and (not pdb_path.is_file() or pdb_path.stat().st_size == 0):
            raise ValueError(
                f"link completed without producing the requested PDB: {pdb_path}"
            )

        linked_data = args.output.read_bytes()
        link_manifest = {
            "complete": True,
            "target_triple": object_manifest["target_triple"],
            "function_count": len(functions),
            "object_count": len(objects),
            "library": args.output.name,
            "library_size_bytes": len(linked_data),
            "library_sha256": hashlib.sha256(linked_data).hexdigest(),
            "dispatcher_entry": "ee_game_run",
            "dispatch_glue": dispatch_source.name,
            "dispatch_object": dispatch_object.name,
            "dispatch_chunk_objects": [path.name for path in dispatch_objects[1:]],
            "dispatcher_optimization_level": args.dispatcher_opt,
            "dispatcher_entry_optimization_level": 2 if args.data_dispatch else max(2, args.dispatcher_opt),
            "pdb": pdb_path.name if pdb_path is not None else None,
            "pdb_size_bytes": pdb_path.stat().st_size if pdb_path is not None else None,
            "pdb_sha256": hashlib.sha256(pdb_path.read_bytes()).hexdigest()
                          if pdb_path is not None else None,
        }
        manifest_path = args.output.with_suffix(args.output.suffix + ".link.json")
        manifest_path.write_text(json.dumps(link_manifest, indent=2) + "\n", encoding="utf-8")
        print(
            f"link_native_objects: PASS ({len(functions)} functions, "
            f"{len(linked_data):,} library bytes)",
            flush=True,
        )
        return 0

    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print(f"link_native_objects: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
