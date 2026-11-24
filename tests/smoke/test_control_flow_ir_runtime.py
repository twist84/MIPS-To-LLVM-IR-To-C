#!/usr/bin/env python3
"""JIT regressions for indirect/direct transfers and link/delay-slot ordering."""

import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm

from ee_diagnostic_stubs import register_ee_diagnostic_stubs


class U128(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", U128 * 32),
        ("hi", ctypes.c_uint64),
        ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64),
        ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
    ]


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    state = ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents
    return backing, state


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(fixture_exe, temp_dir, target, retained, mode, function_name, setup, verify, expected_exit=3):
    print(f"control-flow case: {mode}", flush=True)
    target_machine = target.create_target_machine(opt=0)
    retained.append(target_machine)
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture_exe), mode, str(ir_path)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    retained.extend((engine, module))
    print(f"{mode}: JIT finalized", flush=True)

    address = engine.get_function_address(function_name)
    require(address != 0, f"{mode}: JIT did not expose {function_name}")

    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(address)

    backing, state = make_aligned_state()
    _ = backing
    setup(state)

    print(f"{mode}: entering JIT function", flush=True)
    result = guest_fn(ctypes.pointer(state), None)
    print(f"{mode}: JIT returned {result}", flush=True)
    require(result == expected_exit,
            f"{mode}: expected exit reason {expected_exit}, got {result}")
    require(state.exit_reason == expected_exit,
            f"{mode}: state exit reason {state.exit_reason}, expected {expected_exit}")
    verify(state)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_flow_ir_runtime.py fixture_executable")

    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    register_ee_diagnostic_stubs()
    target = llvm.Target.from_default_triple()
    retained = []

    with tempfile.TemporaryDirectory(prefix="ee-control-flow-") as temp:
        temp_dir = pathlib.Path(temp)

        def check_beq_taken(state):
            state.gpr[2].lo = 0
            state.gpr[2].hi = 0x2222333344445555

        def verify_beq_taken(state):
            require(state.pc == 0x00100014,
                    f"beq taken: delay slot changed sampled condition: PC=0x{state.pc:08x}")
            require(state.gpr[2].lo == 1,
                    f"beq taken: delay-slot ADDIU did not execute: {state.gpr[2].lo}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "beq taken: delay slot clobbered upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "beq_taken",
                 "guest_beq_taken_delay_test", check_beq_taken, verify_beq_taken)

        def check_beq_not_taken(state):
            state.gpr[2].lo = 1
            state.gpr[2].hi = 0x2222333344445555

        def verify_beq_not_taken(state):
            require(state.pc == 0x00100008,
                    f"beq not-taken: delay slot changed sampled condition: PC=0x{state.pc:08x}")
            require(state.gpr[2].lo == 0,
                    f"beq not-taken: delay-slot ADDIU did not execute: {state.gpr[2].lo}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "beq not-taken: delay slot clobbered upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "beq_not_taken",
                 "guest_beq_not_taken_delay_test", check_beq_not_taken, verify_beq_not_taken)

        def check_jr(state):
            state.gpr[2].lo = 0x00200000
            state.gpr[2].hi = 0x2222333344445555

        def verify_jr(state):
            require(state.pc == 0x00200000,
                    f"jr: delay slot changed captured target: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0x1234,
                    f"jr: delay-slot ADDIU did not execute: 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "jr: delay slot clobbered the upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "jr",
                 "guest_jr_delay_test", check_jr, verify_jr)

        def check_jrra(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[31].hi = 0x3131313131313131

        def verify_jrra(state):
            require(state.pc == 0x00300000,
                    f"jr $ra: delay slot changed captured target: 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x1234,
                    "jr $ra: delay-slot ADDIU did not execute")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "jr $ra: delay slot clobbered the upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "jrra",
                 "guest_jr_return_test", check_jrra, verify_jrra, expected_exit=0)

        def check_jr_daddu(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[19].lo = 0x004581e0
            state.gpr[19].hi = 0x1919191919191919
            state.gpr[2].lo = 1
            state.gpr[2].hi = 0x2222333344445555

        def verify_jr_daddu(state):
            require(state.pc == 0x00300000,
                    f"DADDU return: wrong continuation 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0x004581e0,
                    f"DADDU return: expected pointer, got 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "DADDU return: upper V0 register half was overwritten")

        run_case(fixture_exe, temp_dir, target, retained, "jr_daddu",
                 "guest_jr_daddu_return_test", check_jr_daddu, verify_jr_daddu,
                 expected_exit=0)

        def check_branch_liveness(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[2].lo = 0  # stale state must not drive BEQL
            state.gpr[2].hi = 0x2222333344445555
            state.gpr[19].lo = 0x004581e0
            state.gpr[19].hi = 0x1919191919191919

        def verify_branch_liveness(state):
            require(state.pc == 0x00300000,
                    f"cross-block return: wrong PC 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0x004581e0,
                    f"cross-block DADDU value lost: 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "cross-block DADDU clobbered upper GPR half")
            require(state.gpr[17].lo == 0x66,
                    f"BEQL used stale V0 / wrong route: s1=0x{state.gpr[17].lo:x}")

        run_case(fixture_exe, temp_dir, target, retained, "branch_liveness",
                 "guest_branch_liveness_test", check_branch_liveness,
                 verify_branch_liveness, expected_exit=0)

        def check_branch_likely_kill(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[3].lo = 1  # BEQL not taken: its delay slot is annulled
            state.gpr[16].lo = 0  # stale S0 must not survive
            state.gpr[19].lo = 0x004581e0

        def verify_branch_likely_kill(state):
            require(state.pc == 0x00300000,
                    f"branch-likely kill: wrong PC 0x{state.pc:08x}")
            require(state.gpr[17].lo == 0x66,
                    f"BEQL took the wrong route: s1=0x{state.gpr[17].lo:x}")
            require(state.gpr[16].lo == 0x004581e0,
                    "value killed only by an annulled BEQL slot was lost: "
                    f"s0=0x{state.gpr[16].lo:x}")

        run_case(fixture_exe, temp_dir, target, retained, "branch_likely_kill",
                 "guest_branch_likely_kill_test", check_branch_likely_kill,
                 verify_branch_likely_kill, expected_exit=0)

        def check_mov_liveness(state, condition):
            state.gpr[31].lo = 0x00300000
            state.gpr[8].lo = 0x004581e0
            state.gpr[9].lo = condition
            state.gpr[19].lo = 0  # stale S3 must not reach the next block
            state.gpr[2].lo = 1
            state.gpr[2].hi = 0x2222333344445555

        def verify_mov_liveness(state):
            require(state.pc == 0x00300000,
                    f"conditional move: wrong PC 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0x004581e0,
                    f"conditional move saw stale source 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "conditional move clobbered upper register half")
            require(state.gpr[18].lo == 0x55,
                    "conditional move JR delay slot did not execute")

        run_case(fixture_exe, temp_dir, target, retained, "movz_liveness",
                 "guest_movz_liveness_test",
                 lambda state: check_mov_liveness(state, 0),
                 verify_mov_liveness, expected_exit=0)
        run_case(fixture_exe, temp_dir, target, retained, "movn_liveness",
                 "guest_movn_liveness_test",
                 lambda state: check_mov_liveness(state, 1),
                 verify_mov_liveness, expected_exit=0)

        def check_srav_liveness(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[8].lo = 1
            state.gpr[17].lo = 0x80000000
            state.gpr[19].lo = 0  # stale S3 would shift by zero
            state.gpr[2].lo = 0
            state.gpr[2].hi = 0x2222333344445555

        def verify_srav_liveness(state):
            require(state.pc == 0x00300000,
                    f"SRAV: wrong PC 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0xffffffffc0000000,
                    f"SRAV cross-block shift count lost: 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "SRAV overwrote upper register half")

        run_case(fixture_exe, temp_dir, target, retained, "srav_liveness",
                 "guest_srav_liveness_test", check_srav_liveness,
                 verify_srav_liveness, expected_exit=0)

        def check_jal(state):
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0xAAAAAAAA

        def verify_jal(state):
            require(state.pc == 0x00102000,
                    f"jal: wrong dispatched target: 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"jal: wrong link value: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "jal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    f"jal: delay slot did not observe PC+8: 0x{state.gpr[8].lo:x}")

        run_case(fixture_exe, temp_dir, target, retained, "jal",
                 "guest_jal_delay_test", check_jal, verify_jal)

        def check_jalr(state):
            state.gpr[8].lo = 0x00200000
            state.gpr[8].hi = 0x8888888888888888
            state.gpr[9].lo = 0xAAAAAAAA

        def verify_jalr(state):
            require(state.pc == 0x00200000,
                    f"jalr: target was not captured before rd/rs alias write: 0x{state.pc:08x}")
            require(state.gpr[8].lo == 0x00100008,
                    f"jalr: rd did not receive PC+8: 0x{state.gpr[8].lo:x}")
            require(state.gpr[8].hi == 0x8888888888888888,
                    "jalr: link write clobbered upper GPR half")
            require(state.gpr[9].lo == 0x00100009,
                    f"jalr: delay slot did not observe new link: 0x{state.gpr[9].lo:x}")

        run_case(fixture_exe, temp_dir, target, retained, "jalr",
                 "guest_jalr_delay_test", check_jalr, verify_jalr)

        def check_jalr_rd(state):
            state.gpr[8].lo = 0x00210000
            state.gpr[16].lo = 0xAAAAAAAA
            state.gpr[16].hi = 0x1616161616161616
            state.gpr[9].lo = 0xBBBBBBBB
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131

        def verify_jalr_rd(state):
            require(state.pc == 0x00210000,
                    "jalr rd: dynamic target was not captured from rs")
            require(state.gpr[16].lo == 0x00100008,
                    f"jalr rd: encoded rd did not receive PC+8: 0x{state.gpr[16].lo:x}")
            require(state.gpr[16].hi == 0x1616161616161616,
                    "jalr rd: link write clobbered upper GPR half")
            require(state.gpr[9].lo == 0x00100009,
                    "jalr rd: delay slot did not read the encoded link register")
            require(state.gpr[31].lo == 0xDEADBEEF and
                    state.gpr[31].hi == 0x3131313131313131,
                    "jalr rd: non-$ra destination incorrectly modified $ra")

        run_case(fixture_exe, temp_dir, target, retained, "jalr_rd",
                 "guest_jalr_rd_delay_test", check_jalr_rd, verify_jalr_rd)

        def check_beq_taken(state):
            state.gpr[2].lo = 0

        def verify_beq_taken(state):
            require(state.pc == 0x00100014,
                    f"beq_taken: delay slot changed captured outcome: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 1,
                    "beq_taken: delay-slot ADDIU did not execute")

        run_case(fixture_exe, temp_dir, target, retained, "beq_taken",
                 "guest_beq_taken_delay_test", check_beq_taken, verify_beq_taken)

        def check_beq_not_taken(state):
            state.gpr[2].lo = 1

        def verify_beq_not_taken(state):
            require(state.pc == 0x00100008,
                    f"beq_not_taken: delay slot changed captured outcome: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0,
                    "beq_not_taken: delay-slot ADDIU did not execute")

        run_case(fixture_exe, temp_dir, target, retained, "beq_not_taken",
                 "guest_beq_not_taken_delay_test", check_beq_not_taken,
                 verify_beq_not_taken)

        def check_link(state, taken):
            state.gpr[2].lo = 0xFFFFFFFFFFFFFFFF if taken else 1
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0x55555555

        def verify_bltzal(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bltzal: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"bltzal: link must be written on either outcome: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bltzal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    "bltzal: ordinary delay slot did not execute/read link")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_link(state, taken)
            def verify(state, taken=taken):
                verify_bltzal(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bltzal",
                     "guest_bltzal_delay_test", setup, verify)

        def verify_bltzall(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bltzall: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"bltzall: link must be written on either outcome: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bltzall: link write clobbered upper GPR half")
            expected_t0 = 0x00100008 if taken else 0x55555555
            require(state.gpr[8].lo == expected_t0,
                    f"bltzall: delay-slot annulment mismatch: 0x{state.gpr[8].lo:x}")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_link(state, taken)
            def verify(state, taken=taken):
                verify_bltzall(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bltzall",
                     "guest_bltzall_delay_test", setup, verify)

        def check_bgez_link(state, taken):
            state.gpr[2].lo = 1 if taken else 0xFFFFFFFFFFFFFFFF
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0x55555555

        def verify_bgezal(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bgezal: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    "bgezal: link must be written on either outcome")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bgezal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    "bgezal: ordinary delay slot did not execute/read link")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_bgez_link(state, taken)
            def verify(state, taken=taken):
                verify_bgezal(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bgezal",
                     "guest_bgezal_delay_test", setup, verify)

        def verify_bgezall(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bgezall: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    "bgezall: link must be written on either outcome")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bgezall: link write clobbered upper GPR half")
            expected_t0 = 0x00100008 if taken else 0x55555555
            require(state.gpr[8].lo == expected_t0,
                    "bgezall: delay-slot annulment mismatch")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_bgez_link(state, taken)
            def verify(state, taken=taken):
                verify_bgezall(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bgezall",
                     "guest_bgezall_delay_test", setup, verify)


        def load_function(mode, function_name):
            target_machine = target.create_target_machine(opt=0)
            retained.append(target_machine)
            ir_path = temp_dir / f"{mode}_reentry.ll"
            subprocess.run([str(fixture_exe), mode, str(ir_path)], check=True)
            module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
            module.verify()
            module.triple = llvm.get_default_triple()
            module.data_layout = str(target_machine.target_data)
            engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
            engine.add_module(module)
            engine.finalize_object()
            retained.extend((engine, module))
            address = engine.get_function_address(function_name)
            require(address != 0, f"{mode}: JIT did not expose {function_name}")
            return ctypes.CFUNCTYPE(
                ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
            )(address)
    
        # Exercise the full call/return/continuation contract with two separately
        # compiled guest functions. JAL exits to the callee; JR $ra returns to the
        # caller's PC+8; the caller is re-entered and must dispatch to its second
        # LLVM basic block rather than restarting at its function entry.
        caller_fn = load_function("caller_resume", "guest_caller_resume_test")
        callee_fn = load_function("callee_return", "guest_callee_return_test")
        backing, state = make_aligned_state()
        _ = backing
        state.gpr[16].hi = 0x1616161616161616
        state.gpr[17].hi = 0x1717171717171717
        state.gpr[8].hi = 0x0808080808080808
        state.gpr[31].hi = 0x3131313131313131
    
        caller_exit = caller_fn(ctypes.pointer(state), None)
        require(caller_exit == 3 and state.exit_reason == 3,
                f"caller JAL: expected dispatch exit 3, got {caller_exit}/{state.exit_reason}")
        require(state.pc == 0x00102000,
                f"caller JAL: expected callee PC 0x00102000, got 0x{state.pc:08x}")
        require(state.gpr[31].lo == 0x00100008,
                f"caller JAL: wrong return address 0x{state.gpr[31].lo:x}")
        require(state.gpr[16].lo == 0x55 and state.gpr[16].hi == 0x1616161616161616,
                "caller JAL: delay-slot side effect or upper GPR preservation failed")
    
        callee_exit = callee_fn(ctypes.pointer(state), None)
        require(callee_exit == 0 and state.exit_reason == 0,
                f"callee JR $ra: expected return exit 0, got {callee_exit}/{state.exit_reason}")
        require(state.pc == 0x00100008,
                f"callee JR $ra: expected caller continuation 0x00100008, got 0x{state.pc:08x}")
        require(state.gpr[8].lo == 0x77 and state.gpr[8].hi == 0x0808080808080808,
                "callee JR $ra: delay-slot side effect or upper GPR preservation failed")
    
        resume_exit = caller_fn(ctypes.pointer(state), None)
        require(resume_exit == 3 and state.exit_reason == 3,
                f"caller continuation: expected dispatch exit 3, got {resume_exit}/{state.exit_reason}")
        require(state.pc == 0x0010000C,
                f"caller continuation: expected next PC 0x0010000c, got 0x{state.pc:08x}")
        require(state.gpr[17].lo == 0x66 and state.gpr[17].hi == 0x1717171717171717,
                "caller continuation block was not resumed correctly")
        # An unknown guest PC must return to the host dispatcher rather
        # than silently restarting this translated function.
        state.pc = 0xDEAD0000
        unknown_exit = caller_fn(ctypes.pointer(state), None)
        require(unknown_exit == 3 and state.exit_reason == 3,
                f"unknown caller PC: expected dispatcher exit 3, got {unknown_exit}/{state.exit_reason}")
        require(state.pc == 0xDEAD0000,
                f"unknown caller PC was overwritten: 0x{state.pc:08x}")
        require(state.gpr[16].lo == 0x55 and state.gpr[17].lo == 0x66,
                "unknown caller PC unexpectedly restarted the function")

        # Cross-function dynamic JALR: target $t0 points at the separately
        # translated callee, while the delay slot overwrites $t0 with the new
        # link value. The dispatcher must use the target captured before slot
        # execution, then resume this caller at PC+8 after JR $ra.
        caller_jalr_fn = load_function("caller_jalr", "guest_caller_jalr_test")
        jalr_backing, jalr_state = make_aligned_state()
        _ = jalr_backing
        jalr_state.pc = 0x00100000
        jalr_state.gpr[8].lo = 0x00102000
        jalr_state.gpr[8].hi = 0x8888888888888888
        jalr_state.gpr[17].hi = 0x1717171717171717
        jalr_state.gpr[31].lo = 0xDEADBEEF
        jalr_state.gpr[31].hi = 0x3131313131313131

        jalr_exit = caller_jalr_fn(ctypes.pointer(jalr_state), None)
        require(jalr_exit == 3 and jalr_state.exit_reason == 3,
                f"caller JALR: expected dispatch exit 3, got {jalr_exit}/{jalr_state.exit_reason}")
        require(jalr_state.pc == 0x00102000,
                f"caller JALR: target was not captured before delay slot: 0x{jalr_state.pc:08x}")
        require(jalr_state.gpr[31].lo == 0x00100008,
                f"caller JALR: wrong link value 0x{jalr_state.gpr[31].lo:x}")
        require(jalr_state.gpr[8].lo == 0x00100008,
                f"caller JALR: delay slot did not overwrite rs with link: 0x{jalr_state.gpr[8].lo:x}")
        require(jalr_state.gpr[8].hi == 0x8888888888888888 and
                jalr_state.gpr[31].hi == 0x3131313131313131,
                "caller JALR: link/delay slot clobbered an upper GPR half")

        jalr_callee_exit = callee_fn(ctypes.pointer(jalr_state), None)
        require(jalr_callee_exit == 0 and jalr_state.exit_reason == 0,
                f"JALR callee: expected return exit 0, got {jalr_callee_exit}/{jalr_state.exit_reason}")
        require(jalr_state.pc == 0x00100008,
                f"JALR callee: wrong caller continuation 0x{jalr_state.pc:08x}")
        require(jalr_state.gpr[8].lo == 0x77,
                "JALR callee: return delay slot did not execute")

        jalr_resume_exit = caller_jalr_fn(ctypes.pointer(jalr_state), None)
        require(jalr_resume_exit == 3 and jalr_state.exit_reason == 3,
                f"JALR caller resume: expected dispatch exit 3, got {jalr_resume_exit}/{jalr_state.exit_reason}")
        require(jalr_state.pc == 0x0010000C,
                f"JALR caller resume: wrong next PC 0x{jalr_state.pc:08x}")
        require(jalr_state.gpr[17].lo == 0x66 and
                jalr_state.gpr[17].hi == 0x1717171717171717,
                "JALR caller continuation did not execute or clobbered upper GPR half")
        print("control-flow cross-function JALR dispatch/resume: PASS", flush=True)

        # Execute one recursive JAL activation through the same host
        # dispatcher. The guest saves the outer $ra in $k0, then the inner
        # invocation returns to its caller continuation before the outer
        # invocation returns to the original host address.
        recursive_fn = load_function("recursive", "guest_recursive_call_test")
        recursive_backing, recursive_state = make_aligned_state()
        _ = recursive_backing
        recursive_state.pc = 0x00100000
        recursive_state.gpr[2].hi = 0x0202020202020202
        recursive_state.gpr[31].lo = 0xDEAD0000
        recursive_state.gpr[31].hi = 0x3131313131313131
        recursive_state.gpr[26].hi = 0x2626262626262626
        recursive_state.gpr[8].hi = 0x0808080808080808
        recursive_state.gpr[9].hi = 0x0909090909090909
        recursive_state.gpr[10].hi = 0x1010101010101010

        recursive_exits = []
        recursive_block_pcs = {
            0x00100000, 0x00100008, 0x00100014, 0x00100020
        }
        for _ in range(4):
            recursive_exit = recursive_fn(ctypes.pointer(recursive_state), None)
            recursive_exits.append(recursive_exit)
            if recursive_state.pc not in recursive_block_pcs:
                break
        else:
            raise AssertionError("recursive guest dispatch did not leave the translated function")

        require(recursive_exits == [3, 0, 3],
                f"recursive guest: wrong dispatcher exit sequence: {recursive_exits}")
        require(recursive_state.pc == 0xDEAD0000,
                f"recursive guest: outer return target wrong: 0x{recursive_state.pc:08x}")
        require(recursive_state.gpr[2].lo == 2,
                f"recursive guest: expected two BEQ delay-slot increments, got {recursive_state.gpr[2].lo}")
        require(recursive_state.gpr[31].lo == 0x00100020,
                f"recursive guest: recursive JAL link wrong: 0x{recursive_state.gpr[31].lo:x}")
        require(recursive_state.gpr[26].lo == 0xFFFFFFFFDEAD0000,
                f"recursive guest: saved outer return address lost: 0x{recursive_state.gpr[26].lo:x}")
        require(recursive_state.gpr[26].hi == 0x2626262626262626 and
                recursive_state.gpr[2].hi == 0x0202020202020202 and
                recursive_state.gpr[8].hi == 0x0808080808080808 and
                recursive_state.gpr[9].hi == 0x0909090909090909 and
                recursive_state.gpr[10].hi == 0x1010101010101010 and
                recursive_state.gpr[31].hi == 0x3131313131313131,
                "recursive guest: control flow clobbered an upper 64-bit GPR half")
        require(recursive_state.gpr[8].lo == 0x77 and
                recursive_state.gpr[9].lo == 0x11 and
                recursive_state.gpr[10].lo == 0x22,
                "recursive guest: one of the three delay-slot side effects was lost")
        print("control-flow recursive call/return: PASS", flush=True)
        print("control-flow nested call/resume: PASS", flush=True)

    print("control_flow_ir_runtime: PASS (JR return/dispatch; JAL/JALR; BEQ condition sampling; BLTZAL/BGEZAL/BLTZALL/BGEZALL taken and not-taken)")


if __name__ == "__main__":
    main()
