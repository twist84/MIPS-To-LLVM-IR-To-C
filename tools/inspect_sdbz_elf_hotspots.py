#!/usr/bin/env python3
"""Read-only, file-backed R5900 ELF hotspot/cross-reference inspector.

No guest execution, translation, rewriting, cache mutation or third-party
dependencies. Direct branch/call cross-references are exact; inferred
load/store addresses are marked as conservative local-constant candidates.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DEFAULT_PCS = (
    0x001751ec, 0x00175204, 0x00175210,
    0x00178de8, 0x001bd58c, 0x001ba584, 0x001baa10,
    0x00108220, 0x001094dc,
    0x001024d0, 0x00102560, 0x00102670,
)
DEFAULT_DATA = (
    0x1000f000, 0x1000f010, 0x10000000,
    0x01ffeeb0, 0x01ffeeb8,
    0x00502368, 0x0050236c, 0x005030ac,
)
GPR = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
)
MEM_OPS = {
    0x1e: "lq", 0x1f: "sq", 0x20: "lb", 0x21: "lh",
    0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
    0x26: "lwr", 0x27: "lwu", 0x28: "sb", 0x29: "sh",
    0x2a: "swl", 0x2b: "sw", 0x2c: "sdl", 0x2d: "sdr",
    0x2e: "swr", 0x30: "ll", 0x31: "lwc1", 0x32: "lwc2",
    0x34: "lld", 0x35: "ldc1", 0x36: "ldc2", 0x37: "ld",
    0x38: "sc", 0x39: "swc1", 0x3a: "swc2", 0x3c: "scd",
    0x3d: "sdc1", 0x3e: "sdc2", 0x3f: "sd",
}
MEM_WRITES = {"sq", "sb", "sh", "swl", "sw", "sdl", "sdr", "swr",
              "sc", "scd", "swc1", "swc2", "sdc1", "sdc2", "sd"}
BRANCH_OPS = {
    0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
}
REGIMM_BRANCHES = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl",
    0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
}


def sx16(x):
    return x - 0x10000 if x & 0x8000 else x


def parse_address(text):
    return int(text, 0)


class Elf:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if (len(self.data) < 52 or self.data[:4] != b"\x7fELF" or
                self.data[4] != 1 or self.data[5] != 1):
            raise ValueError("expected an ELF32 little-endian executable")
        h = struct.unpack_from("<HHIIIIIHHHHHH", self.data, 16)
        if h[1] != 8:
            raise ValueError("expected MIPS/R5900 ELF e_machine=8")
        self.entry = h[3]
        phoff, phentsize, phnum = h[4], h[8], h[9]
        if phentsize < 32 or phoff + phnum * phentsize > len(self.data):
            raise ValueError("invalid ELF program headers")
        self.segments = []
        for i in range(phnum):
            t, off, vaddr, _paddr, sz, _memsz, flags, _align = \
                struct.unpack_from("<IIIIIIII", self.data, phoff + i * phentsize)
            if t == 1 and sz > 0:
                if off + sz > len(self.data):
                    raise ValueError("truncated ELF PT_LOAD segment")
                self.segments.append((vaddr, vaddr + sz, off, bool(flags & 1)))
        if not self.segments:
            raise ValueError("no file-backed PT_LOAD segments")

    def read32(self, pc):
        for start, end, off, _x in self.segments:
            if start <= pc and pc + 4 <= end:
                return struct.unpack_from("<I", self.data, off + pc - start)[0]
        return None

    def words(self):
        for start, end, off, executable in self.segments:
            if executable:
                for pc in range(start + ((-start) & 3), end - 3, 4):
                    yield pc, struct.unpack_from("<I", self.data, off + pc - start)[0]


def branch(word, pc):
    op = word >> 26
    rs, rt = (word >> 21) & 31, (word >> 16) & 31
    if op in (2, 3):
        name = "jal" if op == 3 else "j"
        target = ((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
        return name, target
    if op in BRANCH_OPS:
        return BRANCH_OPS[op], (pc + 4 + sx16(word & 0xffff) * 4) & 0xffffffff
    if op == 1 and rt in REGIMM_BRANCHES:
        return REGIMM_BRANCHES[rt], (pc + 4 + sx16(word & 0xffff) * 4) & 0xffffffff
    if op in (0x10, 0x11, 0x12) and rs == 8 and rt <= 3:
        return "bc%d%s%s" % (
            op - 0x10, "t" if rt & 1 else "f", "l" if rt & 2 else ""
        ), (pc + 4 + sx16(word & 0xffff) * 4) & 0xffffffff
    if op == 0 and (word & 63) in (8, 9):
        return ("jalr" if word & 63 == 9 else "jr"), None
    return None


def decode(word, pc):
    op = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    imm = word & 0xffff
    edge = branch(word, pc)
    if edge:
        return "%s %s" % (edge[0], hex(edge[1]) if edge[1] is not None else GPR[rs])
    if op in MEM_OPS:
        return "%s %s,%d(%s)" % (MEM_OPS[op], GPR[rt], sx16(imm), GPR[rs])
    if op == 0x0f:
        return "lui %s,0x%04x" % (GPR[rt], imm)
    if op in (0x09, 0x19):
        return "%s %s,%s,%d" % (
            "daddiu" if op == 0x19 else "addiu", GPR[rt], GPR[rs], sx16(imm)
        )
    if op in (0x0d, 0x0c, 0x0e):
        return "%s %s,%s,0x%04x" % (
            {0x0d: "ori", 0x0c: "andi", 0x0e: "xori"}[op], GPR[rt], GPR[rs], imm
        )
    if op == 0:
        fn = word & 63
        if word == 0:
            return "nop"
        if fn == 0x0c:
            return "syscall"
        if fn in (0, 2, 3):
            return "%s %s,%s,%d" % (
                {0: "sll", 2: "srl", 3: "sra"}[fn],
                GPR[rd], GPR[rt], (word >> 6) & 31
            )
        if fn in (0x21, 0x2d):
            return "%s %s,%s,%s" % (
                "daddu" if fn == 0x2d else "addu", GPR[rd], GPR[rs], GPR[rt]
            )
    return "word 0x%08x" % word


def local_base(elf, pc, base, lookback=32):
    """Locally prove a base constant, stopping propagation at control flow.

    This deliberately does NOT claim inter-block data-flow equivalence.
    """
    regs = [None] * 32
    regs[0] = 0
    reset_after_delay = False
    for address in range(max(0, pc - 4 * lookback), pc, 4):
        word = elf.read32(address)
        if word is None:
            regs = [None] * 32
            regs[0] = 0
            continue
        op = word >> 26
        rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        imm = word & 0xffff
        fn = word & 63
        if op == 15:
            regs[rt] = (imm << 16) & 0xffffffff
        elif op in (9, 0x19, 0xd, 0xc, 0xe):
            a = regs[rs]
            regs[rt] = (
                ((a + sx16(imm)) & 0xffffffff) if op in (9, 0x19) else
                ((a | imm) & 0xffffffff) if op == 0xd else
                ((a & imm) & 0xffffffff) if op == 0xc else
                ((a ^ imm) & 0xffffffff)
            ) if a is not None else None
        elif op == 0:
            if fn in (0, 2, 3):
                a = regs[rt]
                shift = (word >> 6) & 31
                regs[rd] = (
                    (a << shift) & 0xffffffff if fn == 0 else
                    (a >> shift) if fn == 2 else
                    ((a ^ 0x80000000) - 0x80000000) >> shift
                ) if a is not None else None
            elif fn in (0x21, 0x2d):
                a, b = regs[rs], regs[rt]
                regs[rd] = ((a + b) & 0xffffffff) if a is not None and b is not None else None
            elif fn not in (8, 0xc, 0xd):
                regs[rd] = None
        elif op in MEM_OPS or op in (8, 0xa, 0xb, 0x18, 0x1a):
            if op not in (0x1f, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
                          0x2e, 0x39, 0x3a, 0x3d, 0x3e, 0x3f):
                regs[rt] = None
        if reset_after_delay:
            regs = [None] * 32
            regs[0] = 0
            reset_after_delay = False
        if branch(word, address) is not None:
            reset_after_delay = True
        regs[0] = 0
    return regs[base]


def memory_refs(elf, targets):
    result = {addr: [] for addr in targets}
    for pc, word in elf.words():
        op = word >> 26
        if op not in MEM_OPS:
            continue
        base = (word >> 21) & 31
        value = local_base(elf, pc, base)
        if value is None:
            continue
        effective = (value + sx16(word & 0xffff)) & 0xffffffff
        if effective in result:
            result[effective].append(
                (pc, MEM_OPS[op], decode(word, pc), hex(value))
            )
    return result


def load_functions(path):
    if path is None or not path.is_file():
        return [], []
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("functions", []), data.get("jalr_sites", [])


def owners(functions, pc):
    found = []
    for item in functions:
        address = int(item["vaddr"], 0)
        size = item.get("size", 0)
        if address <= pc < address + size:
            matched = pc == address or f"0x{pc:08x}" in item.get("block_pcs", [])
            found.append((matched, address, item))
    return [p[2] for p in sorted(found, key=lambda v: (-int(v[0]), -v[1]))[:5]]


def analyze(elf, pcs, data_targets, functions=(), jalr_sites=()):
    all_words = list(elf.words())
    incoming = {}
    for pc, word in all_words:
        edge = branch(word, pc)
        if edge is not None and edge[1] is not None:
            incoming.setdefault(edge[1], []).append((pc, edge[0]))
    for site in jalr_sites:
        if site.get("status") == "resolved_executable":
            target = int(site["target"], 0)
            incoming.setdefault(target, []).append((int(site["pc"], 0), "jalr(metadata)"))

    lines = [
        "SDBZ R5900 ELF hotspot cross-references (static; no guest execution)",
        "ELF: %s" % elf.path,
        "SHA-256: %s" % hashlib.sha256(elf.data).hexdigest(),
        "Entry: 0x%08x; file-backed PT_LOAD segments: %d" % (elf.entry, len(elf.segments)),
        "NOTE: binary jump targets and instructions are exact. A window is",
        "NOT a reconstructed CFG. Resolved data refs use only local register",
        "constants; unknown/dynamic references are NOT enumerated. Whole",
        "executable segments may contain non-code bytes, so treat unowned",
        "candidate cross-references as requiring CFG confirmation.",
        "",
    ]
    for target in sorted(set(pcs)):
        lines += ["=" * 76, "HOTSPOT 0x%08x" % target]
        own = owners(functions, target)
        if own:
            lines.append("Containing emitted function candidate(s):")
            for item in own:
                lines.append("  %s entry=%s size=%s blocks=%d" % (
                    item.get("name", "unnamed"), item["vaddr"],
                    item.get("size", "?"), len(item.get("block_pcs", []))
                ))
        else:
            lines.append("Containing function: not established by supplied metadata")
        sources = incoming.get(target, [])
        lines.append("Incoming direct and metadata-resolved xrefs (%d):" % len(sources))
        for pc, kind in sources[:64]:
            lines.append("  %08x %-15s %s" % (pc, kind, decode(elf.read32(pc), pc)))
        if len(sources) > 64:
            lines.append("  ... %d additional" % (len(sources) - 64))
        if own:
            for item in own[:2]:
                entry = int(item["vaddr"], 0)
                if entry != target:
                    refs = incoming.get(entry, [])
                    lines.append("Incoming to function entry %08x (%d):" % (entry, len(refs)))
                    for pc, kind in refs[:32]:
                        lines.append("  %08x %-15s %s" % (pc, kind, decode(elf.read32(pc), pc)))
        lines.append("Surrounding original instructions (PC, word, decoded subset):")
        for pc in range(target - 0x48, target + 0x4c, 4):
            word = elf.read32(pc)
            if word is None:
                continue
            tag = ">>" if pc == target else "  "
            line = "%s %08x %08x  %s" % (tag, pc, word, decode(word, pc))
            op = word >> 26
            if op in MEM_OPS:
                base = (word >> 21) & 31
                value = local_base(elf, pc, base)
                if value is not None:
                    line += "  ; candidate effective=0x%08x (%s)" % (
                        (value + sx16(word & 0xffff)) & 0xffffffff,
                        "write" if MEM_OPS[op] in MEM_WRITES else "read"
                    )
            lines.append(line)
        lines.append("")
    lines += ["=" * 76, "CANDIDATE STATIC DATA ACCESSES (local constants only)"]
    for address, refs in sorted(memory_refs(elf, data_targets).items()):
        lines.append("0x%08x: %d local-constant candidate references" % (address, len(refs)))
        for pc, mnemonic, decoded, base in refs[:200]:
            lines.append("  0x%08x [%s] %s ; base=%s" % (
                pc, "write" if mnemonic in MEM_WRITES else "read", decoded, base
            ))
        if len(refs) > 200:
            lines.append("  ... %d additional" % (len(refs) - 200))
    lines.append("")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf", type=Path)
    p.add_argument("--pc", type=parse_address, action="append", default=[])
    p.add_argument("--data", type=parse_address, action="append", default=[])
    p.add_argument("--metadata", type=Path, default=None,
                   help="Existing ir/ee_functions.json; read only")
    p.add_argument("--runtime-json", type=Path, default=None,
                   help="Existing first-boot.json; add current hot and final PCs")
    p.add_argument("--expected-sha256", default=None,
                   help="Optional retail executable digest guard")
    p.add_argument("--out", type=Path, default=None)
    args = p.parse_args()
    elf = Elf(args.elf)
    if args.expected_sha256 is not None:
        actual = hashlib.sha256(elf.data).hexdigest()
        if actual.lower() != args.expected_sha256.lower():
            p.error("ELF digest differs from requested game/region: %s" % actual)
    pcs = list(DEFAULT_PCS) + args.pc
    if args.runtime_json is not None:
        runtime = json.loads(args.runtime_json.read_text(encoding="utf-8"))
        pcs += [runtime.get("final_pc", 0)]
        pcs += [entry["pc"] for entry in runtime.get("m2_runtime", {}).get("hot_pcs", [])[:16]]
        pcs = [pc for pc in pcs if pc != 0]
    functions, jalr_sites = load_functions(args.metadata)
    output = analyze(elf, pcs, list(DEFAULT_DATA) + args.data,
                     functions, jalr_sites)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output, encoding="utf-8")
        print("ELF hotspot cross-references written:", args.out)
    else:
        print(output)


if __name__ == "__main__":
    main()
