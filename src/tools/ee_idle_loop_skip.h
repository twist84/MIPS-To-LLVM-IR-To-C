#pragma once

// Host-only acceleration for translated guest *poll* loops.
//
// Inside one native slice nothing outside the EE can change guest RAM or the
// status registers such a loop reads: the IOP, DMA, scanout and interrupts are
// all serviced by the runner between native calls. A loop that only loads,
// computes and branches, and carries no register from one iteration to the
// next, therefore repeats the identical iteration until its translated
// loop-header safepoint budget is exhausted.
//
// The runner uses this analysis to run exactly ONE real iteration natively
// (all guest state comes from the translated code) and then charges the
// instruction counters for the remaining budgeted iterations. The HOST_YIELD
// it reports is identical to a full native slice: same PC, registers, exit
// reason, dispatch count, and guest instruction/reference-slot totals.

#include "../architecture/ee_scalar_constant.h"

#include <concepts>
#include <cstdint>
#include <span>
#include <vector>

namespace ee
{

struct s_idle_loop_shape
{
    bool eligible = false;
    uint32_t header_pc = 0u;
    // PC of the conditional or unconditional branch back to header_pc.
    uint32_t backedge_pc = 0u;
    // Instruction words from header_pc through the backedge delay slot.
    std::vector<uint32_t> words;
    const char* reason = "not analyzed";
    uint32_t carried_read_pc = 0u;
    uint32_t carried_read_register = 0u;
};

enum class e_idle_iteration : uint8_t
{
    loops,
    exits,
    unsupported
};

namespace idle_loop_detail
{
constexpr uint32_t k_max_words = 32u;

inline constexpr uint32_t op(uint32_t w) { return w >> 26; }
inline constexpr uint32_t rs(uint32_t w) { return (w >> 21) & 31u; }
inline constexpr uint32_t rt(uint32_t w) { return (w >> 16) & 31u; }
inline constexpr uint32_t rd(uint32_t w) { return (w >> 11) & 31u; }
inline constexpr uint32_t sa(uint32_t w) { return (w >> 6) & 31u; }
inline constexpr uint32_t funct(uint32_t w) { return w & 63u; }
inline constexpr int64_t simm(uint32_t w) { return static_cast<int16_t>(w & 0xffffu); }
inline constexpr uint64_t zimm(uint32_t w) { return w & 0xffffu; }
inline constexpr uint64_t sext32(uint64_t v)
{
    return scalar_constant::sign_extend<32>(v);
}

enum class e_kind : uint8_t
{
    invalid,
    alu,     // writes a GPR from GPR/immediate sources
    load,    // writes a GPR from guest memory
    branch   // conditional (non-likely) or unconditional relative branch
};

struct s_decoded
{
    e_kind kind = e_kind::invalid;
    uint32_t dest = 0u;         // 0 when nothing architectural is written
    uint32_t sources[2] = {0u, 0u};
    uint32_t source_count = 0u;
    uint32_t width = 0u;        // load width in bytes
};

inline constexpr s_decoded decode(uint32_t w)
{
    s_decoded d{};
    auto set = [&](e_kind kind, uint32_t dest,
                   uint32_t first, uint32_t second, uint32_t count)
    {
        d.kind = kind;
        d.dest = dest;
        d.sources[0] = first;
        d.sources[1] = second;
        d.source_count = count;
    };
    switch (op(w))
    {
    case 0x00u:
        switch (funct(w))
        {
        case 0x00u: case 0x02u: case 0x03u:              // SLL SRL SRA
        case 0x38u: case 0x3au: case 0x3bu:              // DSLL DSRL DSRA
        case 0x3cu: case 0x3eu: case 0x3fu:              // DSLL32 DSRL32 DSRA32
            if (rs(w) == 0u) set(e_kind::alu, rd(w), rt(w), 0u, 1u);
            break;
        case 0x21u: case 0x23u: case 0x24u: case 0x25u:  // ADDU SUBU AND OR
        case 0x26u: case 0x27u: case 0x2au: case 0x2bu:  // XOR NOR SLT SLTU
        case 0x2du: case 0x2fu:                          // DADDU DSUBU
            if (sa(w) == 0u) set(e_kind::alu, rd(w), rs(w), rt(w), 2u);
            break;
        default:
            break;
        }
        break;
    case 0x01u:                                          // REGIMM BLTZ/BGEZ
        if (rt(w) == 0u || rt(w) == 1u) set(e_kind::branch, 0u, rs(w), 0u, 1u);
        break;
    case 0x04u: case 0x05u:                              // BEQ BNE
        set(e_kind::branch, 0u, rs(w), rt(w), 2u);
        break;
    case 0x06u: case 0x07u:                              // BLEZ BGTZ
        if (rt(w) == 0u) set(e_kind::branch, 0u, rs(w), 0u, 1u);
        break;
    case 0x09u: case 0x0au: case 0x0bu: case 0x0cu:      // ADDIU SLTI SLTIU ANDI
    case 0x0du: case 0x0eu: case 0x19u:                  // ORI XORI DADDIU
        set(e_kind::alu, rt(w), rs(w), 0u, 1u);
        break;
    case 0x0fu:                                          // LUI
        if (rs(w) == 0u) set(e_kind::alu, rt(w), 0u, 0u, 0u);
        break;
    case 0x20u: case 0x24u: d.width = 1u; set(e_kind::load, rt(w), rs(w), 0u, 1u); break;
    case 0x21u: case 0x25u: d.width = 2u; set(e_kind::load, rt(w), rs(w), 0u, 1u); break;
    case 0x23u: case 0x27u: d.width = 4u; set(e_kind::load, rt(w), rs(w), 0u, 1u); break;
    case 0x37u:             d.width = 8u; set(e_kind::load, rt(w), rs(w), 0u, 1u); break;
    default:
        break;
    }
    return d;
}

inline constexpr uint32_t branch_target(uint32_t pc, uint32_t w)
{
    return pc + 4u + static_cast<uint32_t>(simm(w) * 4);
}

inline constexpr bool branch_taken(uint32_t w, const uint64_t* gpr)
{
    const uint64_t a = gpr[rs(w)];
    const uint64_t b = gpr[rt(w)];
    switch (op(w))
    {
    case 0x01u: return rt(w) == 0u ? static_cast<int64_t>(a) < 0
                                   : static_cast<int64_t>(a) >= 0;
    case 0x04u: return a == b;
    case 0x05u: return a != b;
    case 0x06u: return static_cast<int64_t>(a) <= 0;
    case 0x07u: return static_cast<int64_t>(a) > 0;
    default: return false;
    }
}

inline constexpr uint64_t alu(uint32_t w, const uint64_t* gpr)
{
    const uint64_t s = gpr[rs(w)];
    const uint64_t t = gpr[rt(w)];
    switch (op(w))
    {
    case 0x00u:
        switch (funct(w))
        {
        case 0x00u: return sext32(static_cast<uint32_t>(t) << sa(w));
        case 0x02u: return sext32(static_cast<uint32_t>(t) >> sa(w));
        case 0x03u: return sext32(scalar_constant::arithmetic_right<32>(t, sa(w)));
        case 0x38u: return t << sa(w);
        case 0x3au: return t >> sa(w);
        case 0x3bu: return scalar_constant::arithmetic_right<64>(t, sa(w));
        case 0x3cu: return t << (sa(w) + 32u);
        case 0x3eu: return t >> (sa(w) + 32u);
        case 0x3fu: return scalar_constant::arithmetic_right<64>(t, sa(w) + 32u);
        case 0x21u: return sext32(s + t);
        case 0x23u: return sext32(s - t);
        case 0x24u: return s & t;
        case 0x25u: return s | t;
        case 0x26u: return s ^ t;
        case 0x27u: return ~(s | t);
        case 0x2au: return scalar_constant::signed_less(s, t) ? 1u : 0u;
        case 0x2bu: return s < t ? 1u : 0u;
        case 0x2du: return s + t;
        case 0x2fu: return s - t;
        default: return 0u;
        }
    case 0x09u: return sext32(s + static_cast<uint64_t>(simm(w)));
    case 0x0au: return scalar_constant::signed_less(s, scalar_constant::sign_extend<16>(w)) ? 1u : 0u;
    case 0x0bu: return s < static_cast<uint64_t>(simm(w)) ? 1u : 0u;
    case 0x0cu: return s & zimm(w);
    case 0x0du: return s | zimm(w);
    case 0x0eu: return s ^ zimm(w);
    case 0x0fu: return sext32(zimm(w) << 16u);
    case 0x19u: return s + static_cast<uint64_t>(simm(w));
    default: return 0u;
    }
}

inline constexpr uint64_t extend_load(uint32_t w, uint64_t raw)
{
    switch (op(w))
    {
    case 0x20u: return scalar_constant::sign_extend<8>(raw);
    case 0x21u: return scalar_constant::sign_extend<16>(raw);
    case 0x23u: return sext32(raw);
    case 0x24u: return raw & 0xffu;
    case 0x25u: return raw & 0xffffu;
    case 0x27u: return raw & 0xffffffffu;
    default: return raw;  // LD
    }
}
} // namespace idle_loop_detail

// Decode a candidate loop at header_pc. read_word(pc, &word) returns false
// for unmapped code. Eligible loops are single-entry straight-line bodies of
// loads, ALU operations and non-likely branches; internal branches may only
// move forward to non-delay-slot instructions; no store, call, jump, syscall,
// coprocessor, trapping or branch-likely instruction is accepted; and no
// register may carry a value from one iteration into the next.
template <typename t_read>
concept t_ee_instruction_reader = requires(t_read& read, uint32_t pc, uint32_t* word)
{
    { read(pc, word) } -> std::same_as<bool>;
};

template <typename t_load>
concept t_ee_idle_load_reader = requires(t_load& load, uint32_t address,
                                        uint32_t width, uint64_t* raw)
{
    { load(address, width, raw) } -> std::same_as<bool>;
};

template <t_ee_instruction_reader t_read_word>
constexpr s_idle_loop_shape analyze_idle_loop(uint32_t header_pc, t_read_word read_word,
                                   bool allow_forward_branches = false)
{
    using namespace idle_loop_detail;
    s_idle_loop_shape shape{};
    shape.header_pc = header_pc;
    if ((header_pc & 3u) != 0u)
    {
        shape.reason = "unaligned header";
        return shape;
    }

    if (header_pc > 0xffffffffu - 4u * k_max_words)
    {
        shape.reason = "loop window wraps";
        return shape;
    }

    uint32_t end = 0u;  // index of the backedge delay slot
    for (uint32_t i = 0u; i < k_max_words; ++i)
    {
        uint32_t word = 0u;
        if (!read_word(header_pc + 4u * i, &word))
        {
            shape.reason = "unreadable code";
            return shape;
        }
        shape.words.push_back(word);
        const s_decoded d = decode(word);
        if (d.kind == e_kind::invalid)
        {
            shape.reason = "unsupported instruction";
            return shape;
        }
        if (d.kind == e_kind::branch)
        {
            if (i > 0u && decode(shape.words[i - 1u]).kind == e_kind::branch)
            {
                shape.reason = "branch in delay slot";
                return shape;
            }
            if (branch_target(header_pc + 4u * i, word) == header_pc)
            {
                uint32_t slot = 0u;
                if (!read_word(header_pc + 4u * (i + 1u), &slot))
                {
                    shape.reason = "unreadable delay slot";
                    return shape;
                }
                const s_decoded slot_decoded = decode(slot);
                if (slot_decoded.kind == e_kind::invalid ||
                    slot_decoded.kind == e_kind::branch)
                {
                    shape.reason = "unsupported backedge delay slot";
                    return shape;
                }
                shape.words.push_back(slot);
                shape.backedge_pc = header_pc + 4u * i;
                end = i + 1u;
                break;
            }
        }
        if (i + 1u == k_max_words)
        {
            shape.reason = "no backedge within window";
            return shape;
        }
    }

    const uint32_t loop_end_pc = header_pc + 4u * end;
    uint32_t written_anywhere = 0u;
    for (uint32_t i = 0u; i <= end; ++i)
    {
        const s_decoded d = decode(shape.words[i]);
        if (d.dest != 0u)
        {
            written_anywhere |= 1u << d.dest;
        }
        if (d.kind == e_kind::branch && i + 1u != end)
        {
            const uint32_t target = branch_target(header_pc + 4u * i, shape.words[i]);
            if (target >= header_pc && target <= loop_end_pc)
            {
                const uint32_t index = (target - header_pc) / 4u;
                // No secondary backedges or independent entry into a
                // delay slot. Only acyclic forward control flow is proved.
                if (!allow_forward_branches || index <= i + 1u ||
                    decode(shape.words[index - 1u]).kind == e_kind::branch)
                {
                    shape.reason = "branch into the loop body";
                    return shape;
                }
            }
        }
    }

    // Definite assignment over the acyclic body. At a merge, intersect
    // the sets of registers written on every incoming path. A read of
    // a register modified anywhere in the loop must be dominated by
    // a write on every path, including branch operands and delay slots.
    bool reached[k_max_words + 1u] = {};
    uint32_t written[k_max_words + 1u] = {};
    reached[0] = true;
    const auto merge = [&](uint32_t index, uint32_t mask)
    {
        if (index <= end)
        {
            if (reached[index])
            {
                written[index] &= mask;
            }
            else
            {
                reached[index] = true;
                written[index] = mask;
            }
        }
    };
    const auto apply = [&](const s_decoded& d, uint32_t& mask, uint32_t index)
    {
        for (uint32_t s = 0u; s < d.source_count; ++s)
        {
            const uint32_t reg = d.sources[s];
            if (reg != 0u && (written_anywhere & (1u << reg)) != 0u &&
                (mask & (1u << reg)) == 0u)
            {
                shape.carried_read_pc = header_pc + 4u * index;
                shape.carried_read_register = reg;
                return false;
            }
        }
        if (d.dest != 0u)
        {
            mask |= 1u << d.dest;
        }
        return true;
    };
    for (uint32_t i = 0u; i <= end; ++i)
    {
        if (!reached[i])
        {
            continue;
        }
        uint32_t mask = written[i];
        const s_decoded d = decode(shape.words[i]);
        if (!apply(d, mask, i))
        {
            shape.reason = "loop-carried register";
            return shape;
        }
        if (d.kind != e_kind::branch)
        {
            merge(i + 1u, mask);
            continue;
        }
        if (!apply(decode(shape.words[i + 1u]), mask, i + 1u))
        {
            shape.reason = "loop-carried register";
            return shape;
        }
        if (i + 1u != end)
        {
            merge(i + 2u, mask);
            const uint32_t target = branch_target(header_pc + 4u * i, shape.words[i]);
            if (target >= header_pc && target <= loop_end_pc)
            {
                merge((target - header_pc) / 4u, mask);
            }
        }
    }

    shape.eligible = true;
    shape.reason = "eligible";
    return shape;
}

// Predict, without changing guest state, whether the next iteration returns
// to the header. gpr holds the low 64 bits of each guest GPR. load(address,
// width, &raw) must return false for any address whose value could change
// during a native slice or whose read has side effects.
template <t_ee_idle_load_reader t_load>
constexpr e_idle_iteration predict_idle_iteration(
    const s_idle_loop_shape& shape, std::span<const uint64_t, 32> gpr_in, t_load load,
    uint32_t* loop_instruction_count = nullptr)
{
    using namespace idle_loop_detail;
    if (loop_instruction_count != nullptr)
    {
        *loop_instruction_count = 0u;
    }
    if (!shape.eligible || shape.words.size() < 2u)
    {
        return e_idle_iteration::unsupported;
    }
    uint64_t gpr[32];
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        gpr[i] = gpr_in[i];
    }
    gpr[0] = 0u;

    uint32_t executed = 0u;
    const uint32_t end = static_cast<uint32_t>(shape.words.size() - 1u);
    for (uint32_t i = 0u; i <= end; ++i)
    {
        ++executed;
        const uint32_t word = shape.words[i];
        const s_decoded d = decode(word);
        if (d.kind == e_kind::branch)
        {
            // Non-likely: the delay slot executes either way, but only
            // after the branch has read its operands.
            ++executed; // The non-likely delay slot always executes.
            const bool taken = branch_taken(word, gpr);
            const bool backedge = (i + 1u == end);
            const uint32_t slot = shape.words[i + 1u];
            const s_decoded slot_decoded = decode(slot);
            if (slot_decoded.kind == e_kind::load)
            {
                const uint32_t address = static_cast<uint32_t>(
                    gpr[rs(slot)] + static_cast<uint64_t>(simm(slot)));
                uint64_t raw = 0u;
                if ((address & (slot_decoded.width - 1u)) != 0u ||
                    !load(address, slot_decoded.width, &raw))
                {
                    return e_idle_iteration::unsupported;
                }
                if (slot_decoded.dest != 0u) gpr[slot_decoded.dest] = extend_load(slot, raw);
            }
            else if (slot_decoded.dest != 0u)
            {
                gpr[slot_decoded.dest] = alu(slot, gpr);
            }
            if (backedge)
            {
                if (taken && loop_instruction_count != nullptr)
                {
                    *loop_instruction_count = executed;
                }
                return taken ? e_idle_iteration::loops : e_idle_iteration::exits;
            }
            if (taken)
            {
                const uint32_t target = branch_target(shape.header_pc + 4u * i, word);
                if (target < shape.header_pc || target > shape.header_pc + 4u * end)
                {
                    return e_idle_iteration::exits;
                }
                // The analyser proved a forward target, not a delay slot.
                i = (target - shape.header_pc) / 4u - 1u;
                continue;
            }
            ++i;  // delay slot already executed
            continue;
        }
        if (d.kind == e_kind::load)
        {
            const uint32_t address = static_cast<uint32_t>(
                gpr[rs(word)] + static_cast<uint64_t>(simm(word)));
            uint64_t raw = 0u;
            if ((address & (d.width - 1u)) != 0u || !load(address, d.width, &raw))
            {
                return e_idle_iteration::unsupported;
            }
            if (d.dest != 0u) gpr[d.dest] = extend_load(word, raw);
        }
        else if (d.dest != 0u)
        {
            gpr[d.dest] = alu(word, gpr);
        }
    }
    return e_idle_iteration::exits;  // fell through without a backedge
}

} // namespace ee
