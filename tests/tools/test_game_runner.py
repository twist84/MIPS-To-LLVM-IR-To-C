#!/usr/bin/env python3
"""Synthetic ELF32-to-native-runner integration test.

Runs a real MIPS ELF entry through the full emitter, object linker and
bounded native dispatcher. This is not a claim of PS2 BIOS bootability.
"""
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile


def make_elf(path, startup=False):
    image = bytearray(0x140)
    image[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into(
        "<HHIIIIIHHHHHH", image, 16,
        2, 8, 1, 0x00100008, 52, 0, 0,
        52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<IIIIIIII", image, 52,
        1, 0x100, 0x00100000, 0x00100000,
        0x40, 0x60, 5, 16,
    )
    # Entry starts after two NOPs. ADDIU $v0,$zero,7;
    # JR $ra; NOP (delay slot).
    instructions = (
        (
            (0x108, 0x2405ffff), # a1 = -1: auto-allocate stack
            (0x10c, 0x24064000), # a2 = 16 KiB
            (0x110, 0x2403003c), # v1 = SetupThread
            (0x114, 0x0000000c), # syscall
            (0x118, 0x0040e821), # addu sp, v0, zero
            (0x11c, 0x03e00008), # jr ra
        ) if startup else (
            (0x108, 0x24020007),
            (0x10c, 0x03e00008),
        )
    )
    for offset, value in instructions:
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_guest_word_watch_elf(path):
    """One native function modifies a guest word and returns to PC zero."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    words = (
        0x3c080010,  # lui t0,0x0010
        0x35080200,  # ori t0,t0,0x0200
        0x24090007,  # addiu t1,zero,7
        0xad090000,  # sw t1,0(t0)
        0x00000008,  # jr zero
        0x24020007,  # delay slot: addiu v0,zero,7; trace exact guest PC
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    path.write_bytes(image)


def make_deci2_open_close_elf(path, invalid_open=False):
    """Exercise DECI2 OPEN/CLOSE via real translated MIPS syscalls."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    # Four guest words at VA 0x00100200; OPEN reads words 1 and 2.
    struct.pack_into(
        "<IIII", image, 0x300,
        0x11223344, 0x00100220, 0x00100060, 0x55667788,
    )
    if invalid_open:
        # Refuse before changing DECI2 state or modifying guest memory.
        instructions = (
            0x3c057fff,  # lui a1,0x7fff
            0x34a5fffc,  # ori a1,a1,0xfffc (unbacked 0x7ffffffc)
            0x24040001,  # a0=1 OPEN
            0x2403007c,  # v1=Deci2Call
            0x0000000c,  # syscall: must remain an exception
            0x00404021,  # unreachable: addu t0,v0,zero
            0x00000008,
            0x00000000,
        )
    else:
        instructions = (
            0x3c050010,  # lui a1,0x0010
            0x34a50200,  # ori a1,a1,0x200
            0x24040001,  # a0=1 OPEN
            0x2403007c,  # v1=Deci2Call
            0x0000000c,  # syscall
            0x00404021,  # t0=OPEN result 1
            0x24040002,  # a0=2 CLOSE (A1 remains unchanged)
            0x0000000c,  # syscall
            0x00404821,  # t1=CLOSE result 1
            0x3c0b0010,  # lui t3,0x0010
            0x356b0200,  # ori t3,t3,0x200
            0x8d700000,  # lw s0,0(t3): original word 0
            0x8d710004,  # lw s1,4(t3): original word 1
            0x8d720008,  # lw s2,8(t3): original word 2
            0x8d73000c,  # lw s3,12(t3): original word 3
            0x24040007,  # a0=7 unknown DECI2 operation
            0x0000000c,  # syscall: fail closed for unknown opcode
            0x00405021,  # unreachable: t2=V0
            0x00000008,
            0x00000000,
        )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)



def make_deci2_kputs_elf(path, invalid_argument=False, invalid_string=False):
    """Real Deci2Call(0x10) uses A1 as pointer-to-guest-string-pointer."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    instructions = (
        0x3c050010,  # lui a1,0x0010
        0x34a50201 if invalid_argument else 0x34a50200,
                     # invalid argument: unaligned 0x00100201
        0x24040010,  # a0=0x10 KPUTS
        0x2403007c,  # v1=Deci2Call
        0x0000000c,  # syscall
        0x00404021,  # t0 = returned 1
        0x24040007,  # deliberately unsupported operation
        0x0000000c,  # remains an exception
        0x00404821,  # unreachable t1
        0x00000008,
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    struct.pack_into(
        "<I", image, 0x300,
        0x7ffffffc if invalid_string else 0x00100220,
    )
    image[0x320:0x334] = b"DECI2 KPUTS SUCCESS\x00"
    path.write_bytes(image)


def make_sif_set_dchain_elf(path):
    """SIF0 kernel initialization must set real guest-visible DMAC MMIO."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    instructions = (
        0x3c081000,  # lui t0,0x1000 (SIF0 CHCR 0x1000c000)
        0x3508c000,  # ori t0,t0,0xc000
        0x24090055,  # addiu t1,zero,0x55
        0xad090020,  # sw t1,0x20(t0): initial nonzero QWC
        0xad000000,  # sw zero,0(t0): reset CHCR to idle
        0x3c021000,  # lui v0,0x1000 (must survive void syscall)
        0x3442c000,  # ori v0,v0,0xc000
        0x24030078,  # addiu v1,zero,SifSetDChain
        0x0000000c,  # syscall
        0x00408021,  # addu s0,v0,zero (preserved)
        0x8d110020,  # lw s1,0x20(t0): QWC=0
        0x8d120000,  # lw s2,0(t0): CHCR=0x184
        0x8d130010,  # lw s3,0x10(t0): MADR not fabricated
        0x24030079,  # addiu v1,zero,SifSetReg (not yet supported)
        0x0000000c,  # syscall: must fail closed
        0x00000008,  # jr zero
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_sif_poll_elf(path):
    """Spin on unserved SIF SMFLAG, without any synthetic IOP producer."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    instructions = (
        0x24040004,  # addiu a0, zero, 4: SIF_REG_SMFLAG
        0x2403007a,  # addiu v1, zero, 0x7a: sceSifGetReg
        0x0000000c,  # syscall, guest PC 0x00100010
        0x1000fffe,  # beq zero,zero,-2: return to same syscall
        0x00000000,  # delay slot
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_sif_handshake_elf(path):
    """EE branches out of SMFLAG polling only after a genuine IOP write."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    instructions = (
        0x24040004,  # addiu a0,zero,4: SIF_REG_SMFLAG
        0x2403007a,  # addiu v1,zero,0x7a: SifGetReg
        0x0000000c,  # syscall
        0x1040fffe,  # beq v0,zero,-2 (back to syscall)
        0x00000000,  # delay slot
        0x00408021,  # addu s0,v0,zero: capture observed SMFLAG
        0x0000000d,  # break: deliberate fixture termination
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_sif_guest_irx(path, unknown_import=False, unhandled_syscall=False):
    """Non-copyrighted IOP IRX: guest code calls sifman #24 then #23."""
    image = bytearray(0x200)
    image[0:7] = bytes((0x7f, 0x45, 0x4c, 0x46, 1, 1, 1))
    struct.pack_into("<HH", image, 16, 0xff80, 8)
    struct.pack_into("<I", image, 28, 0x34)
    struct.pack_into("<HHH", image, 40, 52, 32, 2)

    # One PT_LOAD plus PT_SCE_IOPMOD.
    struct.pack_into("<IIIIII", image, 0x34, 1, 0x100, 0, 0, 0x70, 0x80)
    struct.pack_into("<II", image, 0x54, 0x70000080, 0x180)
    struct.pack_into("<IIIII", image, 0x184, 0, 0x60, 0x40, 0x20, 0x20)
    image[0x19a:0x19a + 8] = b"SIFTEST" + bytes(1)

    if unhandled_syscall:
        instructions = (
            0x24020063,  # addiu v0,zero,99: unimplemented IOP service
            0x0000000c,  # syscall
            0x08008002,  # loop (unreachable)
            0x00000000,
        )
    else:
        instructions = (
            0x3c040006,  # lui a0,6 => SMFLAG bits 0x60000
            0x0c00800f,  # jal 0x2003c (sifman #24)
            0x00000000,  # delay slot
            0x0c00800d,  # jal 0x20034 (sifman #23)
            0x00000000,  # delay slot
            0x08008005,  # j 0x20014
            0x00000000,  # delay slot
        )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x100 + index * 4, opcode)

    # IRX import stubs use the actual library resolver, not fixed HLE PCs.
    struct.pack_into("<I", image, 0x120, 0x41e00000)
    image[0x12c:0x132] = b"sifman"
    struct.pack_into("<II", image, 0x134, 0x03e00008, 0x24000017)
    struct.pack_into(
        "<II", image, 0x13c, 0x03e00008,
        # SIFMAN #32 (sceSifSetDmaIntr) is now implemented.
        # Keep this negative fixture on still-unimplemented ordinal #33.
        # An unknown export must fail at IRX linkage, not guest syscall.
        0x24000021 if unknown_import else 0x24000018,
    )
    path.write_bytes(image)


def make_iop_provider_irx(path):
    """Synthetic IRX exporting PROVIDER ordinal 0 returning 0x60000."""
    image = bytearray(0x2a0)
    image[0:7] = bytes((0x7f, 0x45, 0x4c, 0x46, 1, 1, 1))
    struct.pack_into("<HH", image, 16, 0xff80, 8)
    struct.pack_into("<I", image, 28, 0x34)
    struct.pack_into("<I", image, 32, 0x240)
    struct.pack_into("<HHH", image, 40, 52, 32, 2)
    struct.pack_into("<HH", image, 46, 40, 1)
    struct.pack_into("<IIIIII", image, 0x34, 1, 0x100, 0, 0, 0x80, 0x80)
    struct.pack_into("<II", image, 0x54, 0x70000080, 0x200)
    struct.pack_into("<IIIII", image, 0x204, 0, 0x50, 0x60, 0x10, 0x10)
    image[0x21a:0x222] = b"PROVIDER"

    # Provider entry is safe to leave dormant or run as an idle loop.
    struct.pack_into("<II", image, 0x100, 0x08000000, 0)
    struct.pack_into("<III", image, 0x110,
                     0x3c020006,  # lui v0,6
                     0x03e00008,  # jr ra
                     0)           # delay slot
    struct.pack_into("<I", image, 0x120, 0x41c00000)
    image[0x12c:0x134] = b"PROVIDER"
    struct.pack_into("<II", image, 0x134, 0x10, 0)

    # Export table contains one R_MIPS_32 relocated pointer.
    struct.pack_into("<I", image, 0x244, 9)
    struct.pack_into("<II", image, 0x250, 0x280, 8)
    struct.pack_into("<II", image, 0x280, 0x34, 2)
    # Idle jump also needs relocation to its allocated module base.
    struct.pack_into("<I", image, 0x254, 16)
    struct.pack_into("<II", image, 0x288, 0, 4)
    path.write_bytes(image)


def make_iop_consumer_irx(path):
    """Calls imported PROVIDER #0, forwards its result to sifman #24."""
    image = bytearray(0x2c0)
    image[0:7] = bytes((0x7f, 0x45, 0x4c, 0x46, 1, 1, 1))
    struct.pack_into("<HH", image, 16, 0xff80, 8)
    struct.pack_into("<I", image, 28, 0x34)
    struct.pack_into("<I", image, 32, 0x240)
    struct.pack_into("<HHH", image, 40, 52, 32, 2)
    struct.pack_into("<HH", image, 46, 40, 1)
    struct.pack_into("<IIIIII", image, 0x34, 1, 0x100, 0, 0, 0x90, 0x90)
    struct.pack_into("<II", image, 0x54, 0x70000080, 0x200)
    struct.pack_into("<IIIII", image, 0x204, 0, 0x60, 0x80, 0x10, 0x00)
    image[0x21a:0x223] = b"CONSUMER" + bytes(1)

    instructions = (
        0x0c00000d,  # jal PROVIDER import stub at module+0x34
        0x00000000,  # delay slot
        0x00402021,  # addu a0,v0,zero
        0x0c000015,  # jal sifman ordinal 24 at module+0x54
        0x00000000,  # delay slot
        0x08000005,  # idle loop at module+0x14
        0x00000000,  # delay slot
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x100 + index * 4, opcode)

    struct.pack_into("<I", image, 0x120, 0x41e00000)
    image[0x12c:0x134] = b"PROVIDER"
    struct.pack_into("<II", image, 0x134, 0x03e00008, 0x24000000)
    struct.pack_into("<I", image, 0x140, 0x41e00000)
    image[0x14c:0x152] = b"sifman"
    struct.pack_into("<II", image, 0x154, 0x03e00008, 0x24000018)

    # R_MIPS_26 relocations for both JALs and the idle jump.
    struct.pack_into("<I", image, 0x244, 9)
    struct.pack_into("<II", image, 0x250, 0x280, 24)
    for index, offset in enumerate((0, 12, 20)):
        struct.pack_into("<II", image, 0x280 + index * 8, offset, 4)
    path.write_bytes(image)


def make_sif_register_elf(path):
    """SifGetReg/SetReg and physical SIF MMIO must share guest-visible state."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x280 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x180)
    struct.pack_into("<I", image, 52 + 20, 0x180)

    instructions = (
        0x3c048000,  # a0=SIF_SYSREG_SUBADDR
        0x2403007a,  # SifGetReg
        0x0000000c,  # initial software SIF register, no IOP
        0x00408021,  # initially zero
        0x3c048000,  # a0=SIF_SYSREG_SUBADDR
        0x3c050010,  # address hi
        0x34a52200,  # a1=0x00102200
        0x24030079,  # SifSetReg
        0x0000000c,  # persist software SIF register
        0x00408821,  # set result=0
        0x2403007a,  # SifGetReg
        0x0000000c,  # read back software state
        0x00409021,  # readback 0x00102200
        0x24040001,  # SIF_REG_MAINADDR
        0x3c050010,  # address hi
        0x34a52300,  # a1=0x00102300
        0x24030079,  # SifSetReg
        0x0000000c,  # publish EE receive buffer
        0x00409821,  # set result=0
        0x3c081000,  # SIF MMIO hi
        0x3508f200,  # MSCOM physical address
        0x8d140000,  # guest reads same MMIO value
        0x24040001,  # SIF_REG_MAINADDR
        0x2403007a,  # SifGetReg
        0x0000000c,  # read same MSCOM via kernel
        0x0040a821,  # readback 0x00102300
        0x24040003,  # SIF_REG_MSFLAG
        0x3c050002,  # EE CMDINIT bit
        0x24030079,  # SifSetReg
        0x0000000c,  # OR MSFLAG
        0x8d160020,  # read MSFLAG via MMIO
        0x24040002,  # SIF_REG_SUBADDR: still IOP-owned
        0x2403007a,  # SifGetReg
        0x0000000c,  # no fake IOP bootstrap
        0x0040b821,  # SUBADDR=0
        0x3c047fff,  # unsupported register
        0x34840000,  # unsupported id unchanged
        0x2403007a,  # SifGetReg
        0x0000000c,  # must fail closed
        0x24090001,  # unreachable
        0x00000008,  # jr zero
        0x00000000,  # nop
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_deci2_reqsend_elf(path, invalid_channel=False):
    """Exercise the reference DECI2 channel acknowledgement in real MIPS."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x600 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x500)
    struct.pack_into("<I", image, 52 + 20, 0x500)

    # OPEN's argument descriptor: word 1 registers a separate channel.
    channel = 0x7FFFFFFC if invalid_channel else 0x00100220
    struct.pack_into(
        "<IIII", image, 0x300,
        0x11223344, channel, 0x00100090, 0x55667788,
    )
    # Eight-word registered DECI2 channel: word 3 is cleared by REQSEND.
    struct.pack_into(
        "<8I", image, 0x320,
        0x1234, 0x10, 0, 0xCAFEBABE, 0x00100260, 5, 6, 7,
    )
    image[0x36c:0x370] = b"PING"

    words = (
        0x3c050010,  # lui a1,0x0010
        0x34a50200,  # ori a1,a1,0x200
        0x24040001,  # a0=OPEN
        0x2403007c,  # v1=Deci2Call
        0x0000000c,  # syscall
        0x00404021,  # t0=V0 (OPEN returned 1)
        0x24040003,  # a0=REQSEND
        0x0000000c,  # syscall (reject invalid channel)
        0x00404821,  # t1=V0 (REQSEND returned 1)
        0x3c0b0010,  # lui t3,0x0010
        0x356b0220,  # ori t3,t3,0x220
        0x8d70000c,  # lw s0,12(t3): pending word cleared
        0x8d710004,  # lw s1,4(t3): packet size unchanged
        0x8d720010,  # lw s2,16(t3): packet pointer unchanged
        0x24040004,  # a0=POLL
        0x0000000c,
        0x00407021,  # t6=1
        0x24040005,  # a0=EXRECV
        0x0000000c,
        0x00407821,  # t7=1
        0x24040006,  # a0=EXSEND
        0x0000000c,
        0x0040c021,  # t8=1
        0x24040002,  # a0=CLOSE
        0x0000000c,
        0x00405021,  # t2=1
        0x24040003,  # a0=REQSEND without active registration
        0x0000000c,
        0x00406021,  # t4=1
        0x24040007,  # unknown operation
        0x0000000c,  # must remain an exception
        0x00406821,  # unreachable: t5
        0x00000008,  # jr zero
        0x00000000,
    )
    for index, opcode in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_create_thread_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    # PT_LOAD guest descriptor at 0x00100200; CreateThread only
    # registers the dormant thread, it must never execute its entry.
    # The third call uses an unaligned descriptor and must fail closed.
    instructions = (
        (0x108, 0x3c040010),  # lui a0,0x0010
        (0x10c, 0x34840200),  # ori a0,a0,0x200
        (0x110, 0x24030020),  # v1 = CreateThread
        (0x114, 0x0000000c),  # syscall
        (0x118, 0x00404021),  # t0 = returned thread id 2
        (0x11c, 0x3c040010),  # lui a0,0x0010
        (0x120, 0x34840200),  # ori a0,a0,0x200
        (0x124, 0x24030020),  # v1 = CreateThread
        (0x128, 0x0000000c),  # syscall
        (0x12c, 0x00404821),  # t1 = returned thread id 3
        (0x130, 0x3c040010),  # lui a0,0x0010
        (0x134, 0x34840201),  # ori a0,a0,0x201 (unaligned)
        (0x138, 0x24030020),  # v1 = CreateThread
        (0x13c, 0x0000000c),  # syscall must be rejected
        (0x140, 0x240a0001),  # unreachable after invalid descriptor
        (0x144, 0x00000008),  # jr zero
        (0x148, 0x00000000),
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    descriptor = (
        0x00000000,  # initial slot
        0x001000f0,  # thread entry (not executed)
        0x00102000,  # stack base
        0x00000800,  # stack size
        0x0010f000,  # gp
        0x00000040,  # priority
        0x01020304,  # reserved field
        0x0000abcd,  # attr
        0xaabbccdd,  # option
    )
    struct.pack_into("<9I", image, 0x300, *descriptor)
    path.write_bytes(image)


def make_heap_boundary_elf(path):
    """EndOfHeap reads the process-wide extent committed by SetupHeap."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    instructions = (
        0x2403003e,  # EndOfHeap before SetupHeap must return initial 0
        0x0000000c,  # syscall
        0x00404021,  # t0 = default end (zero)
        0x3c040050,  # lui a0,0x0050 (GP)
        0x34843070,  # ori a0,a0,0x3070
        0x2405ffff,  # a1 = -1: auto-select initial main stack
        0x24064000,  # a2 = 16 KiB stack
        0x2403003c,  # v1 = SetupThread
        0x0000000c,
        0x00404821,  # t1 = initialized stack top
        0x3c040064,  # lui a0,0x0064
        0x34840380,  # a0 = 0x00640380 (heap base)
        0x2405ffff,  # a1 = -1: heap extends to stack bottom
        0x2403003d,  # v1 = SetupHeap
        0x0000000c,
        0x00405021,  # t2 = 0x01ffb000, SetupHeap return value
        0x2403003e,  # v1 = EndOfHeap
        0x0000000c,
        0x00405821,  # t3 = 0x01ffb000
        0x3c040064,  # a0 = 0x00640000
        0x24052000,  # a1 = explicit heap size 0x2000
        0x2403003d,  # SetupHeap
        0x0000000c,
        0x00406021,  # t4 = 0x00642000
        0x2403003e,  # EndOfHeap
        0x0000000c,
        0x00406821,  # t5 = 0x00642000
        0x3c041234,  # bogus A0: EndOfHeap has no arguments
        0x34845678,
        0x2403003e,
        0x0000000c,
        0x00407021,  # t6 = 0x00642000 regardless of A0
        0x00000008,  # jr zero
        0x00000000,  # delay-slot nop
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    path.write_bytes(image)


def make_shared_heap_elf(path):
    """A real EE worker sees the main thread's configured libc heap."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    entry = (
        0x24040000,  # gp = 0
        0x2405ffff,  # automatic startup stack
        0x24064000,  # stack size 16 KiB
        0x2403003c,  # SetupThread
        0x0000000c,
        0x3c040064,  # heap base 0x00640000
        0x2405ffff,  # all available memory below startup stack
        0x2403003d,  # SetupHeap
        0x0000000c,
        0x00408021,  # s0 = 0x01ffb000
        0x3c040010,
        0x34840200,  # thread descriptor
        0x24030020,
        0x0000000c,
        0x00408821,  # s1 = worker ID
        0x24040002,
        0x24050000,
        0x24030022,  # StartThread
        0x0000000c,
        0x24030032,  # SleepThread: actually enter worker
        0x0000000c,
        0x3c080010,
        0x35080240,  # t0 = worker result address
        0x8d120000,  # lw s2,0(t0): worker result
        0x2403003e,  # root still sees same limit
        0x0000000c,
        0x00409821,  # s3 = root EndOfHeap result
        0x00000008,
        0x00000000,
    )
    worker = (
        0x2403003e,  # EndOfHeap called from thread 2
        0x0000000c,
        0x3c080010,
        0x35080240,
        0xad020000,  # save result in guest RAM
        0x24040001,
        0x24030033,  # WakeupThread(1)
        0x0000000c,
        0x24030023,
        0x0000000c,
        0x00000008,
        0x00000000,
    )
    for index, word in enumerate(entry):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    for index, word in enumerate(worker):
        struct.pack_into("<I", image, 0x1f0 + index * 4, word)
    struct.pack_into("<9I", image, 0x300, 0, 0x001000f0, 0x00102000,
                     0x800, 0, 64, 0, 0, 0)
    path.write_bytes(image)


def make_osd_config_elf(path, invalid_set=False):
    """Get/SetOsdConfigParam use guest pointers to one 32-bit OSD word."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # Preserve the test code and three OSD words inside the PT_LOAD
    # segment, which begins at image offset 0x100.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    # Guest OSD pointers: 0x00100200 (default GET),
    # 0x00100210 (SET input), 0x00100220 (persisted GET).
    instructions = (
        0x3c040010,  # lui a0,0x0010
        0x34840200,  # ori a0,a0,0x0200
        0x2403004b,  # v1 = GetOsdConfigParam
        0x0000000c,  # syscall
        0x8c880000,  # lw t0,0(a0): default configuration 0
        0x00404821,  # addu t1,v0,zero: success = 0
        0x3c040010,
        0x34840210,  # a0 = 0x00100210
        0x2403004a,  # v1 = SetOsdConfigParam
        0x0000000c,
        0x00405021,  # t2 = setter return 0
        0x3c040010,
        0x34840220,  # a0 = 0x00100220
        0x2403004b,  # v1 = GetOsdConfigParam
        0x0000000c,
        0x8c8b0000,  # lw t3,0(a0): configured word
        0x00406021,  # t4 = getter return 0
        0x3c040010 if invalid_set else 0x3c047fff,
        0x34840221 if invalid_set else 0x3484fffc,
                     # invalid SET: unaligned 0x00100221
                     # invalid GET: unmapped 0x7ffffffc
        0x2403004a if invalid_set else 0x2403004b,
        0x0000000c,  # must refuse invalid pointer, no write
        0x240d0001,  # unreachable after rejected syscall
        0x00000008,  # jr zero
        0x00000000,
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    struct.pack_into("<I", image, 0x300, 0xcafebabe)
    struct.pack_into("<I", image, 0x310, 0x12345678)
    struct.pack_into("<I", image, 0x320, 0xfeedbabe)
    path.write_bytes(image)


def make_thread_sleep_wakeup_elf(path):
    """Original EE syscalls: wakeup credit, actual SleepThread, worker wake."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    entry = (
        0x3c040010,  # lui a0,0x0010
        0x34840200,  # ori a0,a0,0x200: thread descriptor
        0x24030020,  # CreateThread
        0x0000000c,
        0x00408021,  # s0 = thread ID 2
        0x24040001,  # a0 = running thread ID 1
        0x24030033,  # WakeupThread(1): self wakeup must fail
        0x0000000c,
        0x00408821,  # s1 = -1
        0x24040002,  # a0 = dormant worker ID 2
        0x24030033,  # WakeupThread(2): dormant target must fail
        0x0000000c,
        0x0040b021,  # s6 = -1
        0x24040002,  # a0 = worker ID 2
        0x24030022,  # StartThread(2), worker is READY at priority 1
        0x0000000c,
        0x24030032,  # SleepThread with no credit: block thread 1
        0x0000000c,  # switch to guest worker entry
        0x00409021,  # s2 = SleepThread result 1 after wake
        0x24030032,  # consume worker-granted wakeup credit without blocking
        0x0000000c,
        0x00409821,  # s3 = SleepThread result 1
        0x24040001,  # a0 = thread ID 1
        0x24030035,  # CancelWakeupThread(1)
        0x0000000c,
        0x0040a021,  # s4 = cleared wakeup count 0
        0x24040063,  # a0 = missing thread ID 99
        0x24030033,  # WakeupThread(99) must return -1
        0x0000000c,
        0x0040a821,  # s5 = -1
        0x00000008,  # jr zero: finish synthetic test
        0x00000000,
    )
    worker = (
        0x24040001,  # a0 = sleeping main thread ID 1
        0x24030034,  # iWakeupThread(1): make main ready, defer reschedule
        0x0000000c,
        0x24040001,
        0x24030033,  # WakeupThread(1): grant credit to ready main
        0x0000000c,
        0x24030023,  # ExitThread: return to main
        0x0000000c,
        0x00000008,
        0x00000000,
    )
    for index, opcode in enumerate(entry):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    for index, opcode in enumerate(worker):
        struct.pack_into("<I", image, 0x1f0 + index * 4, opcode)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 1, 0, 0, 0
    )
    path.write_bytes(image)



def make_thread_release_wait_elf(path):
    """Release a blocked guest twice, including WAITSUSPEND and i variant."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    entry = (
        0x3c040010,  # a0 = 0x00100200 descriptor
        0x34840200,
        0x24030020,  # CreateThread
        0x0000000c,
        0x00408021,  # s0 = worker ID 2
        0x24040002,
        0x24030022,  # StartThread(2): lower priority worker READY
        0x0000000c,
        0x24030032,  # SleepThread(): WAIT, switch to worker
        0x0000000c,
        0x00408821,  # s1 = 1, resumed after ReleaseWaitThread
        0x24030032,  # SleepThread() again
        0x0000000c,
        0x00409021,  # s2 = 1, resumed after Suspend/Release/Resume
        0x24040002,
        0x2403002d,  # ReleaseWaitThread(2) while READY: return -1
        0x0000000c,
        0x00409821,  # s3 = -1
        0x00000008,  # jr zero
        0x00000000,
    )
    worker = (
        0x24040001,
        0x2403002d,  # ReleaseWaitThread(1): WAIT -> READY, preempt
        0x0000000c,
        0x0040a021,  # s4 = returned ID 1
        0x24040001,
        0x24030037,  # SuspendThread(1): WAIT -> WAITSUSPEND
        0x0000000c,
        0x24040001,
        0x2403002e,  # iReleaseWaitThread(1): WAITSUSPEND -> SUSPEND
        0x0000000c,
        0x0040a821,  # s5 = returned ID 1
        0x24040001,
        0x24030039,  # ResumeThread(1): SUSPEND -> READY, preempt
        0x0000000c,
        0x24030023,  # ExitThread if worker is rescheduled later
        0x0000000c,
        0x00000008,
        0x00000000,
    )
    for index, opcode in enumerate(entry):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    for index, opcode in enumerate(worker):
        struct.pack_into("<I", image, 0x1f0 + index * 4, opcode)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 1, 0, 0, 0,
    )
    path.write_bytes(image)


def make_thread_semaphore_release_wait_elf(path):
    """Release a real WaitSema waiter; no stale semaphore queue entry."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    entry = (
        0x3c040010,  # a0 = 0x00100200 thread descriptor
        0x34840200,
        0x24030020,  # CreateThread
        0x0000000c,
        0x00408021,  # s0 = ID 2
        0x3c040010,
        0x34840240,  # a0 = 0x00100240 sema descriptor
        0x24030040,  # CreateSema: initial count 0, maximum 1
        0x0000000c,
        0x00408821,  # s1 = sema ID 1
        0x24040002,
        0x24030022,  # StartThread(2)
        0x0000000c,
        0x24040001,
        0x24030044,  # WaitSema(1), blocks main
        0x0000000c,
        0x00409021,  # s2 = returned sema ID 1
        0x24040001,
        0x3c050010,
        0x34a50280,  # a1 = guest ee_sema_t output
        0x24030047,  # ReferSemaStatus(1)
        0x0000000c,
        0x3c080010,
        0x3508028c,
        0x8d130000,  # lw s3, wait_threads: must be zero
        0x24040001,
        0x24030042,  # SignalSema(1) must increment count, not wake old waiter
        0x0000000c,
        0x24040001,
        0x24030045,  # PollSema(1) succeeds and consumes new count
        0x0000000c,
        0x0040a021,  # s4 = semaphore ID 1
        0x00000008,
        0x00000000,
    )
    worker = (
        0x24040001,
        0x2403002d,  # ReleaseWaitThread(1): removes sema queue entry
        0x0000000c,
        0x24030023,  # ExitThread if ever resumed
        0x0000000c,
        0x00000008,
        0x00000000,
    )
    for index, opcode in enumerate(entry):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    for index, opcode in enumerate(worker):
        struct.pack_into("<I", image, 0x1f0 + index * 4, opcode)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 1, 0, 0, 0,
    )
    struct.pack_into("<6I", image, 0x340, 0, 1, 0, 0, 0, 0)
    path.write_bytes(image)


def make_interrupt_status_encoding_elf(path, negative_service=-0x31):
    """Negative EE thread status/id syscalls must fail outside an IRQ."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    instructions = (
        0x24040001,  # a0 = main thread 1
        0x00002821,  # a1 = null output pointer
        0x24030031,  # positive iReferThreadStatus is permitted
        0x0000000c,
        0x00408021,  # s0 = RUN status (1)
        0x24030000 | (negative_service & 0xffff),  # signed IRQ-only service
        0x0000000c,  # rejected outside a registered IRQ callback
        0x00408821,  # unreachable s1
        0x00000008,
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_thread_suspend_resume_elf(path):
    """Create, start, suspend, resume a real guest EE thread via syscalls."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    instructions = (
        0x3c040010,  # lui a0,0x0010
        0x34840200,  # ori a0,a0,0x0200: thread descriptor
        0x24030020,  # CreateThread
        0x0000000c,
        0x00408021,  # s0 = new thread ID 2
        0x24040002,  # a0 = 2
        0x24030022,  # StartThread, leaving child READY at priority 64
        0x0000000c,
        0x24040002,  # ReferThreadStatus(2, nullptr)
        0x00002821,  # a1 = 0
        0x24030030,
        0x0000000c,
        0x00408821,  # s1 = READY (2)
        0x24040002,
        0x24030037,  # SuspendThread(2) -- retail SDBZ failure
        0x0000000c,
        0x00409021,  # s2 = returned ID 2
        0x24040002,
        0x24030030,  # ReferThreadStatus(2) after suspension
        0x0000000c,
        0x00409821,  # s3 = SUSPEND (8)
        0x24040002,
        0x24030039,  # ResumeThread(2)
        0x0000000c,
        0x0040a021,  # s4 = returned ID 2
        0x24040002,
        0x24030030,  # ReferThreadStatus(2) after resume
        0x0000000c,
        0x0040a821,  # s5 = READY (2)
        0x24040063,
        0x24030037,  # SuspendThread(99): invalid ID, no mutation
        0x0000000c,
        0x0040b021,  # s6 = -1
        0x00000008,  # jr zero to end the synthetic game
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 64, 0, 0, 0
    )
    path.write_bytes(image)


def make_self_suspend_elf(path):
    """Self-suspension is visible until an explicit guest reschedule."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    instructions = (
        0x24040001,  # self ID
        0x24030037,  # SuspendThread(1)
        0x0000000c,
        0x00408021,  # s0 = ID
        0x24040001,
        0x24050000,  # no status buffer
        0x24030030,  # ReferThreadStatus
        0x0000000c,
        0x00408821,  # s1 = SUSPEND
        0x24040000,  # priority zero
        0x2403002b,  # RotateThreadReadyQueue: real scheduler entry
        0x0000000c,
        0x24040001,
        0x24050000,
        0x24030030,
        0x0000000c,
        0x00409021,  # s2 = RUN
        0x00000008,
        0x00000000,
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    path.write_bytes(image)


def make_thread_priority_elf(path):
    """Exercise EE thread-priority return values and per-thread state."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # PT_LOAD descriptor at guest VA 0x00100200.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)

    instructions = (
        0x2403002f,  # GetThreadId
        0x0000000c,
        0x00404021,  # t0 = running ID 1
        0x24040001,  # a0 = main thread ID 1
        0x24050001,  # a1 = priority 1
        0x24030029,  # ChangeThreadPriority
        0x0000000c,
        0x00404821,  # t1 = previous priority 0
        0x24040000,  # a0 = current thread alias 0
        0x24050005,  # a1 = priority 5
        0x24030029,
        0x0000000c,
        0x00405021,  # t2 = previous priority 1
        0x3c040010,  # a0 = 0x00100200
        0x34840200,
        0x24030020,  # CreateThread from priority-64 descriptor
        0x0000000c,
        0x00405821,  # t3 = new dormant ID 2
        0x24040002,  # a0 = dormant thread ID 2
        0x24050082,  # a1 = 130, masked to 2
        0x2403002a,  # iChangeThreadPriority
        0x0000000c,
        0x00406021,  # t4 = old dormant priority 64
        0x24040002,
        0x24050007,  # new priority 7
        0x24030029,
        0x0000000c,
        0x00406821,  # t5 = old priority 2, proves write occurred
        0x24040063,  # a0 = invalid thread ID 99
        0x24050003,
        0x24030029,
        0x0000000c,
        0x00407021,  # t6 = -1
        0x2403002f,  # GetThreadId again, without scheduling child
        0x0000000c,
        0x00407821,  # t7 = running ID 1
        0x00000008,  # JR zero: synthetic exit
        0x00000000,
    )
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + 4 * index, word)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 64, 0, 0, 0
    )
    path.write_bytes(image)


def make_thread_lifecycle_elf(path):
    """Real HLE dispatch: delete/terminate validity, restart and ID reuse."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    instructions = []

    def call(service, arg, saved_register):
        if arg == 0x00100200:
            instructions.extend((0x3c040010, 0x34840200))
        else:
            instructions.append(0x24040000 | arg)
        instructions.extend((0x24050000, 0x24030000 | service, 0x0000000c,
                             0x00400021 | (saved_register << 11)))

    call(0x21, 1, 16)  # self delete fails
    call(0x20, 0x00100200, 17)  # create dormant thread 2
    call(0x25, 2, 18)  # dormant terminate fails
    call(0x22, 2, 19)  # start thread 2 (lower priority)
    call(0x21, 2, 20)  # READY delete fails
    call(0x25, 2, 21)  # terminate READY thread
    call(0x22, 2, 22)  # restart terminated thread
    call(0x25, 2, 23)  # terminate again
    call(0x21, 2, 24)  # delete dormant thread
    call(0x21, 2, 25)  # deleted ID fails
    call(0x20, 0x00100200, 15)  # lowest free ID reused
    instructions.extend((0x00000008, 0x00000000))
    for index, word in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + 4 * index, word)
    struct.pack_into("<2I", image, 0x280, 0x03e00008, 0)
    struct.pack_into("<9I", image, 0x300, 0, 0x00100180, 0x00102000,
                     0x800, 0x0010f000, 64, 0, 0, 0)
    path.write_bytes(image)


def make_get_thread_id_elf(path):
    """GetThreadId must return the running main thread, not a dormant child."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x400 - len(image))))
    # The test's guest thread descriptor is at VA 0x00100200.
    struct.pack_into("<I", image, 52 + 16, 0x300)
    struct.pack_into("<I", image, 52 + 20, 0x300)
    instructions = (
        (0x108, 0x2403002f),  # v1 = GetThreadId
        (0x10c, 0x0000000c),  # syscall
        (0x110, 0x00404021),  # t0 = GetThreadId before CreateThread
        (0x114, 0x3c040010),  # a0 = 0x00100200
        (0x118, 0x34840200),
        (0x11c, 0x24030020),  # v1 = CreateThread
        (0x120, 0x0000000c),  # syscall
        (0x124, 0x00404821),  # t1 = created dormant thread ID
        (0x128, 0x2403002f),  # v1 = GetThreadId again
        (0x12c, 0x0000000c),  # syscall (a0 still holds 0x00100200)
        (0x130, 0x00405021),  # t2 = GetThreadId after creation
        (0x134, 0x00000008),  # jr zero: finish bounded synthetic test
        (0x138, 0x00000000),
    )
    for offset, word in instructions:
        struct.pack_into("<I", image, offset, word)
    struct.pack_into(
        "<9I", image, 0x300,
        0, 0x001000f0, 0x00102000, 0x800,
        0x0010f000, 64, 0, 0, 0,
    )
    path.write_bytes(image)


def make_semaphore_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    if len(image) < 0x1a0:
        image.extend(bytes(0x1a0 - len(image)))
    # PT_LOAD includes the guest ee_sema_t at virtual address 0x00100080.
    struct.pack_into("<I", image, 52 + 16, 0xa0)
    struct.pack_into("<I", image, 52 + 20, 0xa0)
    for offset, value in (
        (0x108, 0x3c040010), # lui a0,0x0010
        (0x10c, 0x34840080), # ori a0,a0,0x0080
        (0x110, 0x24030040), # v1 = CreateSema
        (0x114, 0x0000000c), # syscall
        (0x118, 0x03e00008), # jr ra
        (0x11c, 0x00000000), # nop
    ):
        struct.pack_into("<I", image, offset, value)
    struct.pack_into("<IIIIII", image, 0x180, 0, 2, 1, 0, 0, 0)
    path.write_bytes(image)


def make_semaphore_operations_elf(path, blocking_wait=False,
                                  invalid_status=False):
    """Exercise the stateful semaphore kernel ABI through guest MIPS syscalls."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x500 - len(image))))
    # Descriptor at guest 0x00100300, status block at 0x00100320.
    # The ELF PT_LOAD file offset is 0x100; increase the segment sizes.
    struct.pack_into("<I", image, 52 + 16, 0x400)
    struct.pack_into("<I", image, 52 + 20, 0x400)
    struct.pack_into(
        "<6I", image, 0x400,
        0, 2, 0 if blocking_wait else 1, 0, 0x1234, 0x5678,
    )
    opcodes = []

    def emit(*words):
        opcodes.extend(words)

    def li(register, value):
        if value <= 0x7fff:
            emit(0x24000000 | (register << 16) | value)
        else:
            emit(
                0x3c000000 | (register << 16) | ((value >> 16) & 0xffff),
                0x34000000 | (register << 21) | (register << 16) |
                (value & 0xffff),
            )

    def syscall(service, register=None):
        li(3, service)             # v1 selects EE kernel syscall
        emit(0x0000000c)
        if register is not None:
            # addu destination, v0, zero
            emit((2 << 21) | (register << 11) | 0x21)

    li(4, 0x00100300)
    syscall(0x40, 8)               # t0 = CreateSema() -> 1
    li(4, 1)                       # all remaining operations use ID 1
    syscall(0x44, 9)               # t1 = WaitSema() -> 1
    if not blocking_wait:
        syscall(0x45, 10)          # t2 = PollSema() -> -1
        syscall(0x46, 11)          # t3 = iPollSema() -> -1
        syscall(0x42, 12)          # t4 = SignalSema() -> 1
        syscall(0x43, 13)          # t5 = iSignalSema() -> 1
        syscall(0x42, 14)          # t6 = signal at max -> -1
        syscall(0x44, 15)          # t7 = WaitSema() -> 1
        syscall(0x44, 16)          # s0 = WaitSema() -> 1
        li(5, 0x7ffffffc if invalid_status else 0x00100320)
        syscall(0x47, 17)          # s1 = ReferSemaStatus() -> 1
        if not invalid_status:
            syscall(0x48, 18)      # s2 = iReferSemaStatus() -> 1
            emit(
                0x8ca00000 | (19 << 16) | 0,   # lw s3,  0(a1) count
                0x8ca00000 | (20 << 16) | 4,   # lw s4,  4(a1) max
                0x8ca00000 | (21 << 16) | 8,   # lw s5,  8(a1) init
                0x8ca00000 | (22 << 16) | 12,  # lw s6, 12(a1) waiters
                0x8ca00000 | (23 << 16) | 16,  # lw s7, 16(a1) attr
                0x8ca00000 | (24 << 16) | 20,  # lw t8, 20(a1) option
            )
            syscall(0x41, 25)      # t9 = DeleteSema() -> 1
            syscall(0x44)          # invalid deleted ID -> -1
    emit(0x00000008, 0x00000000)  # jr zero; delay-slot nop
    assert 0x108 + 4 * len(opcodes) < 0x400
    for index, opcode in enumerate(opcodes):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_set_syscall_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    for offset, value in (
        (0x108, 0x24040083), # a0 = 0x83
        (0x10c, 0x3c050010), # lui a1,0x0010
        (0x110, 0x34a50030), # handler = 0x00100030
        (0x114, 0x24030074), # v1 = SetSyscall
        (0x118, 0x0000000c), # syscall
        (0x11c, 0x03e00008), # jr ra
        (0x120, 0x00000000), # delay slot
    ):
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_eret_target_elf(path, status):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x300 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x200)
    struct.pack_into("<I", image, 52 + 20, 0x200)

    # Both alternate targets are reachable static basic blocks, but the
    # never-taken BNE instructions do not actually take either branch.
    # This tests the real MTC0+ERET instructions in LLVM-generated code.
    instructions = (
        (0x108, 0x1400001d),  # bne zero, zero, ErrorEPC 0x00100080
        (0x10c, 0x00000000),
        (0x110, 0x1400002b),  # bne zero, zero, EPC 0x001000c0
        (0x114, 0x00000000),
        (0x118, 0x24080000 | status),  # t0 = Status (EXL or ERL)
        (0x11c, 0x40886000),  # mtc0 t0, Status
        (0x120, 0x3c090010),  # lui t1, 0x0010
        (0x124, 0x35290080),  # ErrorEPC = 0x00100080
        (0x128, 0x4089f000),  # mtc0 t1, ErrorEPC
        (0x12c, 0x3c0a0010),  # lui t2, 0x0010
        (0x130, 0x354a00c0),  # EPC = 0x001000c0
        (0x134, 0x408a7000),  # mtc0 t2, EPC
        (0x138, 0x42000018),  # ERET
        (0x13c, 0x240f0001),  # must never execute
        (0x180, 0x400b6000),  # mfc0 t3, Status at ErrorEPC
        (0x184, 0x400cf000),  # mfc0 t4, ErrorEPC
        (0x188, 0x400d7000),  # mfc0 t5, EPC
        (0x18c, 0x00000008),  # jr zero to finish
        (0x190, 0x00000000),
        (0x1c0, 0x400b6000),  # mfc0 t3, Status at EPC
        (0x1c4, 0x400cf000),  # mfc0 t4, ErrorEPC
        (0x1c8, 0x400d7000),  # mfc0 t5, EPC
        (0x1cc, 0x00000008),
        (0x1d0, 0x00000000),
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_overlapping_entry_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Call at 0x00100050 first: its translated SYSCALL block is
    # followed by a return block owned at 0x00100058. Then compute a
    # runtime JALR target of 0x00100054, the *interior* SYSCALL PC.
    # Translating that entry must retain the existing owner of 0x100058,
    # and both calls must execute genuine native MIPS/SYSCALL behavior.
    # Including the syscall return in the target computation prevents
    # static call-target discovery from splitting the first function.
    instructions = (
        (0x108, 0x0c040014),  # jal 0x00100050
        (0x10c, 0x00000000),  # delay slot
        (0x110, 0x00408021),  # addu s0, v0, zero
        (0x114, 0x24030064),  # addiu v1, zero, FlushCache
        (0x118, 0x3c080010),  # lui t0, 0x0010
        (0x11c, 0x25080054),  # addiu t0, t0, 0x54
        (0x120, 0x01024021),  # addu t0, t0, v0
        (0x124, 0x0100f809),  # jalr ra, t0
        (0x128, 0x00000000),  # delay slot
        (0x12c, 0x00404821),  # addu t1, v0, zero
        (0x130, 0x00000008),  # jr zero (known stop)
        (0x134, 0x00000000),  # delay slot
        (0x150, 0x24030064),  # addiu v1, zero, FlushCache
        (0x154, 0x0000000c),  # SYSCALL (second entry)
        (0x158, 0x03e00008),  # jr ra (shared return block)
        (0x15c, 0x00000000),  # delay slot
    )
    for offset, opcode in instructions:
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_interrupt_mask_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x500 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x400)
    struct.pack_into("<I", image, 52 + 20, 0x400)

    # Exercise both regular and interrupt-context syscall variants.
    # The return value is 1 only if a hardware mask bit changes.
    # Read actual EE INTC_MASK and DMAC D_STAT afterward to verify
    # the syscalls affect emulated guest-visible MMIO state.
    calls = (
        (11, 0x14, 8),   # EnableIntc -> 1
        (11, 0x14, 9),   # EnableIntc again -> 0
        (11, 0x15, 10),  # DisableIntc -> 1
        (11, 0x15, 11),  # DisableIntc again -> 0
        (11, 0x1a, 12),  # iEnableIntc -> 1
        (11, 0x1b, 13),  # iDisableIntc -> 1
        (2,  0x16, 14),  # EnableDmac -> 1
        (2,  0x16, 15),  # EnableDmac again -> 0
        (2,  0x17, 16),  # DisableDmac -> 1
        (2,  0x17, 17),  # DisableDmac again -> 0
        (2,  0x1c, 18),  # iEnableDmac -> 1
        (2,  0x1d, 19),  # iDisableDmac -> 1
        (11, 0x14, 20),  # finally enable INTC
        (2,  0x16, 21),  # finally enable DMAC
    )
    words = []
    for cause, service, dst in calls:
        words.extend((
            0x24040000 | cause,       # addiu a0,zero,cause
            0x24030000 | service,     # addiu v1,zero,service
            0x0000000c,              # syscall
            0x00400021 | (dst << 11),  # addu saved_register,v0,zero
        ))
    words.extend((
        0x3c1a1000,  # lui k0,0x1000
        0x375af010,  # ori k0,k0,INTC_MASK low
        0x8f560000,  # lw s6,0(k0)
        0x3c1a1000,  # lui k0,0x1000
        0x375ae010,  # ori k0,k0,D_STAT low
        0x8f570000,  # lw s7,0(k0)
        0x00000008,  # jr zero
        0x00000000,
    ))
    for index, opcode in enumerate(words):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_interrupt_handlers_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Registers: t0/t1/t2 receive global IDs 1/2/3; t3/t4/t5
    # receive remaining-handler counts. s0 checks remove-missing.
    # Exercise the guest syscall trap and resume, not just a decoder.
    instructions = (
        0x3c1c1234,  # lui gp,0x1234
        0x2404000b,  # a0 = INTC cause 11
        0x3c050010,  # a1 = 0x00100090
        0x34a50090,
        0x2406ffff,  # a2 = -1 (append)
        0x24070066,  # a3 = callback argument
        0x24030010,  # AddIntcHandler
        0x0000000c,
        0x00404021,  # t0 = handler ID 1
        0x2404000b,
        0x3c050010,
        0x34a500a0,  # a1 = 0x001000a0
        0x24060000,  # a2 = 0 (insert before first)
        0x24070077,
        0x24030010,  # AddIntcHandler
        0x0000000c,
        0x00404821,  # t1 = handler ID 2
        0x24040002,  # a0 = DMAC channel 2
        0x3c050010,
        0x34a500b0,  # a1 = 0x001000b0
        0x2406ffff,
        0x24070088,
        0x24030012,  # AddDmacHandler (shared global IDs)
        0x0000000c,
        0x00405021,  # t2 = handler ID 3
        0x2404000b,
        0x01202821,  # a1 = t1 (ID 2)
        0x24030011,  # RemoveIntcHandler
        0x0000000c,
        0x00405821,  # t3 = remaining 1
        0x2404000b,
        0x24050063,  # remove unknown ID 99
        0x24030011,
        0x0000000c,
        0x00408021,  # s0 = remaining 1, not success/failure
        0x2404000b,
        0x01002821,  # a1 = t0 (ID 1)
        0x24030011,
        0x0000000c,
        0x00406021,  # t4 = remaining 0
        0x24040002,
        0x01402821,  # a1 = t2 (ID 3)
        0x24030013,  # RemoveDmacHandler
        0x0000000c,
        0x00406821,  # t5 = remaining 0
        0x00000008,  # jr zero: finish through native dispatcher
        0x00000000,
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_out_of_range_set_syscall_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x200 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x100)
    struct.pack_into("<I", image, 52 + 20, 0x100)

    # Install a valid service, then attempt out-of-range 0x12C.
    # Both valid entry 0x83 and the first word beyond the table
    # must remain intact. The second bad index tests overflow.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (KSEG0 base)
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50050,  # ori a1, a1, 0x0050
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d481a0c,  # lw t0, 0x1A0C(t2): original entry
        0x2404012c,  # addiu a0, zero, 0x12C
        0x3c058007,  # lui a1, 0x8007
        0x34a56488,  # ori a1, a1, 0x6488
        0x24020007,  # addiu v0, zero, 7 (overwritten)
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall: out-of-range index
        0x00404821,  # addu t1, v0, zero: zero return
        0x8d4b1a0c,  # lw t3, 0x1A0C(t2): valid entry still set
        0x8d4c1c00,  # lw t4, 0x1C00(t2): first word after table
        0x3c04ffff,  # lui a0, 0xFFFF
        0x3484ffff,  # ori a0, a0, 0xFFFF
        0x24020009,  # addiu v0, zero, 9 (overwritten)
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall: 0xFFFFFFFF
        0x00406821,  # addu t5, v0, zero
        0x8d4e1a0c,  # lw t6, 0x1A0C(t2): preserved
        0x8d4f1c00,  # lw t7, 0x1C00(t2): preserved
        0x00000008,  # jr zero
        0x00000000,  # nop
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_guest_syscall_table_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # Keep the complete synthetic program and both handler addresses
    # inside the single executable PT_LOAD region.
    image.extend(bytes(max(0, 0x1a0 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0xa0)
    struct.pack_into("<I", image, 52 + 20, 0xa0)

    # The HLE syscall table lives in physical RAM at 0x1800, and the
    # KSEG0 mirror begins at 0x80001800. Register 0x83 then 0x5A,
    # verify that their entries occupy separate 32-bit slots, replace
    # 0x83, and check that the adjacent, unwritten slot remains zero.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (KSEG0 base)
        0x8d481a0c,  # lw t0, 0x1A0C(t2): initially zero
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50050,  # ori a1, a1, 0x0050
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d491a0c,  # lw t1, 0x1A0C(t2): 0x00100050
        0x8c0b1a0c,  # lw t3, 0x1A0C(zero): physical alias
        0x2404005a,  # addiu a0, zero, 0x5A
        0x3c050010,  # lui a1, 0x0010
        0x34a50030,  # ori a1, a1, 0x0030
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d4c1968,  # lw t4, 0x1968(t2): 0x00100030
        0x8d4d1a0c,  # lw t5, 0x1A0C(t2): 0x00100050
        0x24040083,  # addiu a0, zero, 0x83
        0x3c050010,  # lui a1, 0x0010
        0x34a50030,  # ori a1, a1, 0x0030
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d4e1a0c,  # lw t6, 0x1A0C(t2): overwritten 0x00100030
        0x8d4f1a10,  # lw t7, 0x1A10(t2): unwritten 0x84, zero
        0x00000008,  # jr zero: stop after returning to caller
        0x00000000,  # nop (delay slot)
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_registered_callback_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x180 - len(image)))
    # Provide 0x80 file-backed bytes beginning at the PT_LOAD's 0x100 offset.
    struct.pack_into("<I", image, 52 + 16, 0x80)
    struct.pack_into("<I", image, 52 + 20, 0x80)
    # SetSyscall(0x83, 0x00100050), invoke it with a0=7, then preserve
    # the guest callback's return value in t0.
    for offset, value in (
        (0x108, 0x24040083),  # addiu a0, zero, 0x83
        (0x10c, 0x3c050010),  # lui a1, 0x10
        (0x110, 0x34a50050),  # ori a1, a1, 0x50
        (0x114, 0x24030074),  # addiu v1, zero, SetSyscall
        (0x118, 0x0000000c),  # syscall
        (0x11c, 0x24040007),  # addiu a0, zero, 7
        (0x120, 0x24030083),  # addiu v1, zero, 0x83
        (0x124, 0x0000000c),  # syscall -> installed callback
        (0x128, 0x00404021),  # addu t0, v0, zero
        (0x12c, 0x00000008),  # jr zero
        (0x130, 0x00000000),  # nop delay slot
        (0x150, 0x24820005),  # addiu v0, a0, 5
        (0x154, 0x03e00008),  # jr ra -> synthetic callback sentinel
        (0x158, 0x00000000),  # nop delay slot
    ):
        struct.pack_into("<I", image, offset, value)
    path.write_bytes(image)


def make_flush_cache_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # Two recognized cache services followed by an unsupported
    # service. A missing --hle-startup must reject the very first
    # service; enabled HLE must resume past both and stop at 0x65.
    instructions = (
        0x2402007b,  # addiu v0, zero, 123: prove return overwrites v0
        0x24030064,  # addiu v1, zero, 0x64 (FlushCache)
        0x0000000c,  # syscall
        0x00404021,  # addu t0, v0, zero: expected 0
        0x2402007c,  # addiu v0, zero, 124
        0x24030068,  # addiu v1, zero, 0x68 (iFlushCache)
        0x0000000c,  # syscall
        0x00404821,  # addu t1, v0, zero: expected 0
        0x24030065,  # addiu v1, zero, unknown 0x65
        0x0000000c,  # syscall: must remain unsupported
        0x240a0001,  # addiu t2, zero, 1: never executed
        0x00000008,  # jr zero
        0x00000000,  # delay slot
    )
    for index, opcode in enumerate(instructions):
        struct.pack_into("<I", image, 0x108 + index * 4, opcode)
    path.write_bytes(image)


def make_kernel_ram_handler_elf(path, invoke=False):
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(max(0, 0x180 - len(image))))
    struct.pack_into("<I", image, 52 + 16, 0x80)
    struct.pack_into("<I", image, 52 + 20, 0x80)
    # Store a tiny guest code word at KSEG0 0x80076000, register that
    # direct RAM address for syscall 0x5B, and read back the kernel
    # table through KSEG0. Invoke it or clear the registration.
    instructions = (
        0x3c0a8000,  # lui t2, 0x8000 (kernel table alias)
        0x3c0b8007,  # lui t3, 0x8007
        0x356b6000,  # ori t3, t3, 0x6000
        0x3c0c03e0,  # lui t4, 0x03e0 (JR RA opcode)
        0x358c0008,  # ori t4, t4, 8
        0xad6c0000,  # sw t4, 0(t3), writing RAM JR $ra
        0x3c0c2402,  # lui t4, 0x2402 (ADDIU v0,zero,42 opcode)
        0x358c002a,  # ori t4, t4, 42
        0xad6c0004,  # sw t4, 4(t3), JR RA delay slot writes v0=42
        0x2404005b,  # addiu a0, zero, 0x5B
        0x01602821,  # addu a1, t3, zero: 0x80076000
        0x24030074,  # addiu v1, zero, SetSyscall
        0x0000000c,  # syscall
        0x8d48196c,  # lw t0, 0x196c(t2): registered pointer
        0x8d690000,  # lw t1, 0(t3): actual guest RAM instruction
    )
    end = (
        (
            0x2403005b,  # addiu v1, zero, 0x5B
            0x0000000c,  # syscall: execute the RAM callback
            0x00406821,  # addu t5, v0, zero: must observe 42
        ) if invoke else (
            0x2404005b,  # addiu a0, zero, 0x5B
            0x00002821,  # addu a1, zero, zero: unregister
            0x24030074,  # addiu v1, zero, SetSyscall
            0x0000000c,  # syscall
            0x8d4d196c,  # lw t5, 0x196c(t2): cleared table entry
        )
    )
    for i, opcode in enumerate(instructions + end + (0x00000008, 0x00000000)):
        struct.pack_into("<I", image, 0x108 + 4 * i, opcode)
    path.write_bytes(image)


def make_kernel_ram_probe_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    for offset, opcode in (
        (0x108, 0x3c048000),  # lui a0, 0x8000
        (0x10c, 0x8c820000),  # lw v0, 0(a0)
        (0x110, 0x00404021),  # addu t0, v0, zero
        (0x114, 0x00000008),  # jr zero
        (0x118, 0x00000000),  # nop
    ):
        struct.pack_into("<I", image, offset, opcode)
    path.write_bytes(image)


def make_counting_loop_elf(path):
    """Count while branching backward within a single native CFG."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    struct.pack_into("<I", image, 0x108, 0x25080001)  # addiu t0,t0,1
    struct.pack_into("<I", image, 0x10c, 0x1000fffe)  # beq zero,zero,-2
    struct.pack_into("<I", image, 0x110, 0x00000000)  # delay-slot nop
    path.write_bytes(image)


def make_unsupported_instruction_elf(path):
    """An invalid COP3 opcode must fail closed with its exact guest bytes."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    struct.pack_into("<I", image, 0x108, 0x4c000000)  # COP3 is not on R5900
    struct.pack_into("<I", image, 0x10c, 0x03e00008)  # jr ra (unreached)
    path.write_bytes(image)


def make_vu0_indexed_native_elf(path):
    """Exercise real COP2 transfer and indexed quadword macros in one CFG."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    program = [
        0x3c081234,  # lui t0,0x1234
        0x35085678,  # ori t0,t0,0x5678
        0x48a81800,  # qmtc2 t0,vf3
        0x24090000,  # addiu t1,zero,0
        0x48c90800,  # ctc2 t1,vi1
        0x4b011b7d,  # vsqi.x vf3,(vi1++)
        0x48c91000,  # ctc2 t1,vi2
        0x4b02137c,  # vlqi.x vf2,(vi2++)
        0x482a1000,  # qmfc2 t2,vf2
        0x484b0800,  # cfc2 t3,vi1
        0x484c1000,  # cfc2 t4,vi2
        0x0000000d,  # break after all guest side effects
    ]
    for index, word in enumerate(program):
        struct.pack_into("<I", image, 0x108 + 4 * index, word)
    path.write_bytes(image)


def make_idle_timer_wakeup_elf(path):
    """The only runnable thread sleeps until its timer IRQ wakes it."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    image.extend(bytes(0x200 - len(image)))
    struct.pack_into("<II", image, 52 + 16, 0x100, 0x100)
    entry = (
        0x24040009,  # a0 = Timer 0 INTC cause
        0x3c050010,  # a1 = 0x00100080 handler
        0x34a50080,
        0x2406ffff,  # append handler
        0x24030010,  # AddIntcHandler
        0x0000000c,
        0x24030014,  # EnableIntc
        0x0000000c,
        0x3c081000,  # t0 = Timer 0 MMIO
        0x24090400,  # compare at 1024 bus ticks
        0xad090020,
        0x24090180,  # enable counter and compare interrupt
        0xad090010,
        0x24030032,  # SleepThread
        0x0000000c,
        0x00408021,  # s0 = SleepThread return value
        0x0000000d,  # BREAK after it resumes
    )
    handler = (
        0x3c081000,
        0xad000010,  # stop Timer 0
        0x2403ffd1,  # _iGetThreadId sees the idle EE
        0x0000000c,
        0x24040001,  # a0 = sleeping thread ID
        0x2403ffcc,  # iWakeupThread
        0x0000000c,
        0x03e00008,
        0x00000000,
    )
    for offset, instructions in ((0x108, entry), (0x180, handler)):
        struct.pack_into(f"<{len(instructions)}I", image, offset, *instructions)
    path.write_bytes(image)


def make_ee_timer_poll_elf(path):
    """Exercise guest-programmed Timer 0 COUNT advancing inside native CFG."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    # 0x10000010 = T0_MODE; 0x10000000 = T0_COUNT.
    # Enable bus-clock counting, then poll >= 32 entirely within a
    # translated native function. A post-dispatch-only clock cannot pass.
    program = [
        0x3c081000,  # lui t0,0x1000
        0x24090080,  # addiu t1,zero,0x80 (CUE enable)
        0xad090010,  # sw t1,0x10(t0)
        0x8d0a0000,  # lw t2,0(t0)
        0x240b0000,  # addiu t3,zero,0
        0x256b0001,  # poll: addiu t3,t3,1
        0x8d0a0000,  # lw t2,0(t0)
        0x29410020,  # slti at,t2,32
        0x1420fffc,  # bne at,zero,poll
        0x00000000,  # delay slot
        0xad000010,  # sw zero,0x10(t0): freeze counter
        0x8d100000,  # lw s0,0(t0): first frozen reading
        0x8d110000,  # lw s1,0(t0): second frozen reading
        0x0000000d,  # BREAK: expected terminal trap
    ]
    for index, word in enumerate(program):
        struct.pack_into("<I", image, 0x108 + index * 4, word)
    path.write_bytes(image)


def make_iop_idle_batch_elf(path):
    """Accumulate IOP debt in one native CFG, then dispatch twice."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    words = (
        0x24080000,  # addiu t0,zero,0
        0x25080001,  # loop: addiu t0,t0,1
        0x29012000,  # slti at,t0,8192
        0x1420fffd,  # bne at,zero,loop
        0x00000000,  # delay slot
        0x3c091000,  # lui t1,0x1000
        0x3529c000,  # ori t1,t1,0xc000
        0x240a0184,  # addiu t2,zero,0x184: arm HLE SIF0
        0xad2a0000,  # sw t2,0(t1)
        0x0c04000e,  # jal 0x00100038
        0x00000000,
        0x0000000d,  # break
    )
    struct.pack_into(f"<{len(words)}I", image, 0x108, *words)
    struct.pack_into("<II", image, 0x138, 0x03e00008, 0)
    path.write_bytes(image)


def make_spin_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # A branch to itself remains inside a single translated CFG. The
    # dispatcher count cannot interrupt this native loop.
    struct.pack_into("<I", image, 0x108, 0x1000ffff)  # beq zero,zero,-1
    struct.pack_into("<I", image, 0x10c, 0x00000000)  # delay-slot nop
    path.write_bytes(image)


def make_repeated_dispatch_elf(path):
    """A recursive JAL exits through the dispatcher on every guest call."""
    make_elf(path)
    image = bytearray(path.read_bytes())
    struct.pack_into("<I", image, 0x108, 0x0c040002)  # jal 0x00100008
    struct.pack_into("<I", image, 0x10c, 0x00000000)  # delay slot
    path.write_bytes(image)


def make_call_elf(path):
    make_elf(path)
    image = bytearray(path.read_bytes())
    # JAL 0x00100030; NOP; JR $zero; NOP. The callee returns with JR $ra.
    struct.pack_into("<I", image, 0x108, 0x0c04000c)
    struct.pack_into("<I", image, 0x10c, 0x00000000)
    struct.pack_into("<I", image, 0x110, 0x00000008)
    struct.pack_into("<I", image, 0x114, 0x00000000)
    struct.pack_into("<I", image, 0x130, 0x03e00008)
    struct.pack_into("<I", image, 0x134, 0x00000000)
    path.write_bytes(image)


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(
            "usage: test_game_runner.py source_root build_dir runner_executable [shard_0_to_3]"
        )
    shard = int(sys.argv[4]) if len(sys.argv) == 5 else None
    if shard is not None and shard not in range(4):
        raise SystemExit("game runner shard must be between 0 and 3")
    source = Path(sys.argv[1]).resolve()
    build = Path(sys.argv[2]).resolve()
    runner = Path(sys.argv[3]).resolve()
    # Multi-config CMake builds must select build/Release rather than a
    # stale build-root executable. Exercise the path selection on CI too.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "run_game_driver", source / "tools" / "run_game.py"
    )
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    with tempfile.TemporaryDirectory(prefix="ee-multiconfig-lookup-") as tmp:
        fake_build = Path(tmp)
        (fake_build / "CMakeCache.txt").write_text(
            "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n", encoding="utf-8"
        )
        suffix = ".exe" if sys.platform == "win32" else ""
        old = fake_build / ("ee_recomp" + suffix)
        preferred = fake_build / "Release" / ("ee_recomp" + suffix)
        preferred.parent.mkdir()
        old.write_bytes(b"stale")
        preferred.write_bytes(b"current")
        assert driver.native_executable(fake_build, "ee_recomp") == preferred
        try:
            driver.native_executable(fake_build, "ee_recomp", "Debug")
        except ValueError:
            pass
        else:
            raise AssertionError("multi-config lookup used a stale root binary")


    help_result = subprocess.run(
        [str(runner), "--help"], capture_output=True, text=True
    )
    assert help_result.returncode == 0, help_result.stderr
    assert "--max-dispatches=N" in help_result.stdout
    # Requesting a Windows PDB on another host must fail rather than
    # pretending a useful symbols file was generated.
    if sys.platform != "win32":
        denied = subprocess.run(
            [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(source / "CMakeLists.txt"), "--pdb",
            ],
            capture_output=True, text=True, check=False,
        )
        assert denied.returncode != 0, denied
        assert "--pdb requires a Windows" in denied.stderr, denied.stderr
        link_denied = subprocess.run(
            [
                sys.executable, str(source / "tools" / "link_native_objects.py"),
                "missing.json", "missing-objects", "missing.so", "--pdb",
            ],
            capture_output=True, text=True, check=False,
        )
        assert link_denied.returncode != 0, link_denied
        assert "--pdb requires a Windows" in link_denied.stderr, link_denied.stderr

    assert "NOT guest instructions" in help_result.stdout

    if shard is None or shard == 0:
        with tempfile.TemporaryDirectory(prefix="ee-native-game-runner-") as tmp:
            root = Path(tmp)
            elf = root / "minimal-mips.elf"
            make_elf(elf)

            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--trace",
                    "--kseg-ram",
                    "--report", str(root / "first_boot.json"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            # The guest JR $ra returns outside compiled code. The runner must
            # report an incomplete execution, not falsely declare a PS2 boot.
            assert result.returncode != 0, result.stdout + result.stderr
            assert "EE ELF entry 0x00100008" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "PT_LOAD 0x00100000" in result.stdout
            assert "EE stopped: dispatched=" in result.stdout
            assert "dispatch=1 pc=0x00100008" in result.stdout
            assert "guest left compiled code" in result.stderr or (
                "guest blocked" in result.stderr
            ), result.stdout + result.stderr
            report = json.loads((root / "first_boot.json").read_text(encoding="utf-8"))
            assert report["schema"] == "ee-game-run-report-v1", report
            assert report["snapshot_kind"] == "stopped", report
            assert report["entry_pc"] == 0x00100008, report
            assert report["dispatches"] >= 1, report
            assert report["guest_instruction_attempts"] >= report["dispatches"], report
            assert report["history"][0]["from_pc"] == 0x00100008, report
            assert report["history"][-1]["dispatch"] == report["dispatches"], report
            assert len(report["gpr_lo"]) == 32, report
            assert (root / "out" / "ir" / "ee_functions.json").is_file()
            # Buffered trace must retain dispatch lines and terminal guest
            # state, including on the incomplete guest-return exit path.
            buffered_report = root / "buffered_boot.json"
            trace_env = dict(os.environ, SDBZ_TRACE_BUFFERED="1")
            buffered = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"), "--max-dispatches=8",
                    "--trace", "--kseg-ram", "--no-recompile",
                    "--report", str(buffered_report),
                ],
                env=trace_env, capture_output=True, text=True, check=False,
            )
            assert buffered.returncode == result.returncode, buffered
            assert "EE trace output: buffered" in buffered.stdout, buffered.stdout
            assert [line for line in buffered.stdout.splitlines()
                    if line.startswith("dispatch=")] == [
                        line for line in result.stdout.splitlines()
                        if line.startswith("dispatch=")]
            buffered_state = json.loads(buffered_report.read_text(encoding="utf-8"))
            for field in ("final_pc", "dispatches", "exit_reason", "dispatch_status",
                          "guest_instruction_attempts", "gpr_lo", "history"):
                assert buffered_state[field] == report[field], field

            # Cycle-model prerequisite: an ordered trace of native-executed
            # instruction PCs, without pretending attempt numbers are EE cycles.
            # BEQL is NOT taken: its delay slot at 0x00100010 is annulled.
            trace_elf = root / "branch-likely-trace.elf"
            make_elf(trace_elf)
            trace_image = bytearray(trace_elf.read_bytes())
            for index, word in enumerate((
                0x24080001,  # ADDIU t0, zero, 1
                0x51000001,  # BEQL t0, zero, +1 (not taken)
                0x24090005,  # annulled ADDIU t1, zero, 5
                0x24020009,  # ADDIU v0, zero, 9
                0x03e00008,  # JR ra
                0x00000000,  # architectural delay slot
            )):
                struct.pack_into("<I", trace_image, 0x108 + index * 4, word)
            trace_elf.write_bytes(trace_image)
            trace_work = root / "instruction-trace-work"
            trace_path = root / "instruction-trace.csv"
            trace_report_path = root / "instruction-trace-report.json"
            trace_cmd = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(trace_elf), "--build-dir", str(build),
                "--work-dir", str(trace_work), "--max-dispatches=8",
                "--kseg-ram", "--report", str(trace_report_path),
            ]
            trace_result = subprocess.run(
                trace_cmd + [
                    "--trace-instructions", str(trace_path),
                    "--trace-instructions-window", "0x00100008:0x0010001c",
                ], capture_output=True, text=True, check=False,
            )
            assert trace_result.returncode == 3, (
                trace_result.stdout + trace_result.stderr
            )
            assert "NO cycle timestamps" in trace_result.stdout, trace_result.stdout
            trace_lines = trace_path.read_text(encoding="utf-8").splitlines()
            assert trace_lines == [
                "attempt,pc,opcode_word,kind,reference_slot,hle_service",
                "1,0x00100008,0x24080001,0,1,0",
                "2,0x0010000c,0x51000001,0,2,0",
                "2,0x00100010,0x24090005,1,3,0",
                "3,0x00100014,0x24020009,0,4,0",
                "4,0x00100018,0x03e00008,0,5,0",
                "5,0x0010001c,0x00000000,0,6,0",
                "# truncated=false range=0x00100008:0x0010001c",
            ], trace_lines
            traced_state = json.loads(trace_report_path.read_text(encoding="utf-8"))
            baseline_report = root / "instruction-baseline-report.json"
            baseline_result = subprocess.run(
                trace_cmd[:-1] + [str(baseline_report), "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert baseline_result.returncode == trace_result.returncode, (
                baseline_result.stdout + baseline_result.stderr
            )
            baseline_state = json.loads(baseline_report.read_text(encoding="utf-8"))
            for field in (
                "entry_pc", "final_pc", "dispatches", "exit_reason", "exit_name",
                "dispatch_status", "guest_instruction_attempts", "gpr_lo",
            ):
                assert traced_state[field] == baseline_state[field], field
            assert traced_state["guest_instruction_attempts"] == 5, traced_state
            assert traced_state["gpr_lo"][9] == 0, traced_state

            # A genuine infinite backward branch fills exactly 65,536 samples.
            # The overflow marker must be explicit; it must not fabricate
            # interrupt events or silently truncate a purported complete trace.
            loop_elf = root / "instruction-loop.elf"
            make_elf(loop_elf)
            loop_image = bytearray(loop_elf.read_bytes())
            struct.pack_into("<II", loop_image, 0x108,
                             0x1000ffff, 0x25080001)  # BEQ self; ADDIU t0,t0,1
            loop_elf.write_bytes(loop_image)
            loop_trace_path = root / "loop-instructions.csv"
            loop_result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(loop_elf), "--build-dir", str(build),
                    "--work-dir", str(root / "instruction-loop-work"),
                    "--max-dispatches=8", "--loop-budget=40000",
                    "--kseg-ram", "--trace-instructions", str(loop_trace_path),
                    "--trace-instructions-window", "0x00100008:0x0010000c",
                ], capture_output=True, text=True, check=False,
            )
            assert loop_result.returncode == 3, (
                loop_result.stdout + loop_result.stderr
            )
            assert "LOOP_BUDGET(5)" in loop_result.stdout, loop_result.stdout
            assert "guest-instruction attempts: 80000" in loop_result.stdout, (
                loop_result.stdout + loop_result.stderr
            )
            loop_lines = loop_trace_path.read_text(encoding="utf-8").splitlines()
            assert len(loop_lines) == 65538, len(loop_lines)
            assert loop_lines[1] == "1,0x00100008,0x1000ffff,0,1,0", loop_lines[1]
            assert loop_lines[-2] == "65536,0x0010000c,0x25080001,0,65536,0", loop_lines[-2]
            assert loop_lines[-1] == (
                "# truncated=true range=0x00100008:0x0010000c"
            ), loop_lines[-1]



            # Probe-only time-paced JSON snapshots must preserve the exact
            # one-dispatch host slicing, guest state and terminal report.
            paced_report_path = root / "first_boot_paced.json"
            paced_environment = dict(os.environ)
            paced_environment["SDBZ_REPORT_SNAPSHOT_MS"] = "1000"
            paced_result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8", "--trace", "--kseg-ram",
                    "--no-recompile", "--report", str(paced_report_path),
                ],
                capture_output=True, text=True, check=False,
                env=paced_environment,
            )
            assert paced_result.returncode == result.returncode, (
                paced_result.stdout + paced_result.stderr
            )
            assert "EE report snapshots: every 1000 ms" in paced_result.stderr, (
                paced_result.stdout + paced_result.stderr
            )
            paced_report = json.loads(
                paced_report_path.read_text(encoding="utf-8")
            )
            for key in (
                "entry_pc", "final_pc", "dispatches", "exit_reason", "exit_name",
                "dispatch_status", "guest_instruction_attempts", "gpr_lo",
                "history", "syscall_history",
            ):
                assert paced_report[key] == report[key], (key, report, paced_report)

            invalid_environment = dict(os.environ)
            invalid_environment["SDBZ_REPORT_SNAPSHOT_MS"] = "not-a-number"
            invalid_result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8", "--no-recompile",
                    "--report", str(root / "invalid-snapshot.json"),
                ],
                capture_output=True, text=True, check=False,
                env=invalid_environment,
            )
            assert invalid_result.returncode != 0, (
                invalid_result.stdout + invalid_result.stderr
            )
            assert "invalid SDBZ_REPORT_SNAPSHOT_MS" in invalid_result.stderr, (
                invalid_result.stdout + invalid_result.stderr
            )
            assert (root / "out" / "objects" / "objects.json").is_file()
            link_only = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--no-recompile", "--link-only",
                ],
                capture_output=True, text=True, check=False,
            )
            assert link_only.returncode == 0, link_only.stdout + link_only.stderr
            assert "native library ready:" in link_only.stdout, link_only.stdout
            assert "EE stopped:" not in link_only.stdout, link_only.stdout

            # A host-only code/tool update must preserve the previously emitted
            # guest IR and objects. Simulate an older v1 provenance snapshot
            # that predates separate IR-generator source hashes, and change only
            # the saved linker-script fingerprint.
            cache_path = root / "out" / "translation-cache.json"
            original_snapshot = json.loads(cache_path.read_text(encoding="utf-8"))
            preserved_metadata = (root / "out" / "ir" / "ee_functions.json").read_bytes()
            preserved_ir = {
                item.name: item.read_bytes()
                for item in (root / "out" / "ir").glob("*.ll")
            }
            legacy_snapshot = dict(original_snapshot)
            legacy_snapshot.pop("ir_sources", None)
            legacy_snapshot.pop("ir_hashes", None)
            legacy_snapshot["inputs"] = dict(original_snapshot["inputs"])
            legacy_snapshot["inputs"]["tools/link_native_objects.py"] = "0" * 64
            cache_path.write_text(
                json.dumps(legacy_snapshot) + "\n", encoding="utf-8"
            )
            stale_link = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--no-recompile", "--link-only",
                ],
                capture_output=True, text=True, check=False,
            )
            assert stale_link.returncode != 0, stale_link
            assert "tools/link_native_objects.py changed" in stale_link.stderr

            repaired = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--relink-existing", "--link-only",
                ],
                capture_output=True, text=True, check=False,
            )
            assert repaired.returncode == 0, repaired.stdout + repaired.stderr
            assert "preserving guest IR and objects" in repaired.stdout, repaired.stdout
            assert "ee_recomp: loading" not in repaired.stdout, repaired.stdout
            assert "reused 1/1 cached object(s)" in repaired.stdout, repaired.stdout
            assert (root / "out" / "ir" / "ee_functions.json").read_bytes() == preserved_metadata
            assert {
                item.name: item.read_bytes()
                for item in (root / "out" / "ir").glob("*.ll")
            } == preserved_ir

            # A mismatched generator-source fingerprint must not reuse old IR.
            repair_command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--relink-existing", "--link-only",
            ]
            repaired_snapshot = json.loads(cache_path.read_text(encoding="utf-8"))

            # The previous dispatch diagnostics header was fingerprinted in
            # include/ee, despite never participating in guest lowering.
            old_key = "include/ee/ee_hot_loop_diagnostics.h"
            new_key = "src/runtime/ee_hot_loop_diagnostics.h"
            old_digest = "d5c9345285e8a93958113d7d24d16c9d8131ced874765bb2a06c8761423017ad"
            assert old_key not in repaired_snapshot["ir_sources"]
            assert new_key not in repaired_snapshot["ir_sources"]
            assert new_key in repaired_snapshot["inputs"]
            old_diagnostics = dict(repaired_snapshot)
            old_diagnostics["ir_sources"] = dict(repaired_snapshot["ir_sources"])
            old_diagnostics["inputs"] = dict(repaired_snapshot["inputs"])
            old_diagnostics["inputs"].pop(new_key)
            old_diagnostics["inputs"][old_key] = old_digest
            old_diagnostics["ir_sources"][old_key] = old_digest
            cache_path.write_text(
                json.dumps(old_diagnostics) + "\n", encoding="utf-8"
            )
            migrated = subprocess.run(
                repair_command, capture_output=True, text=True, check=False,
            )
            assert migrated.returncode == 0, migrated.stdout + migrated.stderr
            assert "migrated host-only dispatch diagnostics" in migrated.stdout
            assert "reused 1/1 cached object(s)" in migrated.stdout
            assert (root / "out" / "ir" / "ee_functions.json").read_bytes() == preserved_metadata
            assert {
                item.name: item.read_bytes()
                for item in (root / "out" / "ir").glob("*.ll")
            } == preserved_ir
            repaired_snapshot = json.loads(cache_path.read_text(encoding="utf-8"))

            # Modern provenance allows a direct relink without repeating
            # the emitter's IR/object scan. The linker must still reject
            # damaged objects and the incremental fallback repairs them.
            direct = subprocess.run(
                repair_command, capture_output=True, text=True, check=False)
            assert direct.returncode == 0, direct.stdout + direct.stderr
            assert "skipping redundant emission scan" in direct.stdout
            assert "verify_ir: reused" not in direct.stdout
            object_path = next((root / "out" / "objects").glob("*.o"))
            object_path.write_bytes(object_path.read_bytes() + b"damaged")
            repaired_object = subprocess.run(
                repair_command, capture_output=True, text=True, check=False)
            assert repaired_object.returncode == 0, repaired_object.stdout + repaired_object.stderr
            assert "direct relink failed; verifying and repairing objects" in repaired_object.stdout
            assert "reused 0/1 cached object(s)" in repaired_object.stdout
            repaired_snapshot = json.loads(cache_path.read_text(encoding="utf-8"))

            # Unknown historical header content cannot be silently accepted.
            old_diagnostics["inputs"][old_key] = "0" * 64
            old_diagnostics["ir_sources"][old_key] = "0" * 64
            cache_path.write_text(
                json.dumps(old_diagnostics) + "\n", encoding="utf-8"
            )
            rejected_diagnostics = subprocess.run(
                repair_command, capture_output=True, text=True, check=False,
            )
            assert rejected_diagnostics.returncode != 0
            assert "guest IR generation input changed:" in rejected_diagnostics.stderr

            altered_sources = dict(repaired_snapshot)
            altered_sources["ir_sources"] = dict(repaired_snapshot["ir_sources"])
            altered_sources["ir_sources"]["ELF"] = "f" * 64
            cache_path.write_text(
                json.dumps(altered_sources) + "\n", encoding="utf-8"
            )
            stale_ir_source = subprocess.run(
                repair_command, capture_output=True, text=True, check=False,
            )
            assert stale_ir_source.returncode != 0, stale_ir_source
            assert "guest IR generation input changed: ELF" in stale_ir_source.stderr
            cache_path.write_text(
                json.dumps(repaired_snapshot) + "\n", encoding="utf-8"
            )

            # Even with a matching ELF and generator, the preserved code's
            # content must still match its recorded hashes.
            module_path = next((root / "out" / "ir").glob("*.ll"))
            original_module = module_path.read_bytes()
            module_path.write_bytes(original_module + b"\n; tampered\n")
            modified_module = subprocess.run(
                repair_command, capture_output=True, text=True, check=False,
            )
            assert modified_module.returncode != 0, modified_module
            assert "preserved LLVM IR module was modified" in modified_module.stderr
            module_path.write_bytes(original_module)

            # A replay must reject old native code when the same ELF path
            # changes, even if its entry and translated function map do not.
            cache_path = root / "out" / "translation-cache.json"
            provenance = json.loads(cache_path.read_text(encoding="utf-8"))
            assert provenance["schema"] == "ee-translation-cache-v1", provenance
            cached_command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--no-recompile", "--link-only",
            ]
            original_elf = elf.read_bytes()
            altered_elf = bytearray(original_elf)
            altered_elf[-1] ^= 1
            elf.write_bytes(altered_elf)
            changed_elf = subprocess.run(
                cached_command, capture_output=True, text=True, check=False,
            )
            assert changed_elf.returncode != 0, changed_elf
            assert "ELF changed" in changed_elf.stderr, changed_elf.stderr
            elf.write_bytes(original_elf)

            # An unchanged ELF still cannot reuse code from a different
            # recompiler build or a different LLVM object optimization mode.
            changed_compiler = dict(provenance)
            changed_compiler["inputs"] = dict(provenance["inputs"])
            changed_compiler["inputs"]["ee_recomp"] = "0" * 64
            cache_path.write_text(
                json.dumps(changed_compiler) + "\n", encoding="utf-8",
            )
            mismatched_compiler = subprocess.run(
                cached_command, capture_output=True, text=True, check=False,
            )
            assert mismatched_compiler.returncode != 0, mismatched_compiler
            assert "ee_recomp changed" in mismatched_compiler.stderr, (
                mismatched_compiler.stderr
            )
            cache_path.write_text(
                json.dumps(provenance) + "\n", encoding="utf-8",
            )
            wrong_optimization = subprocess.run(
                cached_command + ["--emit-opt=2"],
                capture_output=True, text=True, check=False,
            )
            assert wrong_optimization.returncode != 0, wrong_optimization
            assert "different --emit-opt" in wrong_optimization.stderr, (
                wrong_optimization.stderr
            )

            library_name = (
                "game-translated.dll" if sys.platform == "win32" else
                "libgame-translated.dylib" if sys.platform == "darwin" else
                "libgame-translated.so"
            )
            library = root / "out" / library_name

            # A folder-backed HLE IOP backend must not require an ISO or ROM0,
            # even when the legacy ROM0 environment variable is present.
            disc_folder = root / "extracted-disc"
            disc_folder.mkdir()
            (disc_folder / "SYSTEM.CNF").write_text(
                "BOOT2 = cdrom0:\\MINIMAL-MIPS.ELF;1\n", encoding="ascii"
            )
            hle_environment = dict(os.environ)
            hle_environment["SDBZ_IOP_ROM0"] = str(root / "absent-rom0.bin")
            folder_command = [
                str(runner), str(elf), str(library),
                "--max-dispatches=8", f"--disc-folder={disc_folder}",
            ]
            folder_result = subprocess.run(
                folder_command, capture_output=True, text=True, check=False,
                env=hle_environment,
            )
            assert "[disc] HLE folder mounted:" in folder_result.stdout, (
                folder_result.stdout + folder_result.stderr
            )
            assert "virtual ISO9660 sectors=" in folder_result.stdout, (
                folder_result.stdout + folder_result.stderr
            )
            assert "IOP cdvdman search service install failed" not in (
                folder_result.stdout + folder_result.stderr
            )
            assert "IOP ROM0 reset:" not in folder_result.stdout, folder_result.stdout
            assert "EE stopped:" in folder_result.stdout, folder_result.stdout

            wrapper_folder = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"), "--no-recompile",
                    "--max-dispatches=8", "--disc-folder", str(disc_folder),
                ],
                capture_output=True, text=True, check=False,
                env=hle_environment,
            )
            assert "[disc] HLE folder mounted:" in wrapper_folder.stdout, (
                wrapper_folder.stdout + wrapper_folder.stderr
            )
            assert "EE stopped:" in wrapper_folder.stdout, wrapper_folder.stdout

            invalid_folder = subprocess.run(
                folder_command[:-1] +
                [f"--disc-folder={root / 'missing-disc-folder'}"],
                capture_output=True, text=True, check=False,
                env=hle_environment,
            )
            assert invalid_folder.returncode == 2, invalid_folder
            assert "disc folder must be an existing" in invalid_folder.stderr

            conflicting_sources = subprocess.run(
                folder_command + ["--disc-image=dummy.iso"],
                capture_output=True, text=True, check=False,
                env=hle_environment,
            )
            assert conflicting_sources.returncode == 2, conflicting_sources
            assert "usage:" in conflicting_sources.stdout, conflicting_sources.stdout

            original_library = library.read_bytes()
            altered_library = bytearray(original_library)
            altered_library[-1] ^= 1
            library.write_bytes(altered_library)
            changed_library = subprocess.run(
                cached_command, capture_output=True, text=True, check=False,
            )
            assert changed_library.returncode != 0, changed_library
            assert f"{library_name} changed" in changed_library.stderr, (
                changed_library.stderr
            )
            library.write_bytes(original_library)

            # Confirm a completely unchanged translation is still reusable.
            valid_cached = subprocess.run(
                cached_command, capture_output=True, text=True, check=False,
            )
            assert valid_cached.returncode == 0, (
                valid_cached.stdout + valid_cached.stderr
            )

            # Compare unoptimized and optimized native EE output for the same
            # guest instructions. Optimizing host code must not change EE state.
            optimized_report = root / "optimized.json"
            optimized = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "optimized"),
                    "--max-dispatches=8",
                    "--emit-opt=2",
                    "--kseg-ram",
                    "--report", str(optimized_report),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            optimized_state = json.loads(optimized_report.read_text(encoding="utf-8"))
            optimized_objects = json.loads(
                (root / "optimized" / "objects" / "objects.json").read_text(encoding="utf-8")
            )
            assert optimized_objects["optimization_level"] == 2, optimized_objects
            for field in ("entry_pc", "final_pc", "dispatches", "exit_reason",
                          "dispatch_status", "gpr_lo"):
                assert optimized_state[field] == report[field], (field, optimized_state, report)
            assert optimized.returncode == result.returncode, (
                optimized.stdout + optimized.stderr
            )


    if shard is None or shard == 1:
        with tempfile.TemporaryDirectory(prefix="ee-native-setupthread-") as tmp:
            root = Path(tmp)
            elf = root / "startup-mips.elf"
            make_elf(elf, startup=True)
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--hle-startup",
                    "--trace",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE SetupThread:" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "EE stopped: dispatched=2" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "guest left compiled code" in result.stderr, (
                result.stdout + result.stderr
            )

        with tempfile.TemporaryDirectory(prefix="ee-native-auto-expand-") as tmp:
            root = Path(tmp)
            elf = root / "caller-callee.elf"
            make_call_elf(elf)
            base = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--trace",
            ]
            initial = subprocess.run(
                base,
                capture_output=True,
                text=True,
                check=False,
            )
            assert "EE stopped: dispatched=" in initial.stdout, (
                initial.stdout + initial.stderr
            )
            assert "auto-expand: compiling" not in initial.stdout, initial.stdout
            assert "link_native_objects: compiling 6 runtime sources (cache miss)" in initial.stdout, (
                initial.stdout + initial.stderr
            )
            metadata_path = root / "out" / "ir" / "ee_functions.json"
            assert json.loads(metadata_path.read_text(encoding="utf-8"))["function_count"] == 1

            # Prepare one shared profile and one pre-sharing legacy
            # profile while the base still owns just the entry function.
            early = root / "early-optimized"
            legacy = root / "legacy-optimized"
            prepare_early = [
                sys.executable, str(source / "tools" / "build_optimized_cache.py"),
                str(elf), "--build-dir", str(build), "--emit-opt=2",
                "--source-work-dir", str(root / "out"),
                "--work-dir", str(early),
            ]
            result = subprocess.run(prepare_early, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert not (early / "ir").exists()
            legacy_build = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build), "--emit-opt=2",
                "--work-dir", str(legacy), "--link-only",
            ]
            result = subprocess.run(legacy_build, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert (legacy / "ir" / "ee_functions.json").is_file()

            resumed = subprocess.run(
                base + ["--no-recompile", "--auto-expand=1",
                        "--trace-entry=0x00100008"],
                capture_output=True,
                text=True,
                check=False,
                env=dict(os.environ, SDBZ_EE_DIRECT_DISPATCH="1"),
            )
            assert "EE direct dispatch: enabled" in resumed.stdout, resumed.stdout
            # run_hot_expand merges child stderr into stdout. Keep the
            # three-dispatch assertion, but inspect the merged stream.
            assert "EE direct dispatch: executed=3 " in resumed.stdout, resumed.stdout
            assert "auto-expand: compiling reachable PC 0x00100030" in resumed.stdout, (
                resumed.stdout + resumed.stderr
            )
            assert "EE hot-expand: resumed guest pc=0x00100030" in resumed.stdout, (
                resumed.stdout + resumed.stderr
            )
            assert resumed.stdout.count("EE trace-entry:") == 1, (
                "guest entry executed more than once (unexpected replay): " + resumed.stdout
            )
            assert resumed.stdout.count("EE ELF entry") == 1, resumed.stdout
            assert "EE stopped: dispatched=" in resumed.stdout, (
                resumed.stdout + resumed.stderr
            )
            assert resumed.stdout.count("ee_recomp: loading") == 1, resumed.stdout
            assert "verify_ir: append-only preserved 1 cached object(s) without rehash" in resumed.stdout, (
                resumed.stdout + resumed.stderr
            )
            assert "link_native_objects: reused 6 cached runtime objects" in resumed.stdout, (
                resumed.stdout + resumed.stderr
            )
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            assert metadata["function_count"] == 2, metadata

            # A new discovery in the base must be visible to an existing
            # shared profile without re-lifting either guest function.
            result = subprocess.run(prepare_early, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert "synchronizing native profile to shared EE IR" in result.stdout
            assert "reused 1/2" in result.stdout, result.stdout
            assert "ee_recomp: loading" not in result.stdout
            assert not (early / "ir").exists()
            early_cache = json.loads((early / "translation-cache.json").read_text())
            assert len(early_cache["ir_hashes"]) == 2

            # Migrate the legacy profile with the shorter verified prefix.
            # Its existing O2 object must be rekeyed, not recompiled.
            prepare_legacy = [*prepare_early[:-1], str(legacy)]
            result = subprocess.run(prepare_legacy, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert "reused 1/2" in result.stdout, result.stdout
            assert "removed 1 verified duplicate modules" in result.stdout
            assert not list((legacy / "ir").glob("*.ll"))
            assert not (legacy / "ir" / "ee_functions.json").exists()
            legacy_cache = json.loads((legacy / "translation-cache.json").read_text())
            assert legacy_cache["ir_hashes"] == early_cache["ir_hashes"]

            # Verify a separate native O2 variant from live-expanded IR.
            original_cache = root / "out" / "translation-cache.json"
            original_bytes = original_cache.read_bytes()
            original_map = metadata_path.read_bytes()
            variant = root / "optimized"
            prepare = [
                sys.executable, str(source / "tools" / "build_optimized_cache.py"),
                str(elf), "--build-dir", str(build), "--emit-opt=2",
                "--source-work-dir", str(root / "out"),
                "--work-dir", str(variant),
            ]
            # A source DLL may be stale or completely absent; native
            # profiles depend on verified IR, not another profile's DLL.
            # Select the actual native library, not a cache directory.
            base_library = next(p for p in (root / "out").iterdir()
                if p.is_file() and p.name in ("game-translated.dll",
                    "libgame-translated.so", "libgame-translated.dylib"))
            saved_library = base_library.with_name(base_library.name + ".saved")
            base_library.rename(saved_library)
            try:
                for expected_message in ("Built O2 EE variant", "Reusing verified O2 EE cache"):
                    result = subprocess.run(prepare, capture_output=True, text=True, check=False)
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert expected_message in result.stdout, result.stdout
                    assert "no source DLL relink" in result.stdout
                    if expected_message.startswith("Reusing"):
                        assert "ir_scan=already-verified" in result.stdout, result.stdout
                    assert not base_library.exists()
                    assert original_cache.read_bytes() == original_bytes
                    assert metadata_path.read_bytes() == original_map
            finally:
                saved_library.rename(base_library)
            variant_cache = json.loads((variant / "translation-cache.json").read_text(encoding="utf-8"))
            assert variant_cache["emit_opt"] == 2
            assert variant_cache["ir_hashes"] == json.loads(original_bytes)["ir_hashes"]
            shared_marker = json.loads((variant / "shared-ir.json").read_text(encoding="utf-8"))
            assert not Path(shared_marker["ir_dir"]).is_absolute()
            assert (variant / shared_marker["ir_dir"]).resolve() == metadata_path.parent
            assert not (variant / "ir").exists()
            variant_metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            assert len(variant_metadata["functions"]) == 2
            assert all(Path(fn["ir_path"]).parent == Path(".")
                       for fn in variant_metadata["functions"])
            states = []
            for work_dir, opt in ((root / "out", 0), (variant, 2)):
                report = work_dir / "comparison.json"
                result = subprocess.run([
                    sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                    "--build-dir", str(build), "--work-dir", str(work_dir),
                    f"--emit-opt={opt}", "--max-dispatches=8", "--no-recompile",
                    f"--report={report}",
                ], capture_output=True, text=True, check=False)
                assert report.is_file(), result.stdout + result.stderr
                states.append(json.loads(report.read_text(encoding="utf-8")))
            for key in ("final_pc", "dispatches", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo"):
                assert states[0][key] == states[1][key], (key, states)
            # A refreshed canonical source can have current IR provenance
            # while a native profile still carries older lowering fingerprints.
            # Rebuild the profile using only authenticated shared IR and the
            # full incremental object verifier, not a stale-cache relabel.
            variant_cache_path = variant / "translation-cache.json"
            stale_profile = json.loads(variant_cache_path.read_text(encoding="utf-8"))
            stale_profile["ir_sources"]["include/ee/ee_cycle_scheduler.h"] = "0" * 64
            variant_cache_path.write_text(json.dumps(stale_profile), encoding="utf-8")
            repaired = subprocess.run(prepare, capture_output=True, text=True)
            assert repaired.returncode == 0, repaired.stdout + repaired.stderr
            assert "Optimized profile lowering provenance is stale" in repaired.stdout
            assert "reused 2/2" in repaired.stdout, repaired.stdout
            repaired_cache = json.loads(variant_cache_path.read_text(encoding="utf-8"))
            assert repaired_cache["ir_sources"] == variant_cache["ir_sources"]
            assert repaired_cache["ir_hashes"] == variant_cache["ir_hashes"]
            assert metadata_path.read_bytes() == original_map
            assert original_cache.read_bytes() == original_bytes

            warm = subprocess.run([*prepare, "--defer-warm-validation"],
                                  capture_output=True, text=True, check=False)
            assert warm.returncode == 0, warm.stdout + warm.stderr
            assert "full IR verification deferred to runner" in warm.stdout
            assert "Shared EE IR: verified" not in warm.stdout
            module = metadata_path.parent / variant_metadata["functions"][0]["ir_path"]
            module.write_bytes(module.read_bytes() + b"\n; modified\n")
            # Preflight does not claim to authenticate the IR. The actual
            # driver must fail before native execution when IR is damaged.
            warm = subprocess.run([*prepare, "--defer-warm-validation"],
                                  capture_output=True, text=True, check=False)
            assert warm.returncode == 0, warm.stdout + warm.stderr
            assert "full IR verification deferred to runner" in warm.stdout
            rejected_report = variant / "rejected.json"
            rejected_run = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(variant),
                "--emit-opt=2", "--max-dispatches=8", "--no-recompile",
                f"--report={rejected_report}",
            ], capture_output=True, text=True, check=False)
            assert rejected_run.returncode != 0
            assert not rejected_report.exists()
            assert "shared EE IR provenance is stale or incompatible" in rejected_run.stderr
            refused = subprocess.run(prepare, capture_output=True, text=True, check=False)
            assert refused.returncode != 0, refused.stdout + refused.stderr
            assert "verified IR hashes do not match" in refused.stderr
            assert original_cache.read_bytes() == original_bytes

        # Signed word ALU ops consume the low words. A delay-slot overflow
        # commits preceding writes, preserves rd, and reports the branch EPC.
        with tempfile.TemporaryDirectory(prefix="ee-signed-word-") as tmp:
            root = Path(tmp)
            for name, immediate, operation, expected in (
                ("add", 42, 0x00854820, 43),
                ("sub", 0, 0x00854822, (1 << 64) - 1),
                ("add_overflow", 0x7fffffff, 0x00854820, None),
                ("sub_overflow", 0x80000000, 0x00854822, None),
                ("sub_to_zero_overflow", 0x80000000, 0x00850022, None),
            ):
                elf = root / (name + ".elf")
                make_elf(elf)
                image = bytearray(elf.read_bytes())
                words = (
                    0x3c040000 | (immediate >> 16),
                    0x34840000 | (immediate & 0xffff),
                    0x24050001,  # a1 = 1
                    0x24090063,  # previous t1 = 99
                    0x00000008,  # jr zero
                    operation,
                )
                struct.pack_into("<6I", image, 0x108, *words)
                elf.write_bytes(image)
                report = root / (name + ".json")
                result = subprocess.run([
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build), "--work-dir",
                    str(root / name), "--functions=entry", "--max-dispatches=1",
                    "--report", str(report),
                ], capture_output=True, text=True)
                assert report.is_file(), result.stdout + result.stderr
                state = json.loads(report.read_text())
                if expected is None:
                    assert state["cop0_cause"] & 0x8000007c == 0x80000030, state
                    assert state["cop0_epc"] == 0x00100018, state
                    assert state["gpr_lo"][9] == 99, state
                else:
                    assert state["gpr_lo"][9] == expected, state
                    assert state["cop0_cause"] & 0x7c == 0, state

        # The USA resource wrapper jumps to a block which consumes both
        # incoming arguments with doubleword arithmetic, then reuses a0.
        with tempfile.TemporaryDirectory(prefix="ee-native-tail-arguments-") as tmp:
            root = Path(tmp)
            elf = root / "tail-arguments.elf"
            make_elf(elf)
            image = bytearray(elf.read_bytes())
            words = (
                0x24040029,  # addiu a0,zero,41
                0x08040007,  # j 0x10001c
                0x24050001,  # delay slot: addiu a1,zero,1
                0, 0,
                0x0085402d,  # daddu t0,a0,a1
                0x0080482d,  # daddu t1,a0,zero
                0x0085502f,  # dsubu t2,a0,a1
                0x00a4582f,  # dsubu t3,a1,a0
                0x24040000,  # addiu a0,zero,0
                0x0c04000e,  # jal 0x100038
                0x24050000,  # delay slot: addiu a1,zero,0
                0x00000008, 0,  # jr zero; nop (also the callee)
            )
            struct.pack_into("<14I", image, 0x108, *words)
            elf.write_bytes(image)
            report = root / "tail.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(root / "out"),
                "--functions=all", "--max-dispatches=8", "--hle-startup",
                "--trace-entry=0x00100038",
            ]
            reported = subprocess.run(command + ["--report", str(report)],
                                      capture_output=True, text=True)
            state = json.loads(report.read_text())
            assert state["gpr_lo"][8:12] == [42, 41, 40, (1 << 64) - 40], state
            quiet = subprocess.run(command + ["--no-recompile"],
                                   capture_output=True, text=True)
            entry = "EE trace-entry: dispatch=2 pc=0x00100038"
            assert entry in reported.stdout, reported.stdout + reported.stderr
            assert entry in quiet.stdout, quiet.stdout + quiet.stderr
            assert quiet.returncode == reported.returncode

        # The display setup shifts a width computed in the delay slot,
        # then overwrites the width register in the successor block.
        with tempfile.TemporaryDirectory(prefix="ee-native-shift-arguments-") as tmp:
            root = Path(tmp)
            elf = root / "shift-arguments.elf"
            make_elf(elf)
            image = bytearray(elf.read_bytes())
            words = (
                0x24060200,  # addiu a2,zero,512
                0x04c10002,  # bgez a2,0x100018
                0x00062983,  # delay slot: sra a1,a2,6
                0,
                0x0005303c,  # dsll32 a2,a1,0
                0x3c050050,  # lui a1,0x50
                0x0006303f,  # dsra32 a2,a2,0
                0x00064278,  # dsll t0,a2,9
                0x00000008, 0,  # jr zero; nop
            )
            struct.pack_into("<10I", image, 0x108, *words)
            elf.write_bytes(image)
            report = root / "shift.json"
            result = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build), "--work-dir",
                str(root / "out"), "--functions=entry", "--max-dispatches=8",
                "--report", str(report),
            ], capture_output=True, text=True)
            assert report.is_file(), result.stdout + result.stderr
            state = json.loads(report.read_text())
            assert state["gpr_lo"][8] == 8 << 9, state

        # Batch two related explicit function entries from one cache reload,
        # then publish one merged metadata file. Existing IR stays untouched.
        with tempfile.TemporaryDirectory(prefix="ee-native-batch-append-") as tmp:
            root = Path(tmp)
            elf = root / "overlap-batch.elf"
            make_overlapping_entry_elf(elf)
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=64", "--hle-startup", "--trace",
                    "--extra-functions=0x00100050,0x00100054",
                ],
                capture_output=True, text=True, check=False,
            )
            assert "ee_recomp: loading" in result.stdout, result.stdout + result.stderr
            assert result.stdout.count("ee_recomp: loading") == 2, (
                result.stdout + result.stderr
            )  # ELF entry translation + ONE --functions batch
            assert "auto-expand: appended 2 function(s) in one discovery" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "functions=2" in result.stdout, result.stdout
            metadata = json.loads(
                (root / "out" / "ir" / "ee_functions.json").read_text(encoding="utf-8")
            )
            owners = [
                int(pc, 0) for fn in metadata["functions"]
                for pc in fn["block_pcs"]
            ]
            assert metadata["function_count"] == 3, metadata
            assert len(set(owners)) == len(owners), metadata
            assert 0x00100050 in owners and 0x00100054 in owners
            assert "HLE FlushCache:" in result.stdout, result.stdout + result.stderr

            # Reject malformed target lists instead of silently truncating,
            # misparsing or compiling the wrong guest address.
            recomp = driver.native_executable(build, "ee_recomp")
            for invalid_list in (
                "0x00100050,0x00100050",  # duplicate
                "0x00100050,0x00100051",  # unaligned
                "0x00100050,,0x00100054",  # malformed
                "0x00100050,not_hex",  # invalid
            ):
                invalid = subprocess.run([
                    str(recomp), str(elf), str(root / "invalid"),
                    "--functions=" + invalid_list,
                ], capture_output=True, text=True, check=False)
                assert invalid.returncode != 0, (
                    invalid_list, invalid.stdout, invalid.stderr
                )

        for invalid_set in (False, True):
            with tempfile.TemporaryDirectory(prefix="ee-native-osd-config-") as tmp:
                root = Path(tmp)
                elf = root / "kernel-osd-config.elf"
                make_osd_config_elf(elf, invalid_set=invalid_set)
                report_path = root / "osd-config.json"
                command = [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=16",
                    "--trace",
                    "--report", str(report_path),
                ]

                # The kernel must never be silently enabled without HLE.
                if not invalid_set:
                    without_hle = subprocess.run(
                        command, capture_output=True, text=True, check=False,
                    )
                    assert without_hle.returncode != 0, (
                        without_hle.stdout + without_hle.stderr
                    )
                    refused = json.loads(report_path.read_text(encoding="utf-8"))
                    assert refused["cop0_epc"] == 0x00100014, refused
                    assert "HLE GetOsdConfigParam:" not in without_hle.stdout
                    command.append("--no-recompile")

                with_hle = subprocess.run(
                    command + ["--hle-startup"],
                    capture_output=True, text=True, check=False,
                )
                assert with_hle.returncode != 0, with_hle.stdout + with_hle.stderr
                assert "HLE GetOsdConfigParam: pointer=0x00100200 value=0x00000000 result=0" in with_hle.stdout, (
                    with_hle.stdout + with_hle.stderr
                )
                assert "HLE SetOsdConfigParam: pointer=0x00100210 value=0x12345678 result=0" in with_hle.stdout, (
                    with_hle.stdout + with_hle.stderr
                )
                assert "HLE GetOsdConfigParam: pointer=0x00100220 value=0x12345678 result=0" in with_hle.stdout, (
                    with_hle.stdout + with_hle.stderr
                )
                denied_service = "Set" if invalid_set else "Get"
                denied_address = 0x00100221 if invalid_set else 0x7ffffffc
                assert (
                    f"{denied_service}OsdConfigParam HLE refused unbacked or "
                    f"unaligned pointer 0x{denied_address:08x}"
                ) in with_hle.stderr, with_hle.stdout + with_hle.stderr

                state = json.loads(report_path.read_text(encoding="utf-8"))
                assert state["cop0_epc"] == 0x00100058, state
                registers = state["gpr_lo"]
                assert registers[8] == 0, state           # initially zero
                assert registers[9] == 0, state           # Get returns zero
                assert registers[10] == 0, state          # Set returns zero
                assert registers[11] == 0x12345678, state # Set persisted
                assert registers[12] == 0, state          # second Get result
                assert registers[13] == 0, state          # no fallthrough
                assert state["exit_name"] == "EXCEPTION", state

        with tempfile.TemporaryDirectory(prefix="ee-native-sif-dchain-") as tmp:
            root = Path(tmp)
            elf = root / "sif-dchain.elf"
            make_sif_set_dchain_elf(elf)
            report_path = root / "sif-dchain.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16", "--trace",
                "--report", str(report_path),
            ]

            # No HLE means the first syscall remains an exception.
            disabled = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert disabled.returncode == 3, disabled.stdout + disabled.stderr
            disabled_state = json.loads(report_path.read_text(encoding="utf-8"))
            assert disabled_state["cop0_epc"] == 0x00100028, disabled_state
            assert "HLE SifSetDChain" not in disabled.stdout
            assert disabled_state["gpr_lo"][2] == 0x1000c000, disabled_state

            enabled = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert enabled.returncode == 3, enabled.stdout + enabled.stderr
            assert (
                "HLE SifSetDChain: channel=5 qwc=0 "
                "chcr=0x00000184 result=void (no IOP producer)"
            ) in enabled.stdout, enabled.stdout + enabled.stderr
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["exit_name"] == "EXCEPTION", state
            assert state["cop0_epc"] == 0x00100040, state
            assert state["gpr_lo"][2] == 0x1000c000, state
            assert state["gpr_lo"][16] == 0x1000c000, state  # V0 preserved
            assert state["gpr_lo"][17] == 0, state           # SIF0 QWC
            assert state["gpr_lo"][18] == 0x184, state       # SIF0 CHCR
            assert state["gpr_lo"][19] == 0, state           # SIF0 MADR
            assert "guest blocked by an unsupported operation" in enabled.stderr


        with tempfile.TemporaryDirectory(prefix="ee-native-sif-register-") as tmp:
            root = Path(tmp)
            elf = root / "sif-register.elf"
            make_sif_register_elf(elf)
            report_path = root / "sif-register.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16", "--trace",
                "--report", str(report_path),
            ]

            disabled = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert disabled.returncode == 3, disabled.stdout + disabled.stderr
            disabled_state = json.loads(report_path.read_text(encoding="utf-8"))
            assert disabled_state["cop0_epc"] == 0x00100010, disabled_state
            assert "HLE SifGetReg:" not in disabled.stdout, disabled.stdout

            enabled = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert enabled.returncode == 3, enabled.stdout + enabled.stderr
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["exit_name"] == "EXCEPTION", state
            assert state["cop0_epc"] == 0x001000a0, state
            regs = state["gpr_lo"]
            assert regs[16] == 0, state              # initial SUBADDR
            assert regs[17] == 0, state              # SifSetReg returns 0
            assert regs[18] == 0x00102200, state     # software sysreg persists
            assert regs[19] == 0, state              # main address set returns 0
            assert regs[20] == 0x00102300, state     # MSCOM direct MMIO
            assert regs[21] == 0x00102300, state     # MSCOM syscall read
            assert regs[22] == 0x00020000, state     # MSFLAG OR'd via SifSetReg
            assert regs[23] == 0, state              # no fabricated IOP SUBADDR
            assert regs[9] == 0, state               # no fallthrough after refusal
            assert (
                "HLE SifGetReg: register=0x80000000 result=0x00000000"
            ) in enabled.stdout, enabled.stdout + enabled.stderr
            assert (
                "HLE SifGetReg: register=0x80000000 result=0x00102200"
            ) in enabled.stdout, enabled.stdout + enabled.stderr
            assert (
                "HLE SifSetReg: register=0x00000003 "
                "value=0x00020000 result=0"
            ) in enabled.stdout, enabled.stdout + enabled.stderr
            assert (
                "SifGetReg HLE refused unsupported register=0x7fff0000"
            ) in enabled.stderr, enabled.stdout + enabled.stderr


        # When no IOP is running, repeated reads of IOP-owned SMFLAG must
        # report a bounded diagnostic rather than consuming the whole replay.
        # Default (0) intentionally leaves ordinary execution untouched.
        with tempfile.TemporaryDirectory(prefix="ee-native-sif-poll-") as tmp:
            root = Path(tmp)
            elf = root / "sif-poll.elf"
            make_sif_poll_elf(elf)
            report_path = root / "sif-poll.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=48", "--hle-startup", "--trace",
                "--report", str(report_path),
            ]
            baseline = subprocess.run(
                command + ["--sif-poll-limit=0"],
                capture_output=True, text=True, check=False,
            )
            # The last handled syscall can retain EXCEPTION/status=0 when
            # the native dispatch budget ends at that very boundary.
            assert baseline.returncode == 3, baseline.stdout + baseline.stderr
            baseline_state = json.loads(report_path.read_text(encoding="utf-8"))
            assert baseline_state["dispatches"] == 48, baseline_state
            assert "SIF SMFLAG polling stalled" not in baseline.stderr

            bounded = subprocess.run(
                command + ["--no-recompile", "--sif-poll-limit=6"],
                capture_output=True, text=True, check=False,
            )
            assert bounded.returncode == 3, bounded.stdout + bounded.stderr
            bounded_state = json.loads(report_path.read_text(encoding="utf-8"))
            assert bounded_state["dispatches"] < 48, bounded_state
            assert bounded_state["final_pc"] == 0x00100010, bounded_state
            assert bounded_state["exit_name"] == "UNSUPPORTED", bounded_state
            assert "SIF SMFLAG polling stalled:" in bounded.stderr, bounded.stderr
            assert "guest_epc=0x00100010 register=4 value=0 reads=6" in bounded.stderr, bounded.stderr

        # HLE-only extracted-disc mode models the already-bootstrapped IOP
        # service baseline, matching ps2-web-recomp's SDBZ HLE model. It must
        # not require ROM0 or pretend a guest IRX published that SMFLAG.
        with tempfile.TemporaryDirectory(prefix="ee-native-hle-disc-sif-") as tmp:
            root = Path(tmp)
            elf = root / "ee-sif-wait.elf"
            disc = root / "PS2_GAME"
            disc.mkdir()
            (disc / "SYSTEM.CNF").write_text(
                "BOOT2 = cdrom0:\\\\SLUS_214.42;1\\n", encoding="ascii"
            )
            make_sif_handshake_elf(elf)
            report_path = root / "hle-sif.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=48", "--hle-startup", "--trace",
                "--sif-poll-limit=6", "--report", str(report_path),
                "--disc-folder", str(disc),
            ]
            run = subprocess.run(command, capture_output=True,
                                 text=True, check=False)
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert "[iop] HLE bootstrap: sifcmd=ready" in run.stdout, (
                run.stdout + run.stderr
            )
            assert "HLE SifGetReg: register=0x00000004 result=0x00060000" in (
                run.stdout
            ), run.stdout
            assert state["gpr_lo"][16] == 0x60000, state
            assert state["dispatches"] < 48, state
            assert "SIF SMFLAG polling stalled" not in run.stderr, run.stderr
            assert "IOP guest loaded:" not in run.stdout, run.stdout
            assert "IOP guest published SMFLAG:" not in run.stdout, run.stdout

        # End-to-end integration: EE polls SMFLAG, *actual* IOP R3000 guest
        # instructions invoke sifman via IRX imports, and the EE then exits
        # its polling loop. Without an IOP module, the identical ELF stalls.
        with tempfile.TemporaryDirectory(prefix="ee-native-iop-rendezvous-") as tmp:
            root = Path(tmp)
            elf = root / "ee-sif-wait.elf"
            irx = root / "iop-sifman.irx"
            make_sif_handshake_elf(elf)
            make_sif_guest_irx(irx)
            report_path = root / "ee-iop.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=48", "--hle-startup", "--trace",
                "--sif-poll-limit=6", "--report", str(report_path),
            ]

            without_iop = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            without_state = json.loads(
                report_path.read_text(encoding="utf-8")
            )
            assert without_iop.returncode == 3, (
                without_iop.stdout + without_iop.stderr
            )
            assert without_state["final_pc"] == 0x00100010, without_state
            assert "SIF SMFLAG polling stalled" in without_iop.stderr
            assert "IOP guest loaded:" not in without_iop.stdout

            with_iop = subprocess.run(
                command + [
                    "--no-recompile", f"--iop-irx={irx}",
                    "--iop-quantum=64",
                ],
                capture_output=True, text=True, check=False,
            )
            with_state = json.loads(
                report_path.read_text(encoding="utf-8")
            )
            assert with_state["gpr_lo"][16] == 0x60000, (
                with_state, with_iop.stdout, with_iop.stderr
            )
            assert "IOP guest loaded:" in with_iop.stdout
            assert (
                "IOP guest published SMFLAG: value=0x00060000"
            ) in with_iop.stdout
            assert "HLE SifGetReg: register=0x00000004 result=0x00060000" in (
                with_iop.stdout
            ), with_iop.stdout
            assert "SIF SMFLAG polling stalled" not in with_iop.stderr
            assert with_state["dispatches"] < 48, with_state

            # The kernel's real IRX resolver refuses an unsupported import.
            invalid_irx = root / "bad-import.irx"
            make_sif_guest_irx(invalid_irx, unknown_import=True)
            missing_import = subprocess.run(
                command + ["--no-recompile", f"--iop-irx={invalid_irx}"],
                capture_output=True, text=True, check=False,
            )
            assert missing_import.returncode == 1, (
                missing_import.stdout + missing_import.stderr
            )
            assert "IOP IRX load failed:" in missing_import.stderr
            assert "IOP guest published SMFLAG:" not in missing_import.stdout

            # An IOP thread that traps on an unknown syscall never
            # synthesizes readiness or resumes the EE unconditionally.
            unknown_service_irx = root / "bad-syscall.irx"
            make_sif_guest_irx(unknown_service_irx, unhandled_syscall=True)
            unknown_service = subprocess.run(
                command + [
                    "--no-recompile", f"--iop-irx={unknown_service_irx}",
                    "--iop-quantum=64",
                ],
                capture_output=True, text=True, check=False,
            )
            assert unknown_service.returncode != 0, (
                unknown_service.stdout + unknown_service.stderr
            )
            assert "IOP guest stopped: reason=2" in unknown_service.stderr
            assert "IOP guest published SMFLAG:" not in unknown_service.stdout

            # PR #71 exposes real guest IRX exports, and the EE host must
            # preserve their order. The provider publishes ordinal 0 as a
            # relocated guest function returning 0x60000; only the consumer
            # imports it and uses the returned value for sifman SetSMFlag.
            assets = root / "iop assets"
            assets.mkdir()
            provider = assets / "provider.irx"
            consumer = assets / "consumer.irx"
            make_iop_provider_irx(provider)
            make_iop_consumer_irx(consumer)
            manifest = root / "iop-modules.txt"
            manifest.write_text(
                "# Provider first, IOP entry module last\n"
                "iop assets/provider.irx\n"
                "\n"
                "iop assets/consumer.irx\n",
                encoding="utf-8",
            )

            for invocation in (
                [f"--iop-irx={provider}", f"--iop-irx={consumer}"],
                [f"--iop-irx-list={manifest}"],
                [f"--iop-irx-list={manifest}", "--iop-start-all"],
            ):
                linked_run = subprocess.run(
                    command + ["--no-recompile", "--iop-quantum=64",
                               *invocation],
                    capture_output=True, text=True, check=False,
                )
                linked_state = json.loads(
                    report_path.read_text(encoding="utf-8")
                )
                assert linked_state["gpr_lo"][16] == 0x60000, (
                    invocation, linked_state,
                    linked_run.stdout, linked_run.stderr
                )
                assert linked_run.stdout.count("IOP IRX linked:") == 2, (
                    invocation, linked_run.stdout
                )
                assert "IOP guest published SMFLAG: value=0x00060000" in (
                    linked_run.stdout
                ), (invocation, linked_run.stdout, linked_run.stderr)
                assert "SIF SMFLAG polling stalled" not in linked_run.stderr

            reversed_modules = subprocess.run(
                command + [
                    "--no-recompile",
                    f"--iop-irx={consumer}", f"--iop-irx={provider}",
                ],
                capture_output=True, text=True, check=False,
            )
            assert reversed_modules.returncode == 1, (
                reversed_modules.stdout + reversed_modules.stderr
            )
            assert "IOP IRX load failed: index=1" in reversed_modules.stderr
            assert "IOP guest loaded:" not in reversed_modules.stdout

            # A second copy of a provider must be refused before any guest
            # thread starts, because its export ordinals already exist.
            duplicate_module = subprocess.run(
                command + [
                    "--no-recompile",
                    f"--iop-irx={provider}", f"--iop-irx={provider}",
                ],
                capture_output=True, text=True, check=False,
            )
            assert duplicate_module.returncode == 1
            assert "IOP IRX load failed: index=2" in duplicate_module.stderr
            assert "IOP guest loaded:" not in duplicate_module.stdout

            no_manifest = subprocess.run(
                command + [
                    "--no-recompile", f"--iop-irx-list={root / 'missing.txt'}",
                ],
                capture_output=True, text=True, check=False,
            )
            assert no_manifest.returncode == 2, no_manifest
            assert "cannot read IOP IRX list" in no_manifest.stderr

            ambiguous = subprocess.run(
                command + [
                    "--no-recompile", f"--iop-irx={provider}",
                    f"--iop-irx-list={manifest}",
                ],
                capture_output=True, text=True, check=False,
            )
            assert ambiguous.returncode == 2, ambiguous
            assert "--iop-irx-list and --iop-irx are mutually exclusive" in (
                ambiguous.stderr
            )

        for invalid_argument, invalid_string in (
            (False, False), (True, False), (False, True),
        ):
            with tempfile.TemporaryDirectory(prefix="ee-native-deci2-kputs-") as tmp:
                root = Path(tmp)
                elf = root / "deci2-kputs.elf"
                make_deci2_kputs_elf(
                    elf, invalid_argument=invalid_argument,
                    invalid_string=invalid_string,
                )
                report_path = root / "deci2-kputs.json"
                command = [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=32",
                    "--hle-startup",
                    "--report", str(report_path),
                ]
                result = subprocess.run(
                    command + ["--trace"],
                    capture_output=True, text=True, check=False,
                )
                combined = result.stdout + result.stderr
                state = json.loads(report_path.read_text(encoding="utf-8"))
                assert state["exit_name"] == "EXCEPTION", state
                if invalid_argument:
                    assert state["cop0_epc"] == 0x00100018, state
                    assert (
                        "Deci2Call KPUTS HLE refused unbacked or unaligned "
                        "string pointer 0x00100201"
                    ) in result.stderr, combined
                    assert state["gpr_lo"][8] == 0, state
                    assert "EE KPUTS:" not in result.stdout, combined
                elif invalid_string:
                    assert state["cop0_epc"] == 0x00100018, state
                    assert (
                        "Deci2Call KPUTS HLE refused unterminated or unbacked "
                        "guest text 0x7ffffffc"
                    ) in result.stderr, combined
                    assert state["gpr_lo"][8] == 0, state
                    assert "EE KPUTS:" not in result.stdout, combined
                else:
                    assert state["cop0_epc"] == 0x00100024, state
                    assert "EE KPUTS: DECI2 KPUTS SUCCESS" in result.stdout, combined
                    assert (
                        'HLE Deci2Call KPUTS: text="DECI2 KPUTS SUCCESS" '
                        "bytes=19 result=1"
                    ) in result.stdout, combined
                    # KPUTS must appear without --trace as well. The
                    # native runner is reused, not rebuilt, for this check.
                    untraced = subprocess.run(
                        command + ["--no-recompile"],
                        capture_output=True, text=True, check=False,
                    )
                    assert (
                        "EE KPUTS: DECI2 KPUTS SUCCESS"
                    ) in untraced.stdout, untraced.stdout + untraced.stderr
                    assert (
                        "HLE Deci2Call KPUTS: text="
                    ) not in untraced.stdout, untraced.stdout
                    assert state["gpr_lo"][8] == 1, state
                    assert state["gpr_lo"][9] == 0, state
                    assert (
                        "Deci2Call HLE refused unsupported operation=7"
                    ) in result.stderr, combined

        for invalid_open in (False, True):
            with tempfile.TemporaryDirectory(prefix="ee-native-deci2-") as tmp:
                root = Path(tmp)
                elf = root / "deci2.elf"
                make_deci2_open_close_elf(elf, invalid_open=invalid_open)
                report_path = root / "deci2.json"
                command = [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=32", "--trace",
                    "--report", str(report_path),
                ]

                if not invalid_open:
                    # The opt-in kernel emulation must not be active by default.
                    disabled = subprocess.run(
                        command, capture_output=True, text=True, check=False,
                    )
                    assert disabled.returncode == 3, disabled.stdout + disabled.stderr
                    disabled_state = json.loads(report_path.read_text(encoding="utf-8"))
                    assert disabled_state["cop0_epc"] == 0x00100018, disabled_state
                    assert "HLE Deci2Call" not in disabled.stdout, disabled.stdout
                    command.append("--no-recompile")

                result = subprocess.run(
                    command + ["--hle-startup"],
                    capture_output=True, text=True, check=False,
                )
                assert result.returncode == 3, result.stdout + result.stderr
                state = json.loads(report_path.read_text(encoding="utf-8"))
                assert state["exit_name"] == "EXCEPTION", state
                if invalid_open:
                    assert state["cop0_epc"] == 0x00100018, state
                    assert (
                        "Deci2Call OPEN HLE refused unbacked or unaligned "
                        "descriptor 0x7ffffffc"
                    ) in result.stderr, result.stdout + result.stderr
                    assert "HLE Deci2Call OPEN:" not in result.stdout, result.stdout
                    assert state["gpr_lo"][8] == 0, state  # no fallback write
                else:
                    assert state["cop0_epc"] == 0x00100048, state
                    assert (
                        "HLE Deci2Call OPEN: descriptor=0x00100200 "
                        "channel=0x00100220 handler=0x00100060 result=1"
                    ) in result.stdout, result.stdout
                    assert "HLE Deci2Call CLOSE: result=1" in result.stdout, result.stdout
                    assert (
                        "Deci2Call HLE refused unsupported operation=7 "
                        "descriptor=0x00000000 handler=0x00000000"
                    ) in result.stderr, result.stdout + result.stderr
                    regs = state["gpr_lo"]
                    assert regs[8] == 1, state  # OPEN result
                    assert regs[9] == 1, state  # CLOSE result
                    assert regs[10] == 0, state # no fake REQSEND return
                    assert regs[16:20] == [
                        0x11223344, 0x00100220, 0x00100060, 0x55667788,
                    ], state



        for invalid_channel in (False, True):
            with tempfile.TemporaryDirectory(prefix="ee-native-deci2-reqsend-") as tmp:
                root = Path(tmp)
                elf = root / "deci2-reqsend.elf"
                make_deci2_reqsend_elf(elf, invalid_channel=invalid_channel)
                report_path = root / "deci2-reqsend.json"
                result = subprocess.run(
                    [
                        sys.executable, str(source / "tools" / "run_game.py"),
                        str(elf), "--build-dir", str(build),
                        "--work-dir", str(root / "out"),
                        "--max-dispatches=48",
                        "--hle-startup", "--trace",
                        "--watch-word=0x0010022c",
                        "--report", str(report_path),
                    ],
                    capture_output=True, text=True, check=False,
                )
                assert result.returncode == 3, result.stdout + result.stderr
                state = json.loads(report_path.read_text(encoding="utf-8"))
                assert state["exit_name"] == "EXCEPTION", state
                registers = state["gpr_lo"]
                assert registers[8] == 1, state  # OPEN returned 1
                if invalid_channel:
                    assert state["cop0_epc"] == 0x00100024, state
                    assert (
                        "Deci2Call REQSEND HLE refused unbacked or "
                        "unaligned channel 0x7ffffffc"
                    ) in result.stderr, result.stdout + result.stderr
                    assert registers[9] == 0, state
                    assert "EE watch changed:" not in result.stdout, result.stdout
                else:
                    assert state["cop0_epc"] == 0x00100080, state
                    assert (
                        "HLE Deci2Call REQSEND: channel=0x00100220 "
                        "packet_ptr=0x00100260 packet_size=16 "
                        "pending_before=0xcafebabe pending_after=0 result=1"
                    ) in result.stdout, result.stdout
                    assert (
                        "HLE Deci2Call REQSEND: no active channel result=1"
                    ) in result.stdout, result.stdout
                    for operation in ("POLL", "EXRECV", "EXSEND"):
                        assert (
                            f"HLE Deci2Call {operation}: result=1 "
                            "(debug-only transport)"
                        ) in result.stdout, result.stdout
                    assert "HLE Deci2Call CLOSE: result=1" in result.stdout
                    assert (
                        "Deci2Call HLE refused unsupported operation=7 "
                        "descriptor=0x00000000 handler=0x00000000"
                    ) in result.stderr, result.stdout + result.stderr
                    assert registers[9] == 1, state     # REQSEND
                    assert registers[10] == 1, state    # CLOSE
                    assert registers[12] == 1, state    # unregistered REQSEND
                    assert registers[14] == 1, state    # POLL
                    assert registers[15] == 1, state    # EXRECV
                    assert registers[24] == 1, state    # EXSEND
                    assert registers[16] == 0, state    # channel[3] cleared
                    assert registers[17] == 16, state   # size unchanged
                    assert registers[18] == 0x00100260, state  # pointer unchanged
                    assert registers[13] == 0, state    # no unsupported fallthrough
                    # The existing --watch-word probe compares the RAM
                    # before/after *translated* native callbacks only; this
                    # HLE mutation occurs between callbacks. Verify it via
                    # both the HLE trace and real guest LW result above.
                    assert (
                        "EE watch initial: addr=0x0010022c "
                        "value=0xcafebabe"
                    ) in result.stdout, result.stdout
                    assert "EE watch changed:" not in result.stdout, result.stdout

    if shard is None or shard == 2:
        with tempfile.TemporaryDirectory(prefix="ee-native-word-watch-") as tmp:
            root = Path(tmp)
            elf = root / "word-watch.elf"
            make_guest_word_watch_elf(elf)
            args = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--trace-entry=0x00100008",
                "--watch-word=0x00100200",
                "--trace-v0=0x00100008:0x0010001c",
                "--trace-store=0x00100200",
            ]
            result = subprocess.run(args, capture_output=True, text=True,
                                    check=False)
            assert result.returncode == 3, result.stdout + result.stderr
            assert "EE guest-store watch: addr=0x00100200 max_events=64" in result.stdout, result.stdout
            assert ("EE guest-store: pc=0x00100014 opcode=0xad090000 ins=SW "
                    "effective=0x00100200 affected=0x00100200 width=4 "
                    "src=gpr9 lane=0 source_value=0x0000000000000007 "
                    "base=gpr8 base_value=0x0000000000100200 "
                    "sp=0x01fff000 fp=0x00000000 ra=0x00000000") in result.stdout, result.stdout
            assert result.stdout.count("EE guest-store:") == 1, result.stdout
            assert "EE guest-v0 watch: first_pc=0x00100008 last_pc=0x0010001c max_events=64" in result.stdout, result.stdout
            assert ("EE guest-v0-write: pc=0x0010001c opcode=0x24020007 "
                    "old=0x0000000000000000 new=0x0000000000000007 "
                    "rs=0 rt=2 rd=0") in result.stdout, result.stdout
            assert result.stdout.count("EE guest-v0-write:") == 1, result.stdout
            assert "EE watch initial: addr=0x00100200 value=0x00000000" in result.stdout, result.stdout
            assert "EE trace-entry: dispatch=1 pc=0x00100008" in result.stdout, result.stdout
            assert "sp=0x01fff000" in result.stdout, result.stdout
            assert "EE trace-entry saved: s0=00000000" in result.stdout, result.stdout
            assert "s3=00000000" in result.stdout, result.stdout
            assert "fp=00000000" in result.stdout, result.stdout
            assert result.stdout.count("EE trace-entry stack-pair:") == 8, result.stdout
            assert ("EE trace-entry stack-pair: index=0 addr=0x01fff010 "
                    "word0=0x00000000 word1=0x00000000") in result.stdout, result.stdout
            assert "EE watch changed: dispatch=1 from_pc=0x00100008 " in result.stdout, result.stdout
            assert "addr=0x00100200 old=0x00000000 new=0x00000007" in result.stdout, result.stdout
            assert ("EE watch registers: s3_before=00000000 "
                    "s3_after=00000000") in result.stdout, result.stdout
            assert "fp_before=00000000 fp_after=00000000" in result.stdout, result.stdout
            assert ("EE watch pair: addr=0x001001fc "
                    "word0=0x00000000 word1=0x00000007") in result.stdout, result.stdout

            # The same compiled module must skip both trace helpers when
            # disabled, while preserving the guest word and register result.
            untraced = subprocess.run(
                args[:-2] + ["--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert untraced.returncode == 3, untraced.stdout + untraced.stderr
            assert "EE guest-store:" not in untraced.stdout, untraced.stdout
            assert "EE guest-v0-write:" not in untraced.stdout, untraced.stdout
            assert "addr=0x00100200 old=0x00000000 new=0x00000007" in untraced.stdout, untraced.stdout

            rejected = subprocess.run(
                args[:-2] + ["--watch-word=0x02000000", "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert rejected.returncode == 2, rejected.stdout + rejected.stderr
            assert "watch-word address is not backed: 0x02000000" in rejected.stderr, (
                rejected.stdout + rejected.stderr
            )

            # A guest-store watch does not generate a false event for a
            # different word, and its own guest address must be backed.
            nearby = subprocess.run(
                args[:-1] + ["--trace-store=0x00100204", "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert nearby.returncode == 3, nearby.stdout + nearby.stderr
            assert "EE guest-store:" not in nearby.stdout, nearby.stdout
            invalid_store = subprocess.run(
                args[:-1] + ["--trace-store=0x02000000", "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert invalid_store.returncode == 2, invalid_store.stdout + invalid_store.stderr
            assert "trace-store address is not backed: 0x02000000" in invalid_store.stderr, (
                invalid_store.stdout + invalid_store.stderr
            )

            excluded_v0 = subprocess.run(
                args[:-2] + ["--trace-v0=0x00100008:0x00100018", "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert excluded_v0.returncode == 3, excluded_v0.stdout + excluded_v0.stderr
            assert "EE guest-v0-write:" not in excluded_v0.stdout, excluded_v0.stdout

            bad_v0_window = subprocess.run(
                args[:-2] + ["--trace-v0=0x0010001c:0x00100008", "--no-recompile"],
                capture_output=True, text=True, check=False,
            )
            assert bad_v0_window.returncode == 2, bad_v0_window.stdout + bad_v0_window.stderr
            assert "ordered range" in bad_v0_window.stderr, (
                bad_v0_window.stdout + bad_v0_window.stderr
            )



        with tempfile.TemporaryDirectory(prefix="ee-native-heap-boundary-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-heap.elf"
            make_heap_boundary_elf(elf)
            report_path = root / "heap.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=32",
                "--trace",
                "--report", str(report_path),
            ]

            without_hle = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert without_hle.returncode != 0, (
                without_hle.stdout + without_hle.stderr
            )
            refused = json.loads(report_path.read_text(encoding="utf-8"))
            assert refused["cop0_epc"] == 0x0010000c, refused
            assert "HLE EndOfHeap:" not in without_hle.stdout

            with_hle = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert with_hle.returncode == 3, with_hle.stdout + with_hle.stderr
            for trace_line in (
                "HLE EndOfHeap: id=1 end=0x00000000",
                "HLE SetupHeap: base=0x00640380 end=0x01ffb000 size=0xffffffff",
                "HLE EndOfHeap: id=1 end=0x01ffb000",
                "HLE SetupHeap: base=0x00640000 end=0x00642000 size=0x00002000",
                "HLE EndOfHeap: id=1 end=0x00642000",
            ):
                assert trace_line in with_hle.stdout, (
                    trace_line, with_hle.stdout, with_hle.stderr
                )
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["snapshot_kind"] == "stopped", state
            assert state["final_pc"] == 0, state
            assert state["exit_name"] == "DISPATCH", state
            regs = state["gpr_lo"]
            assert regs[8] == 0, state            # initially unconfigured
            assert regs[9] == 0x01fff000, state   # SetupThread result
            assert regs[10] == 0x01ffb000, state # SetupHeap(-1) result
            assert regs[11] == 0x01ffb000, state # EndOfHeap
            assert regs[12] == 0x00642000, state # SetupHeap(size) result
            assert regs[13] == 0x00642000, state # EndOfHeap updated
            assert regs[14] == 0x00642000, state # EndOfHeap ignores A0

        with tempfile.TemporaryDirectory(prefix="ee-native-shared-heap-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-shared-heap.elf"
            make_shared_heap_elf(elf)
            report_path = root / "shared-heap.json"
            result = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=100", "--auto-expand=4",
                "--trace", "--report", str(report_path), "--hle-startup",
            ], capture_output=True, text=True, check=False)
            combined = result.stdout + result.stderr
            assert "HLE EE thread switch: id=2 pc=0x001000f0" in combined, combined
            assert "HLE EndOfHeap: id=2 end=0x01ffb000" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            regs = state["gpr_lo"]
            assert regs[16:20] == [0x01ffb000, 2, 0x01ffb000, 0x01ffb000], state
            assert state["final_pc"] == 0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-sleep-wakeup-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-sleep-wakeup.elf"
            make_thread_sleep_wakeup_elf(elf)
            report_path = root / "sleep-wakeup.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=160",
                "--auto-expand=4",
                "--trace",
                "--report", str(report_path),
                "--hle-startup",
            ]
            result = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            combined = result.stdout + result.stderr
            assert "HLE CreateThread: id=2" in combined, combined
            assert "HLE WakeupThread: id=1 invalid status/context, result=-1" in combined, combined
            assert "HLE WakeupThread: id=2 invalid status/context, result=-1" in combined, combined
            assert "HLE WakeupThread: id=1 status=2 wakeups=1 released=0" in combined, combined
            assert "HLE SleepThread: id=1 consumed wakeup remaining=0" in combined, combined
            assert "HLE StartThread: id=2" in combined, combined
            assert "HLE SleepThread: id=1 status=4" in combined, combined
            assert "HLE EE thread switch: id=2 pc=0x001000f0" in combined, combined
            assert "HLE iWakeupThread: id=1 status=2 wakeups=0 released=1" in combined, combined
            assert "HLE EE thread switch: id=1" in combined, combined
            assert "HLE CancelWakeupThread: id=1 status=1 wakeups=0" in combined, combined
            assert "HLE WakeupThread: id=99 not found, result=-1" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            registers = state["gpr_lo"]
            assert registers[16] == 2, state  # worker created
            assert registers[17] == 0xffffffffffffffff, state  # self wakeup rejected
            assert registers[18] == 1, state  # returned from genuine sleep
            assert registers[19] == 1, state  # credit consumed without block
            assert registers[22] == 0xffffffffffffffff, state  # dormant wakeup rejected
            assert registers[20] == 0, state  # no leftover wakeup credit
            assert registers[21] == 0xffffffffffffffff, state # invalid ID
            assert state["final_pc"] != 0x001000f0, state # no worker escape

        with tempfile.TemporaryDirectory(prefix="ee-native-release-wait-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-release-wait.elf"
            make_thread_release_wait_elf(elf)
            report_path = root / "release-wait.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=160",
                "--auto-expand=4",
                "--trace",
                "--report", str(report_path),
                "--hle-startup",
            ]
            result = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            combined = result.stdout + result.stderr
            # The expected jr $zero stop is reported as a nonzero host exit.
            for expected_line in (
                "HLE SleepThread: id=1 status=4",
                "HLE ReleaseWaitThread: id=1 old_status=4 new_status=2",
                "HLE SuspendThread: id=1 old_status=4 new_status=12",
                "HLE iReleaseWaitThread: id=1 old_status=12 new_status=8",
                "HLE ResumeThread: id=1 old_status=8 new_status=2",
                "HLE ReleaseWaitThread: id=2 not waiting, result=-1",
            ):
                assert expected_line in combined, (expected_line, combined)
            state = json.loads(report_path.read_text(encoding="utf-8"))
            registers = state["gpr_lo"]
            assert registers[16] == 2, state
            assert registers[17] == 1, state
            assert registers[18] == 1, state
            assert registers[19] == 0xffffffffffffffff, state
            assert state["final_pc"] == 0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-semaphore-release-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-semaphore-release.elf"
            make_thread_semaphore_release_wait_elf(elf)
            report_path = root / "semaphore-release.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=160",
                "--auto-expand=4",
                "--trace",
                "--report", str(report_path),
                "--hle-startup",
            ]
            result = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            combined = result.stdout + result.stderr
            # The native runner reports a nonzero stop when synthetic MIPS
            # exits through jr $zero. Verify guest state, not process code.
            assert "HLE CreateSema: id=1 init=0 max=1" in combined, combined
            assert "HLE ReleaseWaitThread: id=1 old_status=4 new_status=2" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            registers = state["gpr_lo"]
            assert registers[16] == 2, state  # worker created
            assert registers[17] == 1, state  # semaphore created
            assert registers[18] == 1, state  # WaitSema completed
            assert registers[19] == 0, state  # no stale waiters
            assert registers[20] == 1, state  # PollSema consumed new signal
            assert state["final_pc"] == 0, state

        for negative_service in (-0x31, -0x2f):
            with tempfile.TemporaryDirectory(prefix="ee-native-irq-status-") as tmp:
                root = Path(tmp)
                elf = root / "kernel-irq-status.elf"
                make_interrupt_status_encoding_elf(elf, negative_service)
                report_path = root / "irq-status.json"
                result = subprocess.run(
                    [
                        sys.executable, str(source / "tools" / "run_game.py"),
                        str(elf), "--build-dir", str(build),
                        "--work-dir", str(root / "out"),
                        "--max-dispatches=32", "--hle-startup", "--trace",
                        "--report", str(report_path),
                    ],
                    capture_output=True, text=True, check=False,
                )
                combined = result.stdout + result.stderr
                state = json.loads(report_path.read_text(encoding="utf-8"))
                assert state["exit_name"] == "EXCEPTION", (negative_service, state)
                assert state["cop0_epc"] == 0x00100020, (negative_service, state)
                assert state["gpr_lo"][16] == 1, (negative_service, state)
                assert state["gpr_lo"][17] == 0, (negative_service, state)
                assert "HLE iReferThreadStatus: id=1 address=0x00000000 status=1" in combined, combined

        with tempfile.TemporaryDirectory(prefix="ee-native-suspend-resume-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-suspend-resume.elf"
            make_thread_suspend_resume_elf(elf)
            report_path = root / "suspend-resume.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=100",
                "--trace",
                "--report", str(report_path),
                "--hle-startup",
            ]
            result = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            combined = result.stdout + result.stderr
            assert "HLE CreateThread: id=2" in combined, combined
            assert "HLE StartThread: id=2" in combined, combined
            assert "HLE SuspendThread: id=2 old_status=2 new_status=8" in combined, combined
            assert "HLE ResumeThread: id=2 old_status=8 new_status=2" in combined, combined
            assert "HLE SuspendThread: id=99 not found, result=-1" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            regs = state["gpr_lo"]
            assert regs[16] == 2, state  # created ID
            assert regs[17] == 2, state  # initially READY
            assert regs[18] == 2, state  # SuspendThread return
            assert regs[19] == 8, state  # state genuinely suspended
            assert regs[20] == 2, state  # ResumeThread return
            assert regs[21] == 2, state  # state restored READY
            assert regs[22] == 0xffffffffffffffff, state # invalid ID
            assert "HLE EE thread switch: id=2" not in combined, combined
            assert state["final_pc"] != 0x001000f0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-self-suspend-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-self-suspend.elf"
            make_self_suspend_elf(elf)
            report_path = root / "self-suspend.json"
            result = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"), "--max-dispatches=100",
                "--trace", "--report", str(report_path), "--hle-startup",
            ], capture_output=True, text=True, check=False)
            combined = result.stdout + result.stderr
            assert "HLE EE self-suspend reconciled: id=1" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["gpr_lo"][16:19] == [1, 8, 1], state
            assert state["final_pc"] == 0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-thread-lifecycle-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-lifecycle.elf"
            make_thread_lifecycle_elf(elf)
            report_path = root / "lifecycle.json"
            result = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(root / "out"),
                "--max-dispatches=100", "--trace", "--report", str(report_path),
                "--hle-startup",
            ], capture_output=True, text=True, check=False)
            combined = result.stdout + result.stderr
            assert "HLE SetSyscall:" not in combined, combined
            assert "HLE EE thread switch: id=2" not in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            regs = state["gpr_lo"]
            failure = 0xffffffffffffffff
            assert regs[16:26] == [failure, 2, failure, 2, failure, 2, 2, 2, 2, failure], state
            assert regs[15] == 2, state

        with tempfile.TemporaryDirectory(prefix="ee-native-thread-priority-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-thread-priority.elf"
            make_thread_priority_elf(elf)
            report_path = root / "thread-priority.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=32",
                "--trace",
                "--report", str(report_path),
            ]

            without_hle = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert without_hle.returncode != 0, (
                without_hle.stdout + without_hle.stderr
            )
            refused = json.loads(report_path.read_text(encoding="utf-8"))
            assert refused["cop0_epc"] == 0x0010000c, refused
            assert "HLE ChangeThreadPriority:" not in without_hle.stdout

            with_hle = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert "HLE ChangeThreadPriority: id=1 old=0 new=1" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE ChangeThreadPriority: id=1 old=1 new=5" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE iChangeThreadPriority: id=2 old=64 new=2" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE ChangeThreadPriority: id=2 old=2 new=7" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE ChangeThreadPriority: id=99 not found, result=-1" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            state = json.loads(report_path.read_text(encoding="utf-8"))
            registers = state["gpr_lo"]
            assert registers[8] == 1, state   # GetThreadId before changes
            assert registers[9] == 0, state   # old main priority
            assert registers[10] == 1, state  # old priority using ID alias 0
            assert registers[11] == 2, state  # dormant CreateThread result
            assert registers[12] == 64, state # old dormant priority
            assert registers[13] == 2, state  # masked priority persisted
            assert registers[14] == 0xFFFFFFFFFFFFFFFF, state  # invalid thread
            assert registers[15] == 1, state  # GetThreadId remains main
            assert state["final_pc"] != 0x001000f0, state # no child ran

        with tempfile.TemporaryDirectory(prefix="ee-native-get-thread-id-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-get-thread-id.elf"
            make_get_thread_id_elf(elf)
            report_path = root / "get-thread-id.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16",
                "--trace",
                "--report", str(report_path),
            ]

            without_hle = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert without_hle.returncode != 0, (
                without_hle.stdout + without_hle.stderr
            )
            refused = json.loads(report_path.read_text(encoding="utf-8"))
            assert refused["cop0_epc"] == 0x0010000c, refused
            assert "HLE GetThreadId:" not in without_hle.stdout, (
                without_hle.stdout + without_hle.stderr
            )

            with_hle = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert with_hle.stdout.count("HLE GetThreadId: id=1") == 2, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE CreateThread: id=2" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["gpr_lo"][8] == 1, state  # running main thread
            assert state["gpr_lo"][9] == 2, state  # new dormant thread
            assert state["gpr_lo"][10] == 1, state # main remains current
            assert state["gpr_lo"][2] == 1, state  # GetThreadId return value
            assert state["gpr_lo"][4] == 0x00100200, state # ignores a0
            assert state["final_pc"] != 0x001000f0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-create-thread-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-create-thread.elf"
            make_create_thread_elf(elf)
            report_path = root / "create-thread.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=16",
                "--trace",
                "--report", str(report_path),
            ]

            without_hle = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            assert without_hle.returncode != 0, (
                without_hle.stdout + without_hle.stderr
            )
            refused = json.loads(report_path.read_text(encoding="utf-8"))
            assert refused["cop0_epc"] == 0x00100014, refused
            assert "HLE CreateThread:" not in without_hle.stdout, (
                without_hle.stdout + without_hle.stderr
            )

            with_hle = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True, text=True, check=False,
            )
            assert with_hle.returncode != 0, (
                with_hle.stdout + with_hle.stderr
            )
            assert ("HLE CreateThread: id=2 entry=0x001000f0 "
                    "stack=0x00102000 size=0x00000800 gp=0x0010f000 "
                    "priority=64 attr=0x0000abcd option=0xaabbccdd "
                    "parameters=0x00100200 dormant") in with_hle.stdout, (
                        with_hle.stdout + with_hle.stderr
                    )
            assert ("HLE CreateThread: id=3 entry=0x001000f0 "
                    "stack=0x00102000 size=0x00000800 gp=0x0010f000 "
                    "priority=64 attr=0x0000abcd option=0xaabbccdd "
                    "parameters=0x00100200 dormant") in with_hle.stdout, (
                        with_hle.stdout + with_hle.stderr
                    )
            assert ("CreateThread HLE refused unbacked or unaligned "
                    "parameter block 0x00100201") in with_hle.stderr, (
                        with_hle.stdout + with_hle.stderr
                    )
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["cop0_epc"] == 0x0010003c, state
            assert state["gpr_lo"][8] == 2, state
            assert state["gpr_lo"][9] == 3, state
            assert state["gpr_lo"][10] == 0, state
            assert state["gpr_lo"][2] == 3, state
            assert state["final_pc"] != 0x001000f0, state

        with tempfile.TemporaryDirectory(prefix="ee-native-semaphore-") as tmp:
            root = Path(tmp)
            elf = root / "semaphore-mips.elf"
            make_semaphore_elf(elf)
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--hle-startup",
                    "--trace",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE CreateSema: id=1 init=1 max=2" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "EE stopped: dispatched=2" in result.stdout, (
                result.stdout + result.stderr
            )

        with tempfile.TemporaryDirectory(prefix="ee-native-flushcache-") as tmp:
            root = Path(tmp)
            elf = root / "flush-cache-mips.elf"
            make_flush_cache_elf(elf)
            report = root / "flush-cache.json"
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--report", str(report),
                "--trace",
            ]
            without_hle = subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
            )
            assert without_hle.returncode != 0, without_hle.stdout + without_hle.stderr
            initial = json.loads(report.read_text(encoding="utf-8"))
            assert initial["cop0_epc"] == 0x00100010, initial
            assert initial["gpr_lo"][2] == 123, initial
            assert "HLE FlushCache" not in without_hle.stdout, without_hle.stdout

            with_hle = subprocess.run(
                command + ["--no-recompile", "--hle-startup"],
                capture_output=True,
                text=True,
                check=False,
            )
            assert with_hle.returncode != 0, with_hle.stdout + with_hle.stderr
            assert "HLE FlushCache: mode=0x00000000 result=0" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            assert "HLE iFlushCache: mode=0x00000000 result=0" in with_hle.stdout, (
                with_hle.stdout + with_hle.stderr
            )
            stopped = json.loads(report.read_text(encoding="utf-8"))
            assert stopped["cop0_epc"] == 0x0010002c, stopped
            assert stopped["gpr_lo"][8] == 0, stopped  # first service return
            assert stopped["gpr_lo"][9] == 0, stopped  # second service return
            assert stopped["gpr_lo"][10] == 0, stopped  # unknown call never returns
            assert stopped["gpr_lo"][2] == 0, stopped
            assert stopped["gpr_lo"][3] == 0x65, stopped
            assert "guest blocked by an unsupported operation" in with_hle.stderr, (
                with_hle.stdout + with_hle.stderr
            )

        with tempfile.TemporaryDirectory(prefix="ee-native-syscall-hook-") as tmp:
            root = Path(tmp)
            elf = root / "set-syscall-mips.elf"
            make_set_syscall_elf(elf)
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--hle-startup",
                    "--trace",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE SetSyscall: number=0x83 handler=0x00100030" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "EE stopped: dispatched=2" in result.stdout, (
                result.stdout + result.stderr
            )

        for status, expected_target in (
            (2, 0x001000c0),  # EXL: ordinary EPC
            (4, 0x00100080),  # ERL: ErrorEPC takes precedence
        ):
            with tempfile.TemporaryDirectory(prefix="ee-native-eret-") as tmp:
                root = Path(tmp)
                elf = root / "eret.elf"
                make_eret_target_elf(elf, status)
                report_path = root / "eret.json"
                result = subprocess.run(
                    [
                        sys.executable, str(source / "tools" / "run_game.py"),
                        str(elf),
                        "--build-dir", str(build),
                        "--work-dir", str(root / "out"),
                        "--max-dispatches=16",
                        "--trace",
                        "--report", str(report_path),
                    ],
                    capture_output=True, text=True, check=False,
                )
                assert result.returncode != 0, result.stdout + result.stderr
                assert (
                    f"dispatch=1 pc=0x00100008 -> 0x{expected_target:08x} "
                    "reason=DISPATCH(3) status=1"
                ) in result.stdout, result.stdout + result.stderr
                report = json.loads(report_path.read_text(encoding="utf-8"))
                assert report["dispatches"] == 2, report
                assert report["final_pc"] == 0, report
                assert report["gpr_lo"][11] == 0, report
                assert report["gpr_lo"][12] == 0x00100080, report
                assert report["gpr_lo"][13] == 0x001000c0, report
                assert report["gpr_lo"][15] == 0, report

        with tempfile.TemporaryDirectory(prefix="ee-native-overlapping-entry-") as tmp:
            root = Path(tmp)
            elf = root / "shared-successor.elf"
            make_overlapping_entry_elf(elf)
            report_path = root / "shared-successor.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=64",
                    "--auto-expand=2",
                    "--live-full-relink",
                    "--hle-startup",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True, text=True, check=False,
                env=dict(os.environ, SDBZ_EE_DIRECT_DISPATCH="1"),
            )
            assert result.returncode != 0, result.stdout + result.stderr
            assert "EE direct dispatch: enabled" in result.stdout, result.stdout
            assert "with replacement=" in result.stdout, result.stdout
            assert "auto-expand: compiling reachable PC 0x00100050" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "auto-expand: compiling reachable PC 0x00100054" in result.stdout, (
                result.stdout + result.stderr
            )
            assert ("auto-expand: preserving existing ownership of 1 "
                    "overlapping guest block PC(s)") in result.stdout, (
                result.stdout + result.stderr
            )
            assert result.stdout.count("HLE FlushCache: mode=0x00000000 result=0") >= 2, (
                result.stdout + result.stderr
            )

            metadata = json.loads(
                (root / "out" / "ir" / "ee_functions.json").read_text(encoding="utf-8")
            )
            owners = [
                int(pc, 0) for function in metadata["functions"]
                for pc in function["block_pcs"]
            ]
            assert len(owners) == len(set(owners)), metadata
            assert owners.count(0x00100050) == 1, metadata
            assert owners.count(0x00100054) == 1, metadata
            assert owners.count(0x00100058) == 1, metadata
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["dispatches"] >= 7, state
            assert state["final_pc"] == 0, state
            assert state["gpr_lo"][9] == 0, state  # second real syscall return
            assert state["gpr_lo"][16] == 0, state  # first real syscall return

        with tempfile.TemporaryDirectory(prefix="ee-native-interrupt-masks-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-interrupt-masks.elf"
            make_interrupt_mask_elf(elf)
            report_path = root / "masks.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=64",
                    "--hle-startup",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True, text=True, check=False,
            )
            assert result.returncode != 0, result.stdout + result.stderr
            for name in (
                "EnableIntc", "DisableIntc", "EnableDmac", "DisableDmac",
            ):
                assert f"HLE {name}" in result.stdout, result.stdout + result.stderr
            registers = json.loads(report_path.read_text(encoding="utf-8"))["gpr_lo"]
            assert registers[8:16] == [1, 0, 1, 0, 1, 1, 1, 0], registers
            assert registers[16:22] == [1, 0, 1, 1, 1, 1], registers
            assert registers[22] == (1 << 11), registers  # INTC_MASK
            assert registers[23] == (1 << 18), registers  # DMAC D_STAT
            assert "interrupt mask HLE refused" not in result.stderr, result.stderr

        with tempfile.TemporaryDirectory(prefix="ee-native-interrupt-handlers-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-interrupt-handlers.elf"
            make_interrupt_handlers_elf(elf)
            report_path = root / "handlers.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=32",
                    "--hle-startup",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True, text=True, check=False,
            )
            assert result.returncode != 0, result.stdout + result.stderr
            assert "EE stopped: dispatched=" in result.stdout, (
                result.stdout + result.stderr
            )
            for item in (
                "HLE AddIntcHandler: id=1 cause=11 handler=0x00100090 "
                "next=0xffffffff arg=0x00000066 gp=0x12340000",
                "HLE AddIntcHandler: id=2 cause=11 handler=0x001000a0 "
                "next=0x00000000 arg=0x00000077 gp=0x12340000",
                "HLE AddDmacHandler: id=3 cause=2 handler=0x001000b0 "
                "next=0xffffffff arg=0x00000088 gp=0x12340000",
                "HLE RemoveIntcHandler: cause=11 id=2 remaining=1",
                "HLE RemoveIntcHandler: cause=11 id=99 remaining=1",
                "HLE RemoveIntcHandler: cause=11 id=1 remaining=0",
                "HLE RemoveDmacHandler: cause=2 id=3 remaining=0",
            ):
                assert item in result.stdout, (item, result.stdout + result.stderr)
            registers = json.loads(report_path.read_text(encoding="utf-8"))["gpr_lo"]
            assert registers[8:14] == [1, 2, 3, 1, 0, 0], registers
            assert registers[16] == 1, registers

        with tempfile.TemporaryDirectory(prefix="ee-native-syscall-out-of-range-") as tmp:
            root = Path(tmp)
            elf = root / "out-of-range-syscalls.elf"
            make_out_of_range_set_syscall_elf(elf)
            report_path = root / "out-of-range.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=16",
                    "--hle-startup",
                    "--kseg-ram",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE SetSyscall: number=0x83 handler=0x00100050" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "HLE SetSyscall: unsupported number=0x0000012c handler=0x80076488 (ignored)" in result.stderr, (
                result.stdout + result.stderr
            )
            assert "HLE SetSyscall: unsupported number=0xffffffff handler=0x80076488 (ignored)" in result.stderr, (
                result.stdout + result.stderr
            )
            assert "EE stopped: dispatched=" in result.stdout, (
                result.stdout + result.stderr
            )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            registers = report["gpr_lo"]
            assert registers[8] == 0x00100050, report
            assert registers[9] == 0, report  # 0x12C returns zero
            assert registers[11] == 0x00100050, report
            assert registers[12] == 0, report  # past table untouched
            assert registers[13] == 0, report  # huge number returns zero
            assert registers[14] == 0x00100050, report
            assert registers[15] == 0, report  # still untouched
            assert registers[2] == 0, report

        with tempfile.TemporaryDirectory(prefix="ee-native-syscall-table-") as tmp:
            root = Path(tmp)
            elf = root / "guest-syscall-table.elf"
            make_guest_syscall_table_elf(elf)
            report_path = root / "kernel-table.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=24",
                    "--hle-startup",
                    "--kseg-ram",
                    "--trace",
                    "--report", str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "HLE SetSyscall: number=0x83 handler=0x00100050" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "HLE SetSyscall: number=0x5a handler=0x00100030" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "HLE SetSyscall: number=0x83 handler=0x00100030" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "EE stopped: dispatched=" in result.stdout, (
                result.stdout + result.stderr
            )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            registers = report["gpr_lo"]
            assert registers[8] == 0, report  # uninitialized table starts empty
            assert registers[9] == 0x00100050, report  # 0x83 first installation
            assert registers[11] == 0x00100050, report  # physical alias
            assert registers[12] == 0x00100030, report  # 0x5A at 0x1968
            assert registers[13] == 0x00100050, report  # 0x83 unchanged by 0x5A
            assert registers[14] == 0x00100030, report  # 0x83 replacement
            assert registers[15] == 0, report  # untouched 0x84 at 0x1A10

    if shard is None or shard == 3:
        with tempfile.TemporaryDirectory(prefix="ee-native-kernel-ram-handler-") as tmp:
            root = Path(tmp)
            for invoke in (False, True):
                elf = root / ("invoke-ram-handler.elf" if invoke else "clear-ram-handler.elf")
                make_kernel_ram_handler_elf(elf, invoke=invoke)
                report_path = root / "ram-handler.json"
                result = subprocess.run(
                    [
                        sys.executable, str(source / "tools" / "run_game.py"),
                        str(elf),
                        "--build-dir", str(build),
                        "--work-dir", str(root / ("invoke" if invoke else "clear")),
                        "--max-dispatches=16",
                        "--hle-startup",
                        "--kseg-ram",
                        "--trace",
                        "--report", str(report_path),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                assert "HLE SetSyscall: number=0x5b handler=0x80076000" in result.stdout, (
                    result.stdout + result.stderr
                )
                report = json.loads(report_path.read_text(encoding="utf-8"))
                # MIPS LW sign-extends KSEG0 pointers into 64-bit GPRs.
                assert report["gpr_lo"][8] & 0xffffffff == 0x80076000, report
                assert report["gpr_lo"][9] == 0x03e00008, report
                if invoke:
                    assert "HLE guest RAM syscall returned: entry=0x80076000" in result.stdout, (
                        result.stdout + result.stderr
                    )
                    assert report["gpr_lo"][13] == 42, report
                    events = report["syscall_history"]
                    assert len(events) == 1 and events[0]["service"] == 0x5b, report
                    assert events[0]["handler"] == 0x80076000, report
                    assert events[0]["v0"] == 42 and events[0]["returned"], report
                else:
                    assert "HLE SetSyscall: number=0x5b handler=0x00000000" in result.stdout, (
                        result.stdout + result.stderr
                    )
                    assert report["gpr_lo"][13] == 0, report

        with tempfile.TemporaryDirectory(prefix="ee-native-registered-callback-") as tmp:
            root = Path(tmp)
            elf = root / "guest-callback-mips.elf"
            make_registered_callback_elf(elf)
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=16",
                    "--auto-expand=1",
                    "--hle-startup",
                    "--trace",
                    "--report", str(root / "callback.json"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            assert "auto-expand: compiling reachable PC 0x00100050" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "HLE guest syscall: number=0x83 handler=0x00100050" in result.stdout, (
                result.stdout + result.stderr
            )
            assert "HLE guest syscall returned: resume=0x00100028 v0=0x0000000c" in result.stdout, (
                result.stdout + result.stderr
            )
            report = json.loads((root / "callback.json").read_text(encoding="utf-8"))
            assert report["gpr_lo"][8] == 12, report
            assert report["gpr_lo"][31] == 0, report
            assert report["dispatches"] >= 4, report
            events = report["syscall_history"]
            assert len(events) == 1, report
            assert events[0]["service"] == 0x83, events
            assert events[0]["handler"] == 0x00100050, events
            assert events[0]["resume_pc"] == 0x00100028, events
            assert events[0]["a0"] == 7, events
            assert events[0]["v0"] == 12, events
            assert events[0]["returned"] is True, events

        # Progress diagnostics should measure native dispatch versus JSON
        # checkpoint costs without relying on the real game ELF. A recursive
        # JAL creates 300 bounded dispatcher calls and must emit progress at
        # 256 and an end-of-run timing summary.
        with tempfile.TemporaryDirectory(prefix="ee-native-progress-profile-") as tmp:
            root = Path(tmp)
            elf = root / "dispatch-loop.elf"
            make_repeated_dispatch_elf(elf)
            report_path = root / "progress.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=300",
                    "--timeout-seconds=30",
                    "--report", str(report_path),
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=60,
            )
            assert result.returncode == 4, result.stdout + result.stderr
            snapshot = json.loads(report_path.read_text(encoding="utf-8"))
            assert snapshot["snapshot_kind"] == "stopped", snapshot
            assert snapshot["dispatches"] == 300, snapshot
            assert "EE progress: dispatched=256" in result.stderr, result.stderr
            assert "EE timing: dispatched=300" in result.stderr, result.stderr
            assert "native_ms=" in result.stderr, result.stderr
            assert "report_ms=" in result.stderr, result.stderr
            assert "slowest_native_pc=" in result.stderr, result.stderr

            # Throttle host progress text only. The runner's final state,
            # instruction counts and terminal timing must be identical.
            env = dict(os.environ, SDBZ_REPORT_PROGRESS_EVERY="1024")
            sampled_report = root / "sampled.json"
            sampled = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(root / "out"),
                "--max-dispatches=300", "--timeout-seconds=30",
                "--no-recompile", "--report", str(sampled_report),
            ], capture_output=True, text=True, check=False, timeout=60, env=env)
            assert sampled.returncode == 4, sampled.stdout + sampled.stderr
            assert "EE progress: dispatched=" not in sampled.stderr, sampled.stderr
            assert "EE timing: dispatched=300" in sampled.stderr, sampled.stderr
            sampled_state = json.loads(sampled_report.read_text(encoding="utf-8"))
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo"):
                assert sampled_state[key] == snapshot[key], key
            direct_report = root / "direct.json"
            direct_env = dict(env, SDBZ_EE_DIRECT_DISPATCH="1",
                              SDBZ_PROFILE_PCS="1", SDBZ_RUNTIME_PROFILE="1")
            direct_run = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(root / "out"),
                "--max-dispatches=300", "--timeout-seconds=30",
                "--no-recompile", "--report", str(direct_report),
            ], capture_output=True, text=True, check=False, timeout=60,
                env=direct_env)
            assert direct_run.returncode == 4, direct_run.stdout + direct_run.stderr
            assert "EE direct dispatch: enabled" in direct_run.stdout, direct_run.stdout
            assert "EE direct dispatch: executed=300" in direct_run.stderr, direct_run.stderr
            direct_state = json.loads(direct_report.read_text(encoding="utf-8"))
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo", "history"):
                assert direct_state[key] == snapshot[key], key
            profile_lines = [line for line in direct_run.stderr.splitlines()
                             if line.startswith("EE profile: ")]
            assert profile_lines and "compilation excluded" in profile_lines[0], profile_lines
            total_native_ns = int(profile_lines[0].split("native_ns=")[1])
            assert total_native_ns == direct_state["runtime_profile"]["subsystems"]["ee_native_boundary"]["inclusive_ns"]
            assert "idle_charged=0" in profile_lines[0], profile_lines
            profile_entries = [line for line in profile_lines if line.startswith("EE profile: pc=")]
            assert profile_entries, profile_lines
            times = [int(line.split("native_ns=")[1]) for line in profile_entries]
            assert times == sorted(times, reverse=True), profile_entries
            assert sum(times) == total_native_ns, profile_entries
            attempts = sum(int(line.split("non_skipped=")[1].split()[0]) for line in profile_entries)
            assert attempts == direct_state["guest_instruction_attempts"], profile_entries
            assert sampled_state["vu1_worker"]["enabled"] is False
            worker_report = root / "worker.json"
            worker_env = dict(env, SDBZ_VU1_ASYNC="1")
            worker_run = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"), str(elf),
                "--build-dir", str(build), "--work-dir", str(root / "out"),
                "--max-dispatches=300", "--timeout-seconds=30",
                "--no-recompile", "--report", str(worker_report),
            ], capture_output=True, text=True, check=False, timeout=60,
                env=worker_env)
            assert worker_run.returncode == 4, worker_run.stdout + worker_run.stderr
            worker_state = json.loads(worker_report.read_text(encoding="utf-8"))
            assert worker_state["vu1_worker"]["enabled"] is True
            assert worker_state["vu1_worker"]["pending"] is False
            assert worker_state["vu1_worker"]["submitted_jobs"] == 0
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo"):
                assert worker_state[key] == snapshot[key], key

        with tempfile.TemporaryDirectory(prefix="ee-native-loop-budget-") as tmp:
            root = Path(tmp)
            elf = root / "counting-loop.elf"
            make_counting_loop_elf(elf)
            report_path = root / "loop-budget.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--timeout-seconds=10",
                    "--loop-budget=32",
                    "--report", str(report_path),
                ],
                capture_output=True, text=True, check=False, timeout=40,
            )
            assert result.returncode == 3, result.stdout + result.stderr
            assert "guest loop budget exhausted:" in result.stderr, result.stderr
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["snapshot_kind"] == "stopped", state
            assert state["exit_reason"] == 5, state
            assert state["exit_name"] == "LOOP_BUDGET", state
            assert state["dispatches"] == 1, state
            assert state["final_pc"] == 0x00100008, state
            assert state["gpr_lo"][8] == 32, state
            assert state["guest_instruction_attempts"] > 32, state
            assert state["guest_instruction_attempts"] > state["dispatches"], state

        # The opt-in runner resumes the *same* exact compiled loop header
        # across host yields while preserving guest GPRs and the nonarchitectural
        # instruction attempt counter. No guest time or VBlank event is created.
        with tempfile.TemporaryDirectory(prefix="ee-native-resumable-yield-") as tmp:
            root = Path(tmp)
            elf = root / "counting-yield-loop.elf"
            make_counting_loop_elf(elf)
            report_path = root / "loop-yield.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--loop-budget=4",
                    "--loop-yield-resumes=2",
                    "--report", str(report_path),
                    "--timeout-seconds=10",
                    "--trace",
                ],
                capture_output=True, text=True, check=False, timeout=40,
            )
            assert result.returncode == 3, result.stdout + result.stderr
            assert result.stdout.count("EE host loop resume:") == 2, (
                result.stdout, result.stderr
            )
            assert "guest host loop yield cap reached:" in result.stderr, (
                result.stdout, result.stderr
            )
            assert "EE host loop state: pc=0x00100008" in result.stderr, (
                result.stdout, result.stderr
            )
            assert "EE host loop ELF: > 0x00100008 opcode=0x25080001" in result.stderr, (
                result.stdout, result.stderr
            )
            assert "r08=0x000000000000000c" in result.stderr, (
                result.stdout, result.stderr
            )
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["snapshot_kind"] == "stopped", state
            assert state["exit_reason"] == 6, state
            assert state["exit_name"] == "HOST_YIELD", state
            assert state["dispatches"] == 3, state
            assert state["final_pc"] == 0x00100008, state
            assert state["gpr_lo"][8] == 12, state
            assert state["guest_instruction_attempts"] >= 24, state
            assert (state["cop0_cause"] & 0x7c) == 0, state

        # Unknown native guest opcodes stop without silently doing nothing.
        # The final diagnostics must report the actual PC, the ELF word,
        # the live RAM word and the nearby original instruction stream.
        with tempfile.TemporaryDirectory(prefix="ee-native-unsupported-pc-") as tmp:
            root = Path(tmp)
            elf = root / "unsupported.elf"
            make_unsupported_instruction_elf(elf)
            report_path = root / "unsupported.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--functions=entry",
                    "--max-dispatches=8",
                    "--report", str(report_path),
                    "--timeout-seconds=10",
                ],
                capture_output=True, text=True, check=False, timeout=40,
            )
            assert result.returncode == 3, result.stdout + result.stderr
            snapshot = json.loads(report_path.read_text(encoding="utf-8"))
            assert snapshot["exit_name"] == "UNSUPPORTED", snapshot
            assert snapshot["final_pc"] == 0x00100008, snapshot
            # Static expansion identifies the actual unsupported/invalid guest
            # instruction, even when execution later exits on the same word.
            assert ("not-lowered pc=0x00100008 opcode=0x4c000000 "
                    "mnemonic=unknown classification=invalid" in
                    (result.stdout + result.stderr)), (result.stdout, result.stderr)
            assert ("EE unsupported guest PC: pc=0x00100008 "
                    "elf=0x4c000000 ram=0x4c000000") in result.stderr, (
                result.stdout, result.stderr
            )
            assert ("EE unsupported code: > pc=0x00100008 "
                    "ELF=0x4c000000 RAM=0x4c000000") in result.stderr, (
                result.stdout, result.stderr
            )

        # Both real VU0 data memory macros and COP2 register transfers must
        # execute in a single emitted native block, not through a host fallback.
        with tempfile.TemporaryDirectory(prefix="ee-native-vu0-indexed-") as tmp:
            root = Path(tmp)
            elf = root / "vu0-indexed.elf"
            make_vu0_indexed_native_elf(elf)
            report_path = root / "vu0-indexed.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--functions=entry",
                    "--max-dispatches=16",
                    "--hle-startup", "--kseg-ram", "--ee-clock=reference-compat",
                    "--report", str(report_path),
                    "--timeout-seconds=10",
                ],
                capture_output=True, text=True, check=False, timeout=40,
                env=dict(os.environ, SDBZ_PROFILE_HORIZON="0"),
            )
            assert result.returncode == 3, (result.stdout, result.stderr)
            snapshot = json.loads(report_path.read_text(encoding="utf-8"))
            assert snapshot["exit_name"] == "EXCEPTION", (
                snapshot, result.stdout, result.stderr
            )
            assert (snapshot["cop0_cause"] >> 2) & 31 == 9, snapshot
            assert snapshot["gpr_lo"][10] & 0xffffffff == 0x12345678, snapshot
            assert snapshot["gpr_lo"][11] == 1, snapshot
            assert snapshot["gpr_lo"][12] == 1, snapshot
            assert "EE unsupported guest PC:" not in result.stderr, (
                result.stdout, result.stderr
            )

            shadow_report = root / "vu0-indexed-shadow.json"
            shadow = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"), "--no-recompile",
                "--functions=entry", "--max-dispatches=16",
                "--hle-startup", "--kseg-ram", "--ee-clock=reference-compat",
                "--report", str(shadow_report), "--timeout-seconds=10",
            ], capture_output=True, text=True, check=False, timeout=40,
                env=dict(os.environ, SDBZ_PROFILE_HORIZON="1"))
            assert shadow.returncode == result.returncode, shadow.stdout + shadow.stderr
            observed = json.loads(shadow_report.read_text(encoding="utf-8"))
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo", "cop0_cause",
                        "cop0_epc", "ee_cycles"):
                assert observed[key] == snapshot[key], key
            lines = [line for line in shadow.stderr.splitlines()
                     if line.startswith("EE horizon:")]
            assert len(lines) == 7, lines
            horizon = dict(token.split("=") for token in lines[0].split()[2:])
            assert int(horizon["vu_callback_boundaries"]) > 0, lines
            assert int(horizon["cop0_changed_boundaries"]) > 0, lines

        # A real guest Timer0 COUNT poll must finish *inside* its native CFG.
        # Both the clock source and the device must advance; simply resuming
        # a HOST_YIELD would conceal a missing MMIO timer.
        with tempfile.TemporaryDirectory(prefix="ee-native-timer0-poll-") as tmp:
            root = Path(tmp)
            elf = root / "timer0-poll.elf"
            make_ee_timer_poll_elf(elf)
            report_path = root / "timer0-poll.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--functions=entry",
                    "--max-dispatches=16",
                    "--loop-budget=5000",
                    "--loop-yield-resumes=1",
                    "--hle-startup", "--kseg-ram",
                    "--ee-clock=reference-compat",
                    "--report", str(report_path),
                    "--timeout-seconds=10",
                ],
                capture_output=True, text=True, check=False, timeout=40,
                env=dict(os.environ, SDBZ_PROFILE_HORIZON="0"),
            )
            assert result.returncode == 0 or result.returncode == 3, (
                result.stdout, result.stderr
            )
            snapshot = json.loads(report_path.read_text(encoding="utf-8"))
            assert snapshot["exit_name"] == "EXCEPTION", (
                snapshot, result.stdout, result.stderr
            )
            assert snapshot["cop0_cause"] & 0x7c == 0x24, snapshot
            assert 32 <= snapshot["gpr_lo"][10] <= 0xffff, snapshot
            assert snapshot["gpr_lo"][16] == snapshot["gpr_lo"][17], snapshot
            assert 1 <= snapshot["gpr_lo"][11] < 1000, snapshot
            assert "guest host loop yield cap reached" not in result.stderr, (
                result.stdout, result.stderr
            )

            assert "EE horizon:" not in result.stderr, result.stderr
            shadow_report = root / "timer0-shadow.json"
            shadow = subprocess.run([
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"), "--no-recompile",
                "--functions=entry", "--max-dispatches=16",
                "--loop-budget=5000", "--loop-yield-resumes=1",
                "--hle-startup", "--kseg-ram", "--ee-clock=reference-compat",
                "--report", str(shadow_report), "--timeout-seconds=10",
            ], capture_output=True, text=True, check=False, timeout=40,
                env=dict(os.environ, SDBZ_PROFILE_HORIZON="1"))
            assert shadow.returncode == result.returncode, shadow.stdout + shadow.stderr
            observed = json.loads(shadow_report.read_text(encoding="utf-8"))
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo", "cop0_cause",
                        "cop0_epc", "ee_cycles"):
                assert observed[key] == snapshot[key], key
            lines = [line for line in shadow.stderr.splitlines()
                     if line.startswith("EE horizon:")]
            assert len(lines) == 7 and "shadow_only=1" in lines[0], lines
            horizon = dict(token.split("=") for token in lines[0].split()[2:])
            assert (int(horizon["normal_before"]) <=
                    int(horizon["normal_before_iop_lookahead"]) <=
                    int(horizon["normal_before_without_iop"])), lines
            assert int(horizon["iop_idle_candidates"]) >= 0, lines
            assert int(horizon["iop_rpc_barriers"]) >= 0, lines
            assert int(horizon["gs_worker_barriers"]) == 0, lines
            assert int(horizon["dmac_started_barriers"]) >= 0, lines
            assert int(horizon["clock_barriers"]) >= 0, lines
            assert (int(horizon["mmio_free_direct_touched"]) +
                    int(horizon["mmio_free_direct_quiet"]) ==
                    int(horizon["lookahead_mmio_free"])), lines
            assert (int(horizon["lookahead_mmio_free"]) +
                    int(horizon["lookahead_mmio_touched"]) ==
                    int(horizon["normal_before_iop_lookahead"])), lines
            counts = [dict(token.split("=") for token in line.split()[2:])
                      for line in lines[1:]]
            total_samples = sum(int(row["samples"]) for row in counts)
            assert total_samples > 0, lines
            assert int(horizon["clock_barriers"]) <= total_samples, lines
            assert (0 <= int(horizon["dmac_passive_sif0"]) <=
                    int(horizon["dmac_sif0_only"]) <= int(horizon["dmac_str5"])), lines
            assert int(horizon["dmac_str7"]) == 0, lines
            vu_reasons = [int(horizon[key]) for key in
                          ("entry_vif1_pending", "entry_cmsar1", "entry_vu0_running")]
            assert max(vu_reasons) <= int(horizon["entry_vu"]) <= sum(vu_reasons), lines
            assert int(horizon["alu_bound_candidates"]) == (
                int(horizon["alu_bound_matched"]) + int(horizon["alu_bound_mismatch"])), lines
            assert int(horizon["alu_bound_matched"]) <= int(horizon["mmio_free_direct_quiet"]), lines
            assert (0 <= int(horizon["alu_certified_matched"]) <= int(horizon["alu_owner_matched"])
                    <= int(horizon["alu_bound_matched"])), lines
            assert (0 <= int(horizon["alu_certified"]) <= int(horizon["alu_owner_known"])
                    <= int(horizon["alu_bound_candidates"])), lines
            for key in ("entry_callback", "entry_idle", "entry_irq", "entry_vu",
                        "entry_other", *(f"dmac_str{i}" for i in range(10))):
                assert 0 <= int(horizon[key]) <= total_samples, lines
            assert 0 <= int(horizon["vu_callback_boundaries"]) <= total_samples, lines
            assert 0 <= int(horizon["cop0_changed_boundaries"]) <= total_samples, lines
            assert all(int(row["samples"]) == int(row["before"]) + int(row["reached"])
                       for row in counts), lines

        with tempfile.TemporaryDirectory(prefix="ee-native-idle-timer-") as tmp:
            root = Path(tmp)
            elf = root / "idle-timer.elf"
            make_idle_timer_wakeup_elf(elf)
            report_path = root / "idle-timer.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf), "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--functions=entry", "--extra-functions=0x00100080",
                    "--max-dispatches=32", "--auto-expand=8",
                    "--hle-startup", "--kseg-ram", "--ee-clock=reference-compat",
                    "--trace", "--report", str(report_path), "--timeout-seconds=10",
                ],
                capture_output=True, text=True, check=False, timeout=40,
            )
            combined = result.stdout + result.stderr
            assert "HLE EE scheduler idle: no runnable guest threads" in combined, combined
            assert "HLE INTC IRQ: cause=9" in combined, combined
            assert "HLE _iGetThreadId: id=0" in combined, combined
            assert "HLE iWakeupThread: id=1 status=2 wakeups=0 released=1" in combined, combined
            state = json.loads(report_path.read_text(encoding="utf-8"))
            assert state["exit_name"] == "EXCEPTION", state
            assert state["cop0_epc"] == 0x00100048, state
            assert state["gpr_lo"][16] == 1, state
            assert state["ee_cycles"] - state["guest_instruction_attempts"] >= 2000, state

        # Compare actual runner service with and without quiet IOP batching.
        with tempfile.TemporaryDirectory(prefix="ee-iop-idle-batch-") as tmp:
            root = Path(tmp)
            elf = root / "iop-idle-batch.elf"
            make_iop_idle_batch_elf(elf)
            disc = root / "disc"
            disc.mkdir()
            (disc / "SYSTEM.CNF").write_text(
                "BOOT2 = cdrom0:\\\\SLUS_214.42;1\n", encoding="ascii"
            )
            command = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf), "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--functions=entry", "--extra-functions=0x00100038",
                "--max-dispatches=16", "--loop-budget=10000",
                "--hle-startup", "--kseg-ram", "--ee-clock=reference-compat",
                "--disc-folder", str(disc), "--timeout-seconds=10",
            ]
            states = []
            clocks = []
            for enabled, quiet in (("0", "0"), ("1", "0"), ("1", "1")):
                report = root / f"batch-{enabled}-{quiet}.json"
                replay = command if enabled == "0" else [
                    arg for arg in command if not arg.startswith("--extra-functions=")
                ] + ["--no-recompile"]
                run = subprocess.run(
                    replay + ["--report", str(report)],
                    capture_output=True, text=True, check=False, timeout=40,
                    env=dict(os.environ, SDBZ_IOP_COALESCE_IDLE_TURNS=enabled,
                             SDBZ_IDLE_SKIP="0", SDBZ_PROFILE_HORIZON="1",
                             SDBZ_EE_DIRECT_DISPATCH="1",
                             SDBZ_NATIVE_ALU_CERTIFICATES="1",
                             SDBZ_IOP_FAST_QUIET_TURNS=quiet),
                )
                assert run.returncode == 3, run.stdout + run.stderr
                state = json.loads(report.read_text(encoding="utf-8"))
                assert state["exit_name"] == "EXCEPTION", state
                assert state["gpr_lo"][8] == 8192, state
                states.append(state)
                horizon_line = next(line for line in run.stderr.splitlines()
                                    if line.startswith("EE horizon: shadow_only="))
                horizon = dict(token.split("=") for token in horizon_line.split()[2:])
                assert int(horizon["dmac_passive_sif0"]) > 0, horizon_line
                assert (int(horizon["dmac_passive_sif0"]) <=
                        int(horizon["dmac_sif0_only"]) <= int(horizon["dmac_str5"])), horizon_line
                assert int(horizon["dmac_started_barriers"]) == 0, horizon_line
                assert int(horizon["dmac_top_mask"], 16) == 0x20, horizon_line
                assert int(horizon["dmac_top_mask_count"]) == int(horizon["dmac_sif0_only"]), horizon_line
                assert int(horizon["alu_bound_matched"]) > 0, horizon_line
                assert int(horizon["alu_bound_mismatch"]) == 0, horizon_line
                assert int(horizon["alu_bound_candidates"]) == int(horizon["alu_bound_matched"]), horizon_line
                assert int(horizon["alu_owner_known"]) == int(horizon["alu_bound_candidates"]), horizon_line
                assert int(horizon["alu_certified_matched"]) > 0, horizon_line
                assert int(horizon["alu_certified"]) == int(horizon["alu_certified_matched"]), horizon_line
                assert all(int(horizon[key]) == 0 for key in
                           ("entry_vif1_pending", "entry_cmsar1", "entry_vu0_running")), horizon_line
                clock = [line for line in run.stdout.splitlines()
                         if line.startswith("IOP SPU2 clock:")]
                assert len(clock) == 1, run.stdout
                clocks.append(clock[0])
                quiet_lines = [line for line in run.stdout.splitlines()
                               if line.startswith("IOP clock quiet:")]
                if quiet == "1":
                    assert len(quiet_lines) == 1, run.stdout
                    counts = dict(token.split("=") for token in quiet_lines[0].split()[3:])
                    assert int(counts["advanced"]) > 0, quiet_lines
                    assert int(counts["cycles"]) > 0, quiet_lines
                    assert int(counts["attempts"]) == (
                        int(counts["advanced"]) + int(counts["fallback"])), quiet_lines
                else:
                    assert not quiet_lines, quiet_lines
                lines = [line for line in run.stdout.splitlines()
                         if line.startswith("IOP clock coalesce:")]
                if enabled == "0":
                    assert not lines, lines
                else:
                    assert len(lines) == 1, run.stdout
                    counters = dict(token.split("=") for token in lines[0].split()[3:])
                    assert int(counters["saved_turns"]) > 0, lines
                    assert int(counters["combined_batches"]) > 0, lines
                    assert (int(counters["physical_runs"]) + int(counters["saved_turns"]) ==
                            int(counters["logical_turns"])), lines
            for key in ("dispatches", "final_pc", "exit_reason", "dispatch_status",
                        "guest_instruction_attempts", "gpr_lo", "cop0_cause",
                        "cop0_epc", "ee_cycles"):
                assert states[0][key] == states[1][key] == states[2][key], key
            assert clocks[0] == clocks[1] == clocks[2], clocks

        # Resuming without a bounded guest-loop watchdog would spin forever;
        # reject an invalid command before loading or mutating guest state.
        with tempfile.TemporaryDirectory(prefix="ee-invalid-loop-resume-") as tmp:
            root = Path(tmp)
            elf = root / "invalid-yield.elf"
            make_counting_loop_elf(elf)
            result = subprocess.run(
                [
                    str(runner), str(elf), str(root / "unused-library.so"),
                    "--loop-yield-resumes=2",
                ],
                capture_output=True, text=True, check=False, timeout=10,
            )
            assert result.returncode == 2, (result.stdout, result.stderr)
            assert "--loop-yield-resumes requires --loop-budget" in result.stderr, (
                result.stdout, result.stderr
            )

        for blocking_wait, invalid_status in (
            (False, False), (True, False), (False, True),
        ):
            with tempfile.TemporaryDirectory(prefix="ee-native-sema-ops-") as tmp:
                root = Path(tmp)
                elf = root / "semaphore-ops.elf"
                make_semaphore_operations_elf(
                    elf, blocking_wait=blocking_wait,
                    invalid_status=invalid_status,
                )
                report_path = root / "semaphore-ops.json"
                command = [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=64",
                    "--report", str(report_path),
                    "--trace",
                ]
                if not blocking_wait and not invalid_status:
                    no_hle = subprocess.run(
                        command, capture_output=True, text=True, check=False,
                    )
                    assert no_hle.returncode != 0, no_hle.stdout + no_hle.stderr
                    denied = json.loads(report_path.read_text(encoding="utf-8"))
                    assert denied["cop0_epc"] == 0x00100014, denied
                    command.append("--no-recompile")

                result = subprocess.run(
                    command + ["--hle-startup"],
                    capture_output=True, text=True, check=False,
                )
                assert result.returncode != 0, result.stdout + result.stderr
                state = json.loads(report_path.read_text(encoding="utf-8"))
                regs = state["gpr_lo"]
                assert state["snapshot_kind"] == "stopped", state
                assert state["exit_name"] == (
                    "EXCEPTION" if blocking_wait or invalid_status else "DISPATCH"
                ), state
                assert regs[8] == 1, state
                assert "HLE CreateSema: id=1" in result.stdout, result.stdout
                if blocking_wait:
                    assert "WaitSema HLE refused blocking wait:" in result.stderr, (
                        result.stdout + result.stderr
                    )
                    assert regs[9] == 0, state  # never returned from WaitSema
                    assert state["cop0_epc"] == 0x00100024, state
                    assert "HLE WaitSema:" not in result.stdout
                else:
                    assert "HLE WaitSema: id=1 old_count=1 new_count=0 result=1" in result.stdout, (
                        result.stdout + result.stderr
                    )
                    assert regs[9] == 1, state
                    assert regs[10] == 0xffffffffffffffff, state
                    assert regs[11] == 0xffffffffffffffff, state
                    assert regs[12] == 1 and regs[13] == 1, state
                    assert regs[14] == 0xffffffffffffffff, state
                    assert regs[15] == 1 and regs[16] == 1, state
                    if invalid_status:
                        assert "ReferSemaStatus HLE refused unbacked or unaligned pointer" in result.stderr, (
                            result.stdout + result.stderr
                        )
                        assert regs[17] == 0, state
                    else:
                        assert regs[17] == 1 and regs[18] == 1, state
                        assert regs[19] == 0, state   # count
                        assert regs[20] == 2, state   # max
                        assert regs[21] == 1, state   # initial count
                        assert regs[22] == 0, state   # waiters
                        assert regs[23] == 0x1234, state
                        assert regs[24] == 0x5678, state
                        assert regs[25] == 1, state   # delete
                        assert regs[2] == 0xffffffffffffffff, state
                        assert "not found, result=-1" in result.stdout, (
                            result.stdout + result.stderr
                        )

        with tempfile.TemporaryDirectory(prefix="ee-native-timeout-snapshot-") as tmp:
            root = Path(tmp)
            elf = root / "loop-mips.elf"
            make_spin_elf(elf)
            report_path = root / "timeout.json"
            result = subprocess.run(
                [
                    sys.executable, str(source / "tools" / "run_game.py"),
                    str(elf),
                    "--build-dir", str(build),
                    "--work-dir", str(root / "out"),
                    "--max-dispatches=8",
                    "--timeout-seconds=1",
                    "--report", str(report_path),
                    "--trace",
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=40,
            )
            assert result.returncode == 124, result.stdout + result.stderr
            snapshot = json.loads(report_path.read_text(encoding="utf-8"))
            assert snapshot["snapshot_kind"] == "in_progress", snapshot
            assert snapshot["dispatches"] == 0, snapshot
            assert snapshot["syscall_history"] == [], snapshot
            assert snapshot["final_pc"] == 0x00100008, snapshot
            assert "last saved snapshot" in result.stderr, result.stderr

        with tempfile.TemporaryDirectory(prefix="ee-kernel-snapshot-") as tmp:
            root = Path(tmp)
            elf = root / "kernel-probe.elf"
            make_kernel_ram_probe_elf(elf)
            kernel = root / "kernel-ram.bin"
            kernel.write_bytes(struct.pack("<I", 0x12345678) + bytes(0x80000 - 4))
            archive_impostor = root / "wrong-snapshot.bin"
            archive_impostor.write_bytes(b"PK\x03\x04" + bytes(0x80000 - 4))
            report = root / "kernel-probe.json"
            cmd = [
                sys.executable, str(source / "tools" / "run_game.py"),
                str(elf),
                "--build-dir", str(build),
                "--work-dir", str(root / "out"),
                "--max-dispatches=8",
                "--kseg-ram",
                "--report", str(report),
            ]
            first = subprocess.run(cmd, capture_output=True, text=True, check=False)
            assert first.returncode != 0, first.stdout + first.stderr
            before = json.loads(report.read_text(encoding="utf-8"))
            assert before["gpr_lo"][8] == 0, before

            invalid = subprocess.run(
                cmd + ["--no-recompile", "--kernel-ram", str(archive_impostor)],
                capture_output=True, text=True, check=False,
            )
            assert invalid.returncode != 0, invalid.stdout + invalid.stderr
            assert "ZIP/savestate bytes" in invalid.stderr, invalid.stderr

            second = subprocess.run(
                cmd + ["--no-recompile", "--kernel-ram", str(kernel)],
                capture_output=True, text=True, check=False,
            )
            assert second.returncode != 0, second.stdout + second.stderr
            assert "EE kernel RAM snapshot: 524288 bytes" in second.stdout, (
                second.stdout + second.stderr
            )
            after = json.loads(report.read_text(encoding="utf-8"))
            assert after["gpr_lo"][8] == 0x12345678, after

    print(f"game_runner shard {shard if shard is not None else 'all'}: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
