#!/bin/sh
# Portable build without CMake/clang (g++ only). Usage: ./scripts/build.sh
set -e
# Resolve the checkout root independently of the caller's working directory.
cd "$(dirname "$0")/.."
SRCS="src/ee/decoder.cpp src/ee/opcode_tables.cpp src/ee/function_analyzer.cpp src/elf/elf_loader.cpp src/llvm/llvm_context.cpp src/llvm/module_builder.cpp src/llvm/object_emitter.cpp src/lift/instruction_lifter.cpp src/lift/native_codegen.cpp src/runtime/ee_memory.cpp src/runtime/ee_exceptions.cpp"
INC="-Iinclude -Iinclude/ee -Isrc/llvm -Isrc/lift -Isrc/runtime"
CXX=${CXX:-g++}
mkdir -p build/recompiled
$CXX -std=c++17 -O1 $INC -o build/ee_smoke_test tests/smoke/smoke_test.cpp tests/smoke/native_smoke.cpp $SRCS
$CXX -std=c++17 -O1 $INC -o build/ee_recomp src/tools/ee_recomp_main.cpp src/ee/ee_recomp.cpp $SRCS
$CXX -std=c++17 -O1 -Wall -Wextra -Iinclude/ee -o build/ee_decoder_test tests/decoder/decoder_test.cpp src/ee/decoder.cpp src/lift/instruction_lifter.cpp
