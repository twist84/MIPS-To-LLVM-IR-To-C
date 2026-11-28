Warning: truncated output (original token count: 16271)
Total output lines: 1297

cmake_minimum_required(VERSION 4.5.1)
project(ps2_recompiler LANGUAGES C CXX)

option(EE_MSVC_WARNINGS_AS_ERRORS "Treat project MSVC compiler warnings as errors" ON)
option(EE_CLANG_WARNINGS_AS_ERRORS "Treat project Clang compiler warnings as errors" ON)

option(EE_ENABLE_OPENPS2BIOS_SOURCE "Enable the patched OpenPS2BIOS source build target" OFF)

if(EE_ENABLE_OPENPS2BIOS_SOURCE)
    include(cmake/OpenPS2BIOS.cmake)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No project target uses C++ modules. Avoid unnecessary dependency scans.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Use the LLVM-provided clang/clang++ and lld-link when available. LLVM is kept
# optional: the core decoder/runtime build without LLVM (PLAN.md section 6).
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

if(EXISTS "/usr/bin/ld.lld" OR EXISTS "/usr/local/bin/ld.lld")
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_MODULE_LINKER_FLAGS "-fuse-ld=lld")
endif()

include_directories(include include/ee src/llvm src/lift src/runtime)

add_library(ee_core STATIC
    src/ee/decoder.cpp
    src/ee/opcode_tables.cpp
    src/ee/function_analyzer.cpp
    src/ee/recompiled_functions.cpp
    src/ee/ee_dispatcher.cpp
    src/elf/elf_loader.cpp
    src/llvm/llvm_context.cpp
    src/llvm/module_builder.cpp
    src/llvm/object_emitter.cpp
    src/lift/instruction_lifter.cpp
    src/lift/native_codegen.cpp
    src/runtime/ee_memory.cpp
    src/runtime/ee_ram_interpreter.cpp
    src/runtime/ee_strict_native_word.cpp
    src/runtime/ee_unaligned.cpp
    src/runtime/ee_intc.cpp
    src/runtime/ee_dmac.cpp
    src/runtime/ee_gif.cpp
    src/runtime/ee_vif1.cpp
    src/runtime/ipu/ee_ipu.cpp
    src/runtime/ee_vu1.cpp
    src/runtime/ee_vu1_worker.cpp
    src/runtime/ee_vu1_ir.cpp
    src/runtime/ee_vu1_aot.cpp
    src/runtime/ee_gs.cpp
    src/runtime/ee_gs_display.cpp
    src/runtime/ee_exceptions.cpp
    src/runtime/ee_vu0.cpp
    src/runtime/ee_vsync_flags.cpp
    src/runtime/ee_scanout_clock.cpp
    src/runtime/ee_cycle_scheduler.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(ee_core PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
    target_link_libraries(ee_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# Standalone IOP R3000A interpreter, initially isolated from the EE
# runner until module loading, timing and SIF transport are integrated.
add_library(iop_cpu STATIC
    src/iop/iop_cpu.cpp
    src/iop/iop_guest_fault_probe.cpp
    src/iop/iop_hw.cpp
    src/iop/iop_memory_card.cpp
    src/iop/iop_spu2.cpp
    src/iop/irx_loader.cpp
    src/iop/iop_aot.cpp
    src/iop/iop_kernel.cpp
    src/iop/iop_kernel_hle_bridge.cpp
    src/iop/iop_sysclib_strings.cpp
    src/iop/iop_sifman_bridge.cpp
    src/iop/iop_sif1_bridge.cpp
    src/iop/iop_sif1_dma_transport.cpp
    src/iop/iop_sif0_dma_transport.cpp
    src/iop/iop_sifcmd_bridge.cpp
    src/iop/iop_disc_loader.cpp
    src/iop/iop_cdvd.cpp
    src/iop/iop_folder_sector_source.cpp
    src/iop/iop_cdvdman_bridge.cpp
    src/iop/iop_sif_rpc_bridge.cpp
)
target_include_directories(iop_cpu PUBLIC ${CMAKE_SOURCE_DIR}/src/iop)
add_executable(iop_cpu_test tests/smoke/iop_cpu_test.cpp)
target_link_libraries(iop_cpu_test PRIVATE iop_cpu)
add_executable(iop_guest_fault_probe_test
    tests/smoke/iop_guest_fault_probe_test.cpp)
target_link_libraries(iop_guest_fault_probe_test PRIVATE iop_cpu)
add_executable(iop_rom0_boot_test tests/smoke/iop_rom0_boot_test.cpp)
target_link_libraries(iop_rom0_boot_test PRIVATE iop_cpu)
add_executable(iop_hw_test tests/smoke/iop_hw_test.cpp)
target_link_libraries(iop_hw_test PRIVATE iop_cpu)
add_executable(iop_timer5_exception_test tests/smoke/iop_timer5_exception_test.cpp)
target_link_libraries(iop_timer5_exception_test PRIVATE iop_cpu)
add_executable(iop_dma_icr_test tests/smoke/iop_dma_icr_test.cpp)
target_link_libraries(iop_dma_icr_test PRIVATE iop_cpu)
add_executable(iop_dma_global_enable_test tests/smoke/iop_dma_global_enable_test.cpp)
target_link_libraries(iop_dma_global_enable_test PRIVATE iop_cpu)
add_executable(iop_dma_sif_channels_test tests/smoke/iop_dma_sif_channels_test.cpp)
target_link_libraries(iop_dma_sif_channels_test PRIVATE iop_cpu)
add_executable(sif1_dma_registers_test tests/smoke/sif1_dma_registers_test.cpp)
target_link_libraries(sif1_dma_registers_test PRIVATE iop_cpu ee_core)
add_executable(sif1_ee_to_iop_test tests/smoke/sif1_ee_to_iop_test.cpp)
target_link_libraries(sif1_ee_to_iop_test PRIVATE iop_cpu ee_core)
add_executable(sif1_ee_chain_test tests/smoke/sif1_ee_chain_test.cpp)
target_link_libraries(sif1_ee_chain_test PRIVATE iop_cpu ee_core)
add_executable(sif0_iop_to_ee_test tests/smoke/sif0_iop_to_ee_test.cpp)
target_link_libraries(sif0_iop_to_ee_test PRIVATE iop_cpu ee_core)
add_executable(sif_dma_roundtrip_test tests/smoke/sif_dma_roundtrip_test.cpp)
target_link_libraries(sif_dma_roundtrip_test PRIVATE iop_cpu ee_core)
add_executable(sif0_dma_invalid_tags_test tests/smoke/sif0_dma_invalid_tags_test.cpp)
target_link_libraries(sif0_dma_invalid_tags_test PRIVATE iop_cpu ee_core)
add_executable(sif1_dma_invalid_tags_test tests/smoke/sif1_dma_invalid_tags_test.cpp)
target_link_libraries(sif1_dma_invalid_tags_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_handoff_test tests/smoke/iop_sif_handoff_test.cpp)
target_link_libraries(iop_sif_handoff_test PRIVATE iop_cpu)
add_executable(iop_sif_mmio_link_test tests/smoke/iop_sif_mmio_link_test.cpp)
target_link_libraries(iop_sif_mmio_link_test PRIVATE iop_cpu ee_core)
add_executable(iop_rom0_runner src/tools/iop_rom0_runner_main.cpp)
target_link_libraries(iop_rom0_runner PRIVATE iop_cpu)
add_executable(iop_rom0_sif_integration src/tools/iop_rom0_sif_integration_main.cpp)
target_link_libraries(iop_rom0_sif_integration PRIVATE iop_cpu ee_core)
add_executable(iop_irx_loader_test tests/smoke/iop_irx_loader_test.cpp)
target_link_libraries(iop_irx_loader_test PRIVATE iop_cpu)
add_executable(iop_irx_export_link_test tests/smoke/iop_irx_export_link_test.cpp)
target_link_libraries(iop_irx_export_link_test PRIVATE iop_cpu)
add_executable(iop_sysclib_memory_test tests/smoke/iop_sysclib_memory_test.cpp)
target_link_libraries(iop_sysclib_memory_test PRIVATE iop_cpu)
add_executable(iop_sysclib_string_test tests/smoke/iop_sysclib_string_test.cpp)
target_link_libraries(iop_sysclib_string_test PRIVATE iop_cpu)
add_executable(iop_sysclib_ctype_test tests/smoke/iop_sysclib_ctype_test.cpp)
target_link_libraries(iop_sysclib_ctype_test PRIVATE iop_cpu)
add_executable(iop_kernel_hle_bridge_test tests/smoke/iop_kernel_hle_bridge_test.cpp)
target_link_libraries(iop_kernel_hle_bridge_test PRIVATE iop_cpu)
add_executable(iop_scheduler_cache_test tests/smoke/iop_scheduler_cache_test.cpp)
target_link_libraries(iop_scheduler_cache_test PRIVATE iop_cpu)
add_executable(iop_kernel_test tests/smoke/iop_kernel_test.cpp)
target_link_libraries(iop_kernel_test PRIVATE iop_cpu)
add_executable(iop_sifman_bridge_test tests/smoke/iop_sifman_bridge_test.cpp)
target_link_libraries(iop_sifman_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_dma_test tests/smoke/iop_sif_dma_test.cpp)
target_link_libraries(iop_sif_dma_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif1_bridge_test tests/smoke/iop_sif1_bridge_test.cpp)
target_link_libraries(iop_sif1_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sifcmd_bridge_test tests/smoke/iop_sifcmd_bridge_test.cpp)
target_link_libraries(iop_sifcmd_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_disc_loader_test tests/smoke/iop_disc_loader_test.cpp)
target_link_libraries(iop_disc_loader_test PRIVATE iop_cpu)
add_executable(iop_cdvd_test tests/smoke/iop_cdvd_test.cpp)
target_link_libraries(iop_cdvd_test PRIVATE iop_cpu)
add_executable(iop_folder_sector_source_test tests/smoke/iop_folder_sector_source_test.cpp)
target_link_libraries(iop_folder_sector_source_test PRIVATE iop_cpu)
add_executable(iop_cdvdman_bridge_test tests/smoke/iop_cdvdman_bridge_test.cpp)
target_link_libraries(iop_cdvdman_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_rpc_bridge_test tests/smoke/iop_sif_rpc_bridge_test.cpp)
target_link_libraries(iop_sif_rpc_bridge_test PRIVATE iop_cpu ee_core)

add_executable(ee_smoke_test
    tests/smoke/smoke_test.cpp
    tests/smoke/native_smoke.cpp
)
target_link_libraries(ee_smoke_test PRIVATE ee_core)

# Build a native object for the reference/interpreter smoke harness. This is
# deliberately not described as LLVM IR object emission: the actual LLVM text
# path is verified separately by tools/verify_ir.py when llvmlite is available.
add_library(ee_native_smoke_object OBJECT EXCLUDE_FROM_ALL
    tests/smoke/native_smoke.cpp)
# Use the configured C++ language mode and compiler driver for this legacy
# object too, instead of a separate GNU-only compiler command.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/recompiled"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_OBJECTS:ee_native_smoke_object>
        "${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj"
    DEPENDS ee_native_smoke_object $<TARGET_OBJECTS:ee_native_smoke_object>
    COMMENT "Emitting native object for lifted guest function"
    VERBATIM
)
add_custom_target(ee_emit_obj DEPENDS
    ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
)

add_executable(ee_multi_test
    tests/smoke/multi_function_test.cpp
)
target_link_libraries(ee_multi_test PRIVATE ee_core)

# Function entry discovery, including conservative CFG constant propagation
# for static JALR call targets.
add_executable(ee_function_discovery_test
    tests/smoke/function_discovery_test.cpp
)
target_link_libraries(ee_function_discovery_test PRIVATE …11271 tokens truncated…AME ee_checked_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_ee_checked_ir_runtime.py
                $<TARGET_FILE:ee_checked_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME compare_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_compare_ir_runtime.py
                $<TARGET_FILE:ee_compare_ir_fixture>)
    add_test(NAME scalar_specialize_ir_runtime
                COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_scalar_specialize_ir_runtime.py
                $<TARGET_FILE:ee_scalar_specialize_ir_fixture>)
    add_test(NAME ee_source_lowering_comparison
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_compare_ee_source_lowering.py)
    add_test(NAME ee_packed_native_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_audit_ee_packed_native.py)
    add_test(NAME cop0_write_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_cop0_write_ir_runtime.py
                $<TARGET_FILE:ee_cop0_write_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME object_emission_and_dispatch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_object_emission.py
                $<TARGET_FILE:ee_compare_ir_fixture>
                ${CMAKE_SOURCE_DIR}/tools/verify_ir.py
                ${CMAKE_SOURCE_DIR}/tools/link_native_objects.py
                ${CMAKE_SOURCE_DIR})
    add_test(NAME ee_timing_observer_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_ee_timing_observer_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    # Independent synthetic ELF scenarios run in four parallel CI jobs;
    # omitting the optional argument still executes the complete suite.
    foreach(game_shard RANGE 0 3)
        add_test(NAME game_runner_end_to_end_${game_shard}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/tools/test_game_runner.py
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_BINARY_DIR}
                    $<TARGET_FILE:ee_game_runner>
                    ${game_shard})
    endforeach()
    add_test(NAME ps2autotests_harness
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ps2autotests_harness.py)
    add_test(NAME ee_native_thread_scheduler
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_thread_scheduler.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-thread-scheduler-test)
    set_tests_properties(ee_native_thread_scheduler PROPERTIES TIMEOUT 180)
    add_test(NAME ee_r5900_mult_gpr
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_mult_gpr.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-mult-gpr-test)
    set_tests_properties(ee_r5900_mult_gpr PROPERTIES TIMEOUT 180)
    add_test(NAME ee_sif0_stop_dma
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_sif0_stop_dma.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-sif0-stop-test)
    set_tests_properties(ee_sif0_stop_dma PROPERTIES TIMEOUT 180)
    add_test(NAME ee_memory_size_hle
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_memory_size.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-memory-size-test)
    set_tests_properties(ee_memory_size_hle PROPERTIES TIMEOUT 180)

    # Native Python integration tests select their own compatible linker:
    # Windows LLVM clang++ accepts the generated GNU-style driver flags,
    # while the configured CMake compiler may be MSVC cl.exe. Never force
    # CMAKE_CXX_COMPILER into the test environment, since it can override
    # an explicit caller CXX and make link_native_objects.py invoke cl.exe.
    # See select_native_link_compiler() in tools/link_native_objects.py.

    add_test(NAME control_flow_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_control_flow_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME guest_instruction_attempts_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_guest_instruction_attempts_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME guest_loop_yield_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_guest_loop_yield_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME ee_trace_helper_optimizer
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_trace_helper_optimizer.py
                $<TARGET_FILE:ee_mmi_ir_fixture>)
    add_test(NAME mmi_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_ir_runtime.py
                $<TARGET_FILE:ee_mmi_ir_fixture>)
    add_test(NAME mmi0_extended_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi0_extended_ir_runtime.py
                $<TARGET_FILE:ee_mmi0_extended_ir_fixture>)
    add_test(NAME mmi_hilo_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_hilo_ir_runtime.py
                $<TARGET_FILE:ee_mmi_hilo_ir_fixture>)
    add_test(NAME mmi_var_shift_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_var_shift_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
    add_test(NAME mmi_psravw_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_var_shift_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture> psravw)
    add_test(NAME mmi_interleave_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_interleave_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
    add_test(NAME mmi2_permute_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_permute_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture> mmi2)
    add_test(NAME mmi3_permute_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_permute_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture> mmi3)
    add_test(NAME mmi_divbw_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_divbw_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
    add_test(NAME mmi_halfword_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_halfword_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
    add_test(NAME mmi_mac_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_mac_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
    add_test(NAME mmi_word_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_word_ir_runtime.py
                $<TARGET_FILE:ee_mmi_word_ir_fixture>)
else()
    message(STATUS "llvmlite not found: omitting mmi_ir_runtime CTest")
endif()
if(EE_HAVE_LLVM_JIT_TEST)
    add_test(NAME div1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_div1_ir_runtime.py
                $<TARGET_FILE:ee_div1_ir_fixture>)
    add_test(NAME mmi1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi1_ir_runtime.py
                $<TARGET_FILE:ee_mmi1_ir_fixture>)
endif()
set_tests_properties(multi_function smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Apply warning policy only to project targets, not FetchContent raylib/GLFW.
get_property(EE_PROJECT_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS EE_PROJECT_TARGETS)
    if(target STREQUAL "ee_raudio")
        continue()
    endif()
    get_target_property(target_type ${target} TYPE)
    if(NOT target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
        continue()
    endif()
    # Standard C/C++ CRT APIs are intentionally portable. The Windows UCRT
    # annotates them as deprecated in favor of Microsoft-specific variants.
    # This opts into standard CRT usage, not general deprecation suppression.
    if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/W3>")
        if(EE_MSVC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/WX>")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" AND EE_CLANG_WARNINGS_AS_ERRORS)
        # clang-cl requires the MSVC driver spelling; clang++ uses -Werror.
        if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/WX>")
        else()
            target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:-Werror>")
        endif()
    endif()
endforeach()
