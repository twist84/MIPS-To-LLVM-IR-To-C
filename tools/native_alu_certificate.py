"""Conservative certificates from the exact pre-optimization guest IR.

Only unguarded, straight-line blocks with the emitter's canonical entry
switch, CPU GPR/PC/exit-state accesses, integer ALU and instruction-count
calls qualify. Unknown forms are rejected, never guessed. The existing
verified object manifest is the trust boundary, not a verifier for hostile
arbitrary LLVM modules. The runner must also compare the exact opcode words
with its ELF, apply scheduler/diagnostic guards and retain native ownership.
"""
import hashlib
import re
from collections import Counter
from pathlib import Path

_TEMP = r"%t[0-9]+"
_VALUE = rf"(?:{_TEMP}|-?[0-9]+|true|false)"
_GEP = re.compile(
    rf"({_TEMP}) = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 "
    r"(?:0, i32 ([0-9]+), i32 ([01])|([67]))"
)
_LOAD = re.compile(rf"({_TEMP}) = load (i32|i64), ptr ({_TEMP})")
_STORE = re.compile(rf"store (i32|i64) ({_VALUE}), ptr ({_TEMP})")
_COUNT = re.compile(
    r"call void @ee_count_guest_instruction\(ptr %rt, i32 ([0-9]+), i32 ([0-9]+)\)"
)
_ALU = re.compile(
    rf"{_TEMP} = (?:add|sub|and|or|xor|shl|lshr|ashr) i(?:32|64) {_VALUE}, {_VALUE}"
    rf"|{_TEMP} = icmp (?:eq|ne|slt|sle|sgt|sge|ult|ule|ugt|uge) i(?:32|64) {_VALUE}, {_VALUE}"
    rf"|{_TEMP} = (?:sext|zext|trunc) i(?:1|32|64) {_VALUE} to i(?:1|32|64)"
    rf"|{_TEMP} = select i1 {_VALUE}, i(?:32|64) {_VALUE}, i(?:32|64) {_VALUE}"
)
_LABEL = re.compile(r"([A-Za-z0-9_.]+):")
_ENTRY = re.compile(r"i32 ([0-9]+), label %(bb_0x[0-9a-f]+)")


def _block_words(lines, pc):
    pointers = {}
    words = []
    if not lines or lines[-1] not in ("ret i32 0", "ret i32 3"):
        return ()
    for line in lines[:-1]:
        gep = _GEP.fullmatch(line)
        if gep:
            name, reg, lane, field = gep.groups()
            if reg is not None and int(reg) > 31:
                return ()
            pointers[name] = "i64" if reg is not None else "i32"
            continue
        load = _LOAD.fullmatch(line)
        if load:
            if pointers.get(load[3]) != load[2]:
                return ()
            continue
        store = _STORE.fullmatch(line)
        if store:
            if pointers.get(store[3]) != store[1]:
                return ()
            continue
        count = _COUNT.fullmatch(line)
        if count:
            address, word = map(int, count.groups())
            if (address != pc + 4 * len(words) or address > 0xffffffff or
                    word > 0xffffffff or len(words) == 32):
                return ()
            words.append(word)
            continue
        if not _ALU.fullmatch(line):
            return ()
    if len(words) < 2:
        return ()
    jump = words[-2]
    # Earlier source operations and the delay slot are checked by the
    # runner's independent opcode whitelist against these exact words.
    if not (jump >> 26 == 3 or
            (jump >> 26 == 0 and jump & 63 in (8, 9))):
        return ()
    return tuple(words)


def extract_bounds(ir, symbol, block_pcs):
    """Return {exact switch PC: exact instruction words}; fail closed."""
    header = f"define i32 @{symbol}(ptr %cpu, ptr %rt) {{"
    lines = [line.strip() for line in ir.splitlines()]
    try:
        start = lines.index(header) + 1
        end = lines.index("}", start)
    except ValueError:
        return {}
    body = lines[start:end]
    # The only entry prologue allowed is a PC field pointer/load/switch.
    # Additional allocations, traces or branches deliberately reject.
    if len(body) < 5 or body[0] != "entry:":
        return {}
    gep = _GEP.fullmatch(body[1])
    load = _LOAD.fullmatch(body[2])
    if (not gep or gep[4] != "6" or not load or load[2] != "i32" or
            load[3] != gep[1] or body[3] !=
            f"switch i32 {load[1]}, label %dispatch_unknown ["):
        return {}
    try:
        close = body.index("]", 4)
    except ValueError:
        return {}
    entries = {}
    for line in body[4:close]:
        match = _ENTRY.fullmatch(line)
        if not match:
            return {}
        pc, label = int(match[1]), match[2]
        if pc in entries:
            return {}
        entries[pc] = label

    blocks = {}
    label = None
    for line in body[close + 1:]:
        match = _LABEL.fullmatch(line)
        if match:
            label = match[1]
            if label in blocks:
                return {}
            blocks[label] = []
        elif label is not None:
            blocks[label].append(line)
        else:
            return {}
    result = {}
    for pc in block_pcs:
        label = f"bb_0x{pc:x}"
        if pc != 0 and entries.get(pc) == label and label in blocks:
            words = _block_words(blocks[label], pc)
            if words:
                result[pc] = words
    return result


def verified_bounds(metadata_path, functions, manifest):
    """Read only IR whose digest equals the manifest for its native object.

    The linker separately validates all object sizes/digests and symbol
    ownership. Unavailable or relocated/mismatching IR yields no certificate.
    """
    by_input = {Path(item["input"]).name: item for item in manifest["objects"]}
    owner_counts = Counter(fn["vaddr"] for fn in functions)
    result = []
    for fn in functions:
        owner = fn["vaddr"]
        if owner == 0 or owner_counts[owner] != 1:
            continue
        item = by_input.get(Path(fn["ir_path"]).name)
        if not item or not item.get("ir_sha256"):
            continue
        for path in (Path(item["input"]),
                     metadata_path.parent / Path(item["input"]).name,
                     Path(fn["ir_path"])):
            try:
                data = path.read_bytes()
                if hashlib.sha256(data).hexdigest() != item["ir_sha256"]:
                    continue
                ir = data.decode("utf-8")
            except (OSError, UnicodeError):
                continue
            for pc, words in extract_bounds(ir, fn["host_symbol"],
                                            fn["block_pcs"]).items():
                result.append((owner, pc, words))
            break
    return sorted(result)


def build_certificate_source(bounds):
    """Embed exact word sequences in the same linked library as the owner."""
    words = []
    entries = []
    for owner, pc, code in bounds:
        entries.append(f"    {{0x{owner:08x}u, 0x{pc:08x}u, {len(words)}u, {len(code)}u}},")
        words.extend(f"0x{word:08x}u" for word in code)
    return "\n".join([
        '#include "ee_dispatcher.h"',
        "#include <algorithm>",
        "#include <cstdint>",
        "#include <utility>",
        "#if defined(_WIN32)",
        "#define EE_CERT_EXPORT __declspec(dllexport)",
        "#else",
        '#define EE_CERT_EXPORT __attribute__((visibility("default")))',
        "#endif",
        'extern "C" uint32_t ee_game_resolve_owner(uint32_t, ee::t_ee_translated_entry);',
        "namespace",
        "{",
        "struct s_bound { uint32_t m_owner, m_pc, m_offset, m_slots; };",
        "const uint32_t g_words[] = {",
        *("    " + ", ".join(words[i:i+8]) + "," for i in range(0, len(words), 8)),
        "    0u",
        "};",
        "const s_bound g_bounds[] = {",
        *entries,
        "    {0u, 0u, 0u, 0u}",
        "};",
        "}",
        'extern "C" EE_CERT_EXPORT const uint32_t* ee_game_alu_words(',
        "    uint32_t pc, ee::t_ee_translated_entry entry, uint32_t* slots)",
        "{",
        "    if (slots == nullptr) { return nullptr; }",
        "    *slots = 0u;",
        "    const uint32_t owner = ee_game_resolve_owner(pc, entry);",
        "    if (owner == 0u) { return nullptr; }",
        "    const auto key = std::make_pair(owner, pc);",
        f"    const auto* end = g_bounds + {len(entries)}u;",
        "    const auto* found = std::lower_bound(g_bounds, end, key,",
        "        [](const s_bound& bound, const std::pair<uint32_t, uint32_t>& wanted)",
        "        { return std::make_pair(bound.m_owner, bound.m_pc) < wanted; });",
        "    if (found == end || found->m_owner != owner || found->m_pc != pc)",
        "    { return nullptr; }",
        "    *slots = found->m_slots;",
        "    return g_words + found->m_offset;",
        "}",
        "",
    ])
