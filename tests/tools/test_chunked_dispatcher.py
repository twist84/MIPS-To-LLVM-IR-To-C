#!/usr/bin/env python3
"""Native dispatcher chunks preserve ownership order and stable cache inputs."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "link_native_objects", ROOT / "tools" / "link_native_objects.py"
)
linker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linker)


def make_function(index):
    address = 0x100000 + index * 0x10
    return {
        "name": f"sub_{address:08x}",
        "host_symbol": f"guest_fn_{address:08x}",
        "vaddr": address,
        "block_pcs": [address, address + 4],
        "ir_path": f"sub_{address:08x}.ll",
    }


class ChunkDispatcherTests(unittest.TestCase):
    def test_256_function_boundary_preserves_cached_chunk(self):
        base = [make_function(i) for i in range(256)]
        expanded = base + [make_function(256)]
        initial = linker.build_dispatch_chunk_source(base, 0, 0)
        expanded_first = linker.build_dispatch_chunk_source(expanded[:256], 0, 0)
        self.assertEqual(initial, expanded_first)
        self.assertIn("entries[255] = &guest_fn_00100ff0;", initial)
        self.assertNotIn("guest_fn_00101000", initial)
        tail = linker.build_dispatch_chunk_source(expanded[256:], 256, 1)
        self.assertIn("entries[256] = &guest_fn_00101000;", tail)
        self.assertNotIn("entries[255]", tail)

        meta = {"entry_point": "0x00100008", "functions_discovered": 300}
        before = linker.build_chunked_dispatch_source(meta, base, 1)
        after = linker.build_chunked_dispatch_source(meta, expanded, 2)
        self.assertIn("compiled.function_count = 256u;", before)
        self.assertIn("compiled.function_count = 257u;", after)
        self.assertNotIn("ee_dispatch_chunk_0001(", before)
        self.assertIn("ee_dispatch_chunk_0001(", after)
        self.assertIn("entries.data()", after)
        self.assertIn("ee_game_resolve_entry(", after)
        self.assertIn("ee::ee_resolve_dispatch_entry(g_dispatch_table, g_dispatch.index, guest_pc)", after)
        self.assertIn("ee_dispatch_run_indexed(", after)
        self.assertIn("cpu, runtime, g_dispatch_table, g_dispatch.index,", after)
        # The O(log n) index is built once, after every chunk has registered.
        self.assertLess(after.index("ee_dispatch_chunk_0001(&compiled"),
                        after.index("index = ee::ee_build_dispatch_index(compiled);"))

    def test_wrapper_optimized_without_recompiling_metadata_chunks(self):
        with tempfile.TemporaryDirectory(prefix="ee-wrapper-flags-") as temp:
            root = Path(temp)
            compiler = root / "clang++"
            compiler.write_bytes(b"synthetic compiler")
            output = root / "game.dll"
            functions = [make_function(0)]
            metadata = {"entry_point": "0x00100000"}
            calls = []

            def compile_unit(command, **kwargs):
                calls.append(command)
                Path(command[command.index("-o") + 1]).write_bytes(b"object")

            with mock.patch.object(linker, "runtime_cache_key", side_effect=
                    lambda sources, source_root, compiler_info, flags: " ".join(flags)), \
                    mock.patch.object(linker.subprocess, "run", side_effect=compile_unit):
                linker.cached_dispatch_objects(
                    output, metadata, functions, str(compiler), root, [], ["-O0"])
                self.assertEqual(len(calls), 2)
                self.assertIn("-O2", calls[0])
                self.assertIn("-O0", calls[1])
                calls.clear()
                linker.cached_dispatch_objects(
                    output, metadata, functions, str(compiler), root, [], ["-O0"])
                self.assertEqual(calls, [])

    def test_chunk_ownership_uses_original_metadata_order(self):
        inputs = [make_function(i) for i in range(3)]
        first = linker.build_dispatch_chunk_source(inputs[:2], 0, 0)
        second = linker.build_dispatch_chunk_source(inputs[2:], 2, 1)
        self.assertIn("entries[0] = &guest_fn_00100000;", first)
        self.assertIn("entries[1] = &guest_fn_00100010;", first)
        self.assertIn("entries[2] = &guest_fn_00100020;", second)
        self.assertIn("compiled->functions.push_back", first)
        self.assertIn("compiled->functions.push_back", second)

    def test_windows_link_driver_discovery_and_override(self):
        # CMake may build EE tools using cl.exe; the generated native DLL
        # needs the GNU-style clang++ driver, not cl.exe or clang-cl.exe.
        with tempfile.TemporaryDirectory(prefix="native-clang-driver-") as tmp:
            program_files = Path(tmp) / "Program Files"
            llvm_exe = program_files / "LLVM" / "bin" / "clang++.exe"
            llvm_exe.parent.mkdir(parents=True)
            llvm_exe.write_bytes(b"test-only compiler fixture")

            with mock.patch.object(linker.shutil, "which", return_value=None):
                selected = linker.select_native_link_compiler(
                    platform="win32", environment={
                        "ProgramFiles": str(program_files),
                    }
                )
                self.assertEqual(selected, str(llvm_exe.resolve()))

                explicit = linker.select_native_link_compiler(
                    str(llvm_exe), platform="win32",
                    environment={"CXX": "cl.exe"},
                )
                self.assertEqual(explicit, str(llvm_exe.resolve()))

                for wrong_driver in ("cl", "cl.exe", "clang-cl.exe"):
                    with self.subTest(wrong_driver=wrong_driver):
                        with self.assertRaisesRegex(ValueError, "requires clang"):
                            linker.select_native_link_compiler(
                                platform="win32",
                                environment={"CXX": wrong_driver},
                            )

                # Isolate the negative case from a real LLVM installation.
                # The search always considers C:/Program Files/LLVM even
                # when a synthetic ProgramFiles environment is supplied.
                with mock.patch.object(Path, "is_file", return_value=False):
                    with self.assertRaisesRegex(
                        ValueError, "clang\\+\\+\\.exe not found"
                    ):
                        linker.select_native_link_compiler(
                            platform="win32",
                            environment={
                                "ProgramFiles": str(Path(tmp) / "missing")
                            },
                        )

    def test_non_windows_link_driver_keeps_override(self):
        self.assertEqual(
            linker.select_native_link_compiler(
                platform="linux", environment={"CXX": "/usr/bin/g++"}
            ),
            "/usr/bin/g++"
        )
        self.assertEqual(
            linker.select_native_link_compiler(
                "/opt/custom/c++", platform="linux",
                environment={"CXX": "/usr/bin/g++"},
            ),
            "/opt/custom/c++"
        )

    def test_data_dispatch_table_preserves_order_and_offsets(self):
        functions = [make_function(i) for i in range(3)]
        functions[1]["block_pcs"] = [0x100010, 0x100018, 0x10001c]
        source = linker.build_dispatch_data_source(
            {"entry_point": "0x00100008", "functions_discovered": 9}, functions
        )
        # Header-free C: no C++ includes to parse for each overlay library.
        self.assertNotIn("#include", source)
        self.assertIn("ee_dispatch_data_function_count = 3u;", source)
        self.assertIn("ee_dispatch_data_entry_point = 0x00100008u;", source)
        self.assertIn("ee_dispatch_data_functions_discovered = 9u;", source)
        # function_count + 1 offsets into the flattened block-PC array.
        self.assertIn("0x00000000u, 0x00000002u, 0x00000005u, 0x00000007u", source)
        entries = source.split("ee_dispatch_data_entries[]", 1)[1]
        self.assertLess(entries.index("guest_fn_00100000"),
                        entries.index("guest_fn_00100010"))
        self.assertLess(entries.index("guest_fn_00100010"),
                        entries.index("guest_fn_00100020"))
        with self.assertRaises(ValueError):
            linker.build_dispatch_data_source({"entry_point": "0x0"}, [])

    def test_cpp_escaping(self):
        item = make_function(0)
        item["name"] = "with \"quoted\" name"
        item["ir_path"] = "C:\\guest\\sub.ll"
        source = linker.build_dispatch_chunk_source([item], 0, 0)
        self.assertIn('with \\"quoted\\" name', source)
        self.assertIn('C:\\\\guest\\\\sub.ll', source)


if __name__ == "__main__":
    unittest.main()
