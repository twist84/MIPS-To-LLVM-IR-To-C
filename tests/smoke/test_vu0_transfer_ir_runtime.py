#!/usr/bin/env python3
"""JIT-test COP2 transfers through the real VU0 runtime helper ABI."""

import ctypes
import faulthandler
import pathlib
import re
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


ENTRY_PC = 0x00130000
EE_EXIT_RETURN = 0
EE_EXIT_DISPATCH = 3
EE_EXIT_UNSUPPORTED = 4
VU0_SYNC_COMPLETE = 0
VU0_SYNC_FAILED = 3


class U128(ctypes.Structure):
    _fields_ = [
        ("lo", ctypes.c_uint64),
        ("hi", ctypes.c_uint64),
    ]


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
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


class IALUPipeEntry(ctypes.Structure):
    _fields_ = [
        ("register_mask", ctypes.c_uint32),
        ("latency", ctypes.c_uint32),
        ("start_cycle", ctypes.c_uint64),
    ]


class VU0State(ctypes.Structure):
    _fields_ = [
        ("vi", ctypes.c_uint32 * 32),
        ("acc", U128),
        ("q", U128),
        ("p", U128),
        ("mac_flags", ctypes.c_uint32),
        ("clip_flags", ctypes.c_uint32),
        ("status_flags", ctypes.c_uint32),
        ("vpu_status", ctypes.c_uint32),
        ("fbrst", ctypes.c_uint32),
        ("cmsar1", ctypes.c_uint32),
        ("tpc", ctypes.c_uint32),
        ("micro_pc", ctypes.c_uint32),
        ("cycle", ctypes.c_uint64),
        ("ee_cycle", ctypes.c_uint64),
        ("running", ctypes.c_uint32),
        ("e_bit", ctypes.c_uint32),
        ("m_bit", ctypes.c_uint32),
        ("_padding_before_memories", ctypes.c_uint32),
        ("data_memory", ctypes.c_uint8 * 0x1000),
        ("micro_memory", ctypes.c_uint8 * 0x1000),
        ("ialu", IALUPipeEntry * 4),
        ("ialu_read_pos", ctypes.c_uint32),
        ("ialu_write_pos", ctypes.c_uint32),
        ("ialu_count", ctypes.c_uint32),
        ("branch", ctypes.c_uint32),
        ("branch_pc", ctypes.c_uint32),
        ("delay_branch_pc", ctypes.c_uint32),
        ("take_delay_branch", ctypes.c_uint32),
        ("vi_backup_cycles", ctypes.c_uint32),
        ("vi_backup_register", ctypes.c_uint32),
        ("vi_backup_old_value", ctypes.c_uint32),
        ("vf_memory_ready_cycle", ctypes.c_uint64),
        ("vf_memory_issues", ctypes.c_uint64 * 8),
        ("vf_memory_issue_index", ctypes.c_uint32),
        ("acc_overflow_mask", ctypes.c_uint32),
        ("q_ready_cycle", ctypes.c_uint64),
        ("q_pending_bits", ctypes.c_uint32),
        ("q_pending", ctypes.c_uint32),
        ("_tail_padding", ctypes.c_uint64),
    ]


assert VU0State.data_memory.offset == 240
assert VU0State.micro_memory.offset == 4336
assert VU0State.ialu.offset == 8432
assert VU0State.ialu_read_pos.offset == 8496
assert VU0State.ialu_write_pos.offset == 8500
assert VU0State.ialu_count.offset == 8504
assert VU0State.branch.offset == 8508
assert VU0State.branch_pc.offset == 8512
assert VU0State.delay_branch_pc.offset == 8516
assert VU0State.take_delay_branch.offset == 8520
assert VU0State.vi_backup_cycles.offset == 8524
assert VU0State.vi_backup_register.offset == 8528
assert VU0State.vi_backup_old_value.offset == 8532
assert VU0State.acc_overflow_mask.offset == 8612
assert VU0State.q_ready_cycle.offset == 8616
assert ctypes.sizeof(VU0State) == 8640


class Runtime(ctypes.Structure):
    _fields_ = [
        ("user", ctypes.c_void_p),
        ("vu0_state", ctypes.c_void_p),
        ("vu0_user", ctypes.c_void_p),
        ("vu0_sync", ctypes.c_void_p),
        ("vu0_control_user", ctypes.c_void_p),
        ("vu0_control", ctypes.c_void_p),
    ]


SYNC_CALLBACK = ctypes.CFUNCTYPE(
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.POINTER(EEState),
    ctypes.POINTER(VU0State),
    ctypes.c_uint32,
    ctypes.c_uint32,
)

CONTROL_CALLBACK = ctypes.CFUNCTYPE(
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.POINTER(EEState),
    ctypes.POINTER(VU0State),
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def aligned_instance(struct_type):
    backing = ctypes.create_string_buffer(ctypes.sizeof(struct_type) + 15)
    address = ctypes.addressof(backing)
    aligned = (address + 15) & ~15
    value = ctypes.cast(aligned, ctypes.POINTER(struct_type)).contents
    return backing, value


def register_runtime_symbols(library_path):
    library = ctypes.CDLL(str(library_path))
    for name in (
        "ee_vu0_qmfc2",
        "ee_vu0_qmtc2",
        "ee_vu0_cfc2",
        "ee_vu0_ctc2",
        "ee_vu0_get_branch_condition",
    ):
        symbol = getattr(library, name)
        address = ctypes.cast(symbol, ctypes.c_void_p).value
        require(address is not None and address != 0,
                f"runtime helper {name} was not exported")
        llvm.add_symbol(name, address)
    return library


def compile_jit(fixture, temp_dir, mode, target):
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture), str(ir_path), mode], check=True)

    ir_text = ir_path.read_text(encoding="utf-8")
    if mode.startswith("ir_v"):
        require("call i32 @ee_vu0_ir_" in ir_text, "missing direct VU0 IR lowering")
        require("call i32 @ee_vu0_macro_fmac(" not in ir_text and
                "call i32 @ee_vu0_macro_scalar(" not in ir_text,
                "translated arithmetic escaped to a runtime opcode helper")
    # The fixture previously modeled only the ABI's first six pointer
    # fields. Native instruction counting appends a counter after later
    # host-owned fields; a 48-byte runtime allocation would cause JIT code
    # to write beyond its backing buffer. Derive the actual field offset
    # from generated IR rather than guessing the C++ struct's tail layout.
    match = re.search(
        r"%counter_ptr = getelementptr i8, ptr %rt, i64 ([0-9]+)", ir_text
    )
    require(match is not None, "missing guest-instruction accounting offset")
    counter_offset = int(match.group(1))
    reference_match = re.search(
        r"%ref_counter_ptr = getelementptr i8, ptr %rt, i64 ([0-9]+)",
        ir_text,
    )
    require(reference_match is not None,
            "missing reference interpreter slot counter")
    counter_offset = max(counter_offset, int(reference_match.group(1)))
    require(counter_offset >= ctypes.sizeof(Runtime),
            "instrumentation counter overlaps VU0 callback ABI")

    module = llvm.parse_assembly(ir_text)
    module.verify()
    target_machine = target.create_target_machine(opt=0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)
    module.verify()

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_vu0_transfer_test")
    require(address != 0, f"{mode}: JIT function was not emitted")

    function_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return function_type(address), engine, module, target_machine, counter_offset


def make_runtime(vu0, sync_result=VU0_SYNC_COMPLETE,
                 control_result=VU0_SYNC_COMPLETE):
    sync_state = {"calls": [], "result": sync_result}
    control_state = {"calls": [], "result": control_result}

    def sync_callback(_user, _cpu, _vu0, mode, pc):
        sync_state["calls"].append((int(mode), int(pc)))
        return sync_state["result"]

    def control_callback(_user, _cpu, _vu0, register_index, value, pc):
        control_state["calls"].append(
            (int(register_index), int(value), int(pc))
        )
        return control_state["result"]

    sync_ref = SYNC_CALLBACK(sync_callback)
    control_ref = CONTROL_CALLBACK(control_callback)
    runtime = Runtime()
    runtime.vu0_state = ctypes.addressof(vu0)
    runtime.vu0_sync = ctypes.cast(sync_ref, ctypes.c_void_p).value
    runtime.vu0_control = ctypes.cast(control_ref, ctypes.c_void_p).value
    return runtime, sync_ref, control_ref, sync_state, control_state


def run_guest(fixture, temp_dir, target, mode, setup=None,
              sync_result=VU0_SYNC_COMPLETE,
              control_result=VU0_SYNC_COMPLETE,
              vu0_state_available=True):
    print("VU0 JIT: compile/execute mode=", mode, flush=True)
    guest_fn, engine, module, target_machine, counter_offset = compile_jit(
        fixture, temp_dir, mode, target
    )
    cpu_backing, cpu = aligned_instance(EEState)
    vu0_backing, vu0 = aligned_instance(VU0State)
    runtime, sync_ref, control_ref, sync_state, control_state = make_runtime(
        vu0, sync_result=sync_result, control_result=control_result
    )
    # Allocate every byte accessed by the JIT, including the appended
    # diagnostic counter. Keep ABI-prefix fields exactly as before.
    runtime_backing = ctypes.create_string_buffer(
        max(ctypes.sizeof(Runtime), counter_offset + ctypes.sizeof(ctypes.c_uint64))
    )
    # Pointer.contents assignment would *retarget* the ctypes pointer to
    # the original 48-byte Runtime object, rather than copy into the
    # extended allocation. Copy the ABI prefix into the owned backing.
    ctypes.memmove(
        ctypes.addressof(runtime_backing),
        ctypes.addressof(runtime),
        ctypes.sizeof(Runtime),
    )
    runtime_ptr = ctypes.cast(runtime_backing, ctypes.POINTER(Runtime))
    require(ctypes.addressof(runtime_ptr.contents) ==
            ctypes.addressof(runtime_backing),
            "runtime pointer must address the entire allocated ABI backing")
    if not vu0_state_available:
        runtime_ptr.contents.vu0_state = None

    if setup:
        setup(cpu, vu0)

    print("VU0 JIT: call", mode, "counter_offset", counter_offset,
          "runtime_backing_bytes", ctypes.sizeof(runtime_backing), flush=True)
    result = guest_fn(ctypes.pointer(cpu), ctypes.byref(runtime_ptr.contents))
    print("VU0 JIT: returned", mode, "exit", result, flush=True)
    retained = (
        cpu_backing, vu0_backing, runtime_backing, engine, module, target_machine,
        sync_ref, control_ref,
    )
    return result, cpu, vu0, sync_state, control_state, retained


def check_sync(sync_state, mode, pc, label):
    require(sync_state["calls"] == [(mode, pc)],
            f"{label}: expected one sync call {(mode, pc)}, "
            f"got {sync_state['calls']}")


def main():
    faulthandler.enable()
    require(
        len(sys.argv) == 3,
        "usage: test_vu0_transfer_ir_runtime.py fixture helper_library",
    )
    fixture = pathlib.Path(sys.argv[1]).resolve()
    helper_library = pathlib.Path(sys.argv[2]).resolve()

    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    runtime_library = register_runtime_symbols(helper_library)

    with tempfile.TemporaryDirectory(prefix="ee-vu0-transfer-") as directory:
        temp_dir = pathlib.Path(directory)

        def vf_words(cpu, index):
            v = cpu.vu0[index]
            return (v.lo & 0xffffffff, v.lo >> 32,
                    v.hi & 0xffffffff, v.hi >> 32)

        def set_vf(cpu, index, words):
            cpu.vu0[index].lo = words[0] | (words[1] << 32)
            cpu.vu0[index].hi = words[2] | (words[3] << 32)

        def init_mul(cpu, vu0):
            set_vf(cpu, 1, (0x3f800000, 0x16a08bd5, 0x7fffffff, 0x7fffffff))
            set_vf(cpu, 2, (0x3fffffff, 0x03f4fe21, 0x3f800000, 0x40000000))

        result, cpu, vu0, sync, _, keep = run_guest(
            fixture, temp_dir, target, "ir_vmul", init_mul)
        require(result == EE_EXIT_DISPATCH and
                vf_words(cpu, 3) == (0x3ffffffe, 0, 0x7fffffff, 0x7fffffff)
                and vu0.mac_flags == 0x1404 and vu0.status_flags == 0x34d,
                "direct VU0 VMUL result and true range flags")
        check_sync(sync, 1, ENTRY_PC, "direct VMUL")

        def init_add(cpu, vu0):
            set_vf(cpu, 1, (0x8080002c, 0x80800036, 0x80c1aa65, 0x3f800000))
            set_vf(cpu, 2, (0x008000ec, 0x00829f6a, 0x01200000, 0x3f800000))

        result, cpu, vu0, sync, _, keep = run_guest(
            fixture, temp_dir, target, "ir_vadd", init_add)
        require(result == EE_EXIT_DISPATCH and
                vf_words(cpu, 1) == (0x00400000, 0x0027cd00, 0x007cab36, 0x40000000)
                and vu0.mac_flags == 0x0e0e and vu0.status_flags == 0x145,
                "direct VU0 aliasing VADD cancellation flags")

        def init_mac(cpu, vu0):
            set_vf(cpu, 1, (0x7fffffff, 0, 0, 0))
            set_vf(cpu, 2, (0x40000000, 0, 0, 0))
            set_vf(cpu, 4, (0xbf800000, 0, 0, 0))

        result, cpu, vu0, sync, _, keep = run_guest(
            fixture, temp_dir, target, "ir_vmula_madd", init_mac)
        require(result == EE_EXIT_DISPATCH and vf_words(cpu, 3)[0] == 0x7fffffff
                and vu0.acc_overflow_mask == 8 and vu0.mac_flags == 0x8000
                and vu0.status_flags == 0x288,
                "direct VU0 ACC overflow latch and MAC sticky")
        require(sync["calls"] == [(1, ENTRY_PC), (1, ENTRY_PC + 4)],
                "each direct FMAC synchronizes")

        for mode, numerator, denominator, expected, flags in (
            ("ir_vdiv_wait", 0x3f800000, 0x3fffffff, 0x3f000001, 0),
            ("ir_vdiv_wait", 0, 0, 0x7fffffff, 0x410),
            ("ir_vsqrt_wait", 0, 0xbf800000, 0x3f800000, 0x410),
        ):
            def init_q(cpu, vu0):
                set_vf(cpu, 1, (numerator, 0, 0, 0))
                set_vf(cpu, 2, (denominator, 0, 0, 0))
            result, cpu, vu0, sync, _, keep = run_guest(
                fixture, temp_dir, target, mode, init_q)
            require(result == EE_EXIT_DISPATCH and vu0.q.lo == expected
                    and vu0.vi[22] == expected and vu0.q_pending == 0
                    and vu0.cycle == 7 and vu0.status_flags == flags,
                    "direct VU0 Q arithmetic and WAITQ retirement")

        result, cpu, vu0, sync, _, keep = run_guest(
            fixture, temp_dir, target, "ir_vmul", init_mul,
            sync_result=VU0_SYNC_FAILED)
        require(result == EE_EXIT_UNSUPPORTED and vf_words(cpu, 3) == (0, 0, 0, 0)
                and vu0.mac_flags == 0, "failed VU0 sync commits no arithmetic")

        def init_qmfc(cpu, vu0):
            cpu.gpr[8] = U128(0, 0)
            cpu.vu0[6] = U128(0x0123456789abcdef, 0xfedcba9876543210)

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmfc2", init_qmfc
        )
        require(result == EE_EXIT_DISPATCH and cpu.exit_reason == EE_EXIT_DISPATCH,
                "QMFC2 should return to the dispatcher")
        require(cpu.gpr[8].lo == 0x0123456789abcdef and
                cpu.gpr[8].hi == 0xfedcba9876543210,
                "QMFC2 must copy both VF halves into the destination GPR")
        check_sync(sync, 0, ENTRY_PC, "QMFC2")

        def init_qmtc(cpu, _vu0):
            cpu.gpr[8] = U128(0x8877665544332211, 0x0123456789abcdef)

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmtc2", init_qmtc
        )
        require(result == EE_EXIT_DISPATCH,
                "QMTC2 should return to the dispatcher")
        require(cpu.vu0[6].lo == 0x8877665544332211 and
                cpu.vu0[6].hi == 0x0123456789abcdef,
                "QMTC2 must copy both GPR halves into VF")
        check_sync(sync, 0, ENTRY_PC, "QMTC2")

        def init_cfc(cpu, vu0):
            cpu.gpr[8].hi = 0x123456789abcdef0
            vu0.vi[6] = 0x81234567

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "cfc2", init_cfc
        )
        require(result == EE_EXIT_DISPATCH,
                "CFC2 should return to the dispatcher")
        require(cpu.gpr[8].lo == 0xffffffff81234567,
                "CFC2 must sign-extend an ordinary VI value")
        require(cpu.gpr[8].hi == 0x123456789abcdef0,
                "CFC2 must preserve the upper 64-bit GPR half")
        check_sync(sync, 0, ENTRY_PC, "CFC2")

        def init_ctc(cpu, _vu0):
            cpu.gpr[8].lo = 0x89abcdef

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2", init_ctc
        )
        require(result == EE_EXIT_DISPATCH and vu0.vi[6] == 0x89abcdef,
                "CTC2 must write the low GPR word to VI")
        check_sync(sync, 0, ENTRY_PC, "CTC2")

        # VF0 is the fixed (0, 0, 0, 1.0f) vector, independent of its
        # backing slot, and writes to VF0 are discarded.
        def init_vf0_read(cpu, _vu0):
            cpu.vu0[0] = U128(0xdeadbeef01234567, 0x89abcdef76543210)

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmfc2_vf0", init_vf0_read
        )
        require(cpu.gpr[8].lo == 0 and
                cpu.gpr[8].hi == 0x3f80000000000000,
                "QMFC2 VF0 must return the constant vector (0,0,0,1.0)")
        check_sync(sync, 0, ENTRY_PC, "QMFC2 VF0")

        def init_vf0_write(cpu, _vu0):
            cpu.vu0[0] = U128(0xdeadbeef01234567, 0x89abcdef76543210)
            cpu.gpr[8] = U128(0x0123456789abcdef, 0xfedcba9876543210)

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmtc2_vf0", init_vf0_write
        )
        require(cpu.vu0[0].lo == 0xdeadbeef01234567 and
                cpu.vu0[0].hi == 0x89abcdef76543210,
                "QMTC2 to VF0 must leave the backing slot untouched")
        check_sync(sync, 0, ENTRY_PC, "QMTC2 VF0")

        # CFC2's R special case replaces only UL[0], preserving both UL[1]
        # and the separate upper 64-bit half of the GPR.
        def init_cfc_r(cpu, vu0):
            cpu.gpr[8].lo = 0xfeedfacecafebeef
            cpu.gpr[8].hi = 0x123456789abcdef0
            vu0.vi[20] = 0xffabcdef

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "cfc2_r", init_cfc_r
        )
        require(cpu.gpr[8].lo == 0xfeedface002bcdef,
                "CFC2 R must mask UL[0] and preserve UL[1]")
        require(cpu.gpr[8].hi == 0x123456789abcdef0,
                "CFC2 R must preserve the upper 64-bit GPR half")
        check_sync(sync, 0, ENTRY_PC, "CFC2 R")

        def init_ctc_r(cpu, _vu0):
            cpu.gpr[8].lo = 0xabcdef01

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2_r", init_ctc_r
        )
        require(vu0.vi[20] == ((0xabcdef01 & 0x007fffff) | 0x3f800000),
                "CTC2 R must force the implicit 1.0 exponent")
        check_sync(sync, 0, ENTRY_PC, "CTC2 R")

        def init_fbrst(cpu, _vu0):
            cpu.gpr[8].lo = 0x00000c0f

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2_fbrst", init_fbrst
        )
        require(vu0.vi[28] == 0x00000c0c,
                "CTC2 FBRST must store only mask 0x0c0c")
        require(control["calls"] == [(28, 0x00000c0f, ENTRY_PC)],
                "CTC2 FBRST must forward action bits and guest PC")
        check_sync(sync, 0, ENTRY_PC, "CTC2 FBRST")

        def init_cmsar1(cpu, _vu0):
            cpu.gpr[8].lo = 0x12345678

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2_cmsar1", init_cmsar1
        )
        require(vu0.cmsar1 == 0x5678,
                "CTC2 CMSAR1 must retain its low 16-bit micro entry")
        require(control["calls"] == [(31, 0x5678, ENTRY_PC)],
                "CTC2 CMSAR1 must forward the low 16 bits and PC")
        check_sync(sync, 0, ENTRY_PC, "CTC2 CMSAR1")

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2_cmsar1", init_cmsar1,
            control_result=VU0_SYNC_FAILED
        )
        require(result == EE_EXIT_UNSUPPORTED and
                cpu.exit_reason == EE_EXIT_UNSUPPORTED and cpu.pc == ENTRY_PC,
                "failed CMSAR1 host action must exit at the transfer")
        require(vu0.cmsar1 == 0,
                "failed CMSAR1 action must not commit register state")
        require(control["calls"] == [(31, 0x5678, ENTRY_PC)],
                "failed CMSAR1 action must still be observable by the host")

        # The transfer's encoded I bit controls whether to interlock;
        # ordinary .NI instructions only catch up to the EE instruction
        # slot even when they execute inside a branch delay slot.
        for mode, expected_mode in (
            ("qmfc2_delay", 0),
            ("cfc2_delay", 0),
            ("qmtc2_delay", 0),
            ("ctc2_delay", 0),
            ("qmfc2_i", 1),
            ("cfc2_i", 1),
            ("qmtc2_i", 2),
            ("ctc2_i", 2),
            ("qmfc2_i_delay", 1),
            ("cfc2_i_delay", 1),
            ("qmtc2_i_delay", 2),
            ("ctc2_i_delay", 2),
        ):
            def init_delay(cpu, vu0):
                cpu.gpr[8] = U128(0x1122334455667788, 0x99aabbccddeeff00)
                vu0.vi[6] = 0x12345678

            result, cpu, vu0, sync, control, keep = run_guest(
                fixture, temp_dir, target, mode, init_delay
            )
            expected_pc = ENTRY_PC + (4 if "_delay" in mode else 0)
            check_sync(sync, expected_mode, expected_pc, mode)

        # Sync refusal must preserve the destination regardless of
        # whether the transfer explicitly interlocks.
        for mode, expected_mode in (("qmfc2_i", 1), ("qmfc2_delay", 0)):
            def init_refused(cpu, _vu0):
                cpu.gpr[8] = U128(0x76543210, 0x0123456789abcdef)
                cpu.vu0[6] = U128(0x99999999, 0x88888888)

            result, cpu, vu0, sync, control, keep = run_guest(
                fixture, temp_dir, target, mode, init_refused,
                sync_result=VU0_SYNC_FAILED
            )
            require(result == EE_EXIT_UNSUPPORTED and cpu.pc ==
                    (ENTRY_PC + 4 if "_delay" in mode else ENTRY_PC),
                    f"{mode}: failed sync must preserve guest transfer PC")
            require(cpu.gpr[8].lo == 0x76543210 and
                    cpu.gpr[8].hi == 0x0123456789abcdef,
                    f"{mode}: failing read changed guest GPR")
            check_sync(sync, expected_mode,
                       ENTRY_PC + (4 if "_delay" in mode else 0), mode)

        # GPR writes made in SSA before a helper call must be visible to the
        # helper, and GPR writes made by a helper must be visible afterwards.
        def init_qmtc_after_addiu(cpu, _vu0):
            cpu.gpr[8].hi = 0x1020304050607080

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmtc2_after_addiu",
            init_qmtc_after_addiu
        )
        require(cpu.vu0[6].lo == 0x1234 and
                cpu.vu0[6].hi == 0x1020304050607080,
                "QMTC2 must observe preceding ADDIU after the GPR cache flush")

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "ctc2_after_addiu"
        )
        require(vu0.vi[6] == 0x1234,
                "CTC2 must observe preceding ADDIU after the GPR cache flush")

        def init_cfc_then_addiu(_cpu, vu0):
            vu0.vi[6] = 0x7fffffff

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "cfc2_then_addiu",
            init_cfc_then_addiu
        )
        require(cpu.gpr[10].lo == 0xffffffff80000000,
                "later ALU instruction must reload the GPR written by CFC2")

        # BC2 samples VPU_STAT bit 8 before the architectural delay slot.
        # Both ordinary and likely forms must route to the same successors as
        # the PCSX2 interpreter, with likely-not-taken slots annulled.
        def run_bc2(mode, condition, taken, likely, label):
            def init_branch(cpu, vu0):
                cpu.gpr[8].lo = 0x7777
                cpu.gpr[9].lo = 0x8888
                cpu.gpr[10].lo = 0x9999
                vu0.vpu_status = (1 << 8) if condition else 0

            result, cpu, vu0, sync, control, keep = run_guest(
                fixture, temp_dir, target, mode, init_branch
            )
            expected_exit = EE_EXIT_RETURN if taken else EE_EXIT_DISPATCH
            require(result == expected_exit and cpu.exit_reason == expected_exit,
                    f"{label}: wrong branch exit for condition={condition}")
            delay_executed = not likely or taken
            expected_gpr8 = 0xffffffffffffaaaa if delay_executed else 0x7777
            require(cpu.gpr[8].lo == expected_gpr8,
                    f"{label}: GPR8=0x{cpu.gpr[8].lo:016x}, "
                    f"expected 0x{expected_gpr8:016x}; "
                    f"GPR9=0x{cpu.gpr[9].lo:016x}, "
                    f"GPR10=0x{cpu.gpr[10].lo:016x}, "
                    f"pc=0x{cpu.pc:08x}, exit={result}")
            require(cpu.gpr[9].lo == (0x8888 if taken else 0x1111),
                    f"{label}: wrong fallthrough state for condition={condition}")
            require(cpu.gpr[10].lo == (0x2222 if taken else 0x9999),
                    f"{label}: wrong taken-target state for condition={condition}")

        run_bc2("bc2_f_delay", False, True, False, "BC2F taken")
        run_bc2("bc2_f_delay", True, False, False, "BC2F not taken")
        run_bc2("bc2_t_delay", True, True, False, "BC2T taken")
        run_bc2("bc2_t_delay", False, False, False, "BC2T not taken")
        run_bc2("bc2_fl_annul", False, True, True, "BC2FL taken")
        run_bc2("bc2_fl_annul", True, False, True, "BC2FL annulled")
        run_bc2("bc2_tl_delay", True, True, True, "BC2TL taken")
        run_bc2("bc2_tl_delay", False, False, True, "BC2TL annulled")

        # Without a VU0 state object there is no valid predicate. The
        # generated branch must fail closed at the BC2 instruction.
        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "bc2_t_delay",
            vu0_state_available=False
        )
        require(result == EE_EXIT_UNSUPPORTED and
                cpu.exit_reason == EE_EXIT_UNSUPPORTED and cpu.pc == ENTRY_PC,
                "BC2 must not guess a branch direction without VU0 state")

        # Runtime callback failure stops at the transfer PC and leaves the
        # helper's destination untouched.
        def init_failure(cpu, vu0):
            cpu.gpr[8] = U128(0xaaaaaaaa55555555, 0xbbbbbbbb66666666)
            cpu.vu0[6] = U128(0x0123456789abcdef, 0xfedcba9876543210)

        result, cpu, vu0, sync, control, keep = run_guest(
            fixture, temp_dir, target, "qmfc2", init_failure,
            sync_result=VU0_SYNC_FAILED
        )
        require(result == EE_EXIT_UNSUPPORTED and
                cpu.exit_reason == EE_EXIT_UNSUPPORTED and cpu.pc == ENTRY_PC,
                "failed VU0 sync must exit at the transfer instruction")
        require(cpu.gpr[8].lo == 0xaaaaaaaa55555555 and
                cpu.gpr[8].hi == 0xbbbbbbbb66666666,
                "failed QMFC2 must not modify its destination")
        check_sync(sync, 0, ENTRY_PC, "failed QMFC2")

    # Keep the library loaded until every JIT engine has been destroyed.
    _ = runtime_library
    print(
        "vu0_transfer_ir_runtime: PASS (QMFC2/QMTC2/CFC2/CTC2 helper calls, "
        "VF0/R special cases, CTC2 control callbacks, BC2 ordinary/likely "
        "branches, GPR cache barriers, delay-slot synchronization, failure exits)"
    )


if __name__ == "__main__":
    main()
