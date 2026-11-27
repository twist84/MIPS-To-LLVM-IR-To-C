#include "iop_ir_lifter.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <functional>

namespace iop
{
namespace
{
// s_iop_aot_context byte offsets (checked by static_assert in iop_aot.cpp).
constexpr int k_ctx_gpr = 0;
constexpr int k_ctx_hi = 8;
constexpr int k_ctx_lo = 16;
constexpr int k_ctx_ram = 24;
constexpr int k_ctx_cpu = 32;
constexpr int k_ctx_read = 40;
constexpr int k_ctx_write = 48;
constexpr int k_ctx_budget = 56;
constexpr int k_ctx_executed = 60;
constexpr int k_ctx_pc = 64;
constexpr int k_ctx_next_pc = 68;
constexpr int k_ctx_in_delay = 72;
constexpr int k_ctx_branch_pc = 76;

std::string c32(uint32_t value)
{
    return std::to_string(static_cast<int32_t>(value));
}

std::string hex8(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

struct s_fields
{
    uint32_t op, rs, rt, rd, sa, funct, imm;
    int32_t simm;
};

s_fields fields(uint32_t w)
{
    s_fields f{};
    f.op = w >> 26;
    f.rs = (w >> 21) & 31u;
    f.rt = (w >> 16) & 31u;
    f.rd = (w >> 11) & 31u;
    f.sa = (w >> 6) & 31u;
    f.funct = w & 63u;
    f.imm = w & 0xffffu;
    f.simm = static_cast<int16_t>(f.imm);
    return f;
}

bool is_control(uint32_t w)
{
    const s_fields f = fields(w);
    if (f.op == 0u)
    {
        return f.funct == 0x08u || f.funct == 0x09u;
    }
    if (f.op == 1u)
    {
        return f.rt == 0x00u || f.rt == 0x01u || f.rt == 0x10u || f.rt == 0x11u;
    }
    return f.op >= 0x02u && f.op <= 0x07u;
}

// Instructions translated inline (also as delay slots). Everything else
// is interpreted.
bool is_simple(uint32_t w)
{
    const s_fields f = fields(w);
    if (f.op == 0u)
    {
        switch (f.funct)
        {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1a: case 0x1b:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: case 0x2a: case 0x2b:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:
            return true;
        default:
            return false;
        }
    }
    switch (f.op)
    {
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
    case 0x0e: case 0x0f: case 0x20: case 0x21: case 0x23: case 0x24:
    case 0x25: case 0x28: case 0x29: case 0x2b:
        return true;
    default:
        return false;
    }
}

// Resume state on a failed (interpreted) instruction.
struct s_fail
{
    uint32_t m_pc = 0u;
    std::string m_next;      // i32 value
    bool m_delay = false;
    uint32_t m_branch = 0u;
};

class c_emitter
{
public:
    c_emitter(const s_iop_ir_module& module, std::string& out,
              const std::vector<uint32_t>* pcs = nullptr)
        : m_module(module), m_out(out), m_region_pcs(pcs)
    {
        m_size = static_cast<uint32_t>(module.m_text.size() & ~size_t(3));
    }

    bool owns(uint32_t pc) const
    {
        return m_region_pcs == nullptr ? contains(pc) :
            std::binary_search(m_region_pcs->begin(), m_region_pcs->end(), pc);
    }

    void for_each_pc(const std::function<void(uint32_t)>& callback) const
    {
        if (m_region_pcs != nullptr)
        {
            for (uint32_t pc : *m_region_pcs)
            {
                callback(pc);
            }
        }
        else
        {
            for (uint32_t pc = m_module.m_base; pc < m_module.m_base + m_size; pc += 4u)
            {
                callback(pc);
            }
        }
    }

    bool contains(uint32_t pc) const
    {
        return pc >= m_module.m_base && pc < m_module.m_base + m_size &&
               (pc & 3u) == 0u;
    }

    uint32_t word(uint32_t pc) const
    {
        const size_t offset = pc - m_module.m_base;
        return static_cast<uint32_t>(m_module.m_text[offset]) |
               (static_cast<uint32_t>(m_module.m_text[offset + 1]) << 8) |
               (static_cast<uint32_t>(m_module.m_text[offset + 2]) << 16) |
               (static_cast<uint32_t>(m_module.m_text[offset + 3]) << 24);
    }

    void emit_function(const std::string& symbol)
    {
        line("define dso_local i32 @" + symbol + "(ptr %c, i32 %start) {");
        line("entry:");
        line("  %n = alloca i32");
        line("  %pcs = alloca i32");
        line("  %ldv = alloca i32");
        line("  %xpc = alloca i32");
        line("  %xnext = alloca i32");
        line("  %xdelay = alloca i32");
        line("  %xbranch = alloca i32");
        line("  %xreason = alloca i32");
        line("  %gpr = load ptr, ptr %c");
        line("  %hip = getelementptr i8, ptr %c, i64 " + std::to_string(k_ctx_hi));
        line("  %hi = load ptr, ptr %hip");
        line("  %lop = getelementptr i8, ptr %c, i64 " + std::to_string(k_ctx_lo));
        line("  %lo = load ptr, ptr %lop");
        line("  %budgetp = getelementptr i8, ptr %c, i64 " + std::to_string(k_ctx_budget));
        line("  %budget = load i32, ptr %budgetp");
        for (uint32_t r = 1u; r < 32u; ++r)
        {
            line("  %g" + std::to_string(r) +
                 " = getelementptr i32, ptr %gpr, i64 " + std::to_string(r));
        }
        line("  store i32 0, ptr %n");
        line("  store i32 %start, ptr %pcs");
        line("  br label %dispatch");

        line("dispatch:");
        line("  %pcv = load i32, ptr %pcs");
        std::string cases;
        for_each_pc([&](uint32_t pc)
        {
            cases += "    i32 " + c32(pc) + ", label %L_" + hex8(pc) + "\n";
        });
        line("  switch i32 %pcv, label %unknown [\n" + cases + "  ]");
        line("unknown:");
        {
            const std::string next = tmp();
            line("  " + next + " = add i32 %pcv, 4");
            exit_to("%pcv", next, "0", "0", 0);
        }

        for_each_pc([&](uint32_t pc)
        {
            line("L_" + hex8(pc) + ":");
            emit_instruction(pc);
        });

        line("done:");
        line("  %nv = load i32, ptr %n");
        store_ctx(k_ctx_executed, "%nv");
        store_ctx_from(k_ctx_pc, "%xpc");
        store_ctx_from(k_ctx_next_pc, "%xnext");
        store_ctx_from(k_ctx_in_delay, "%xdelay");
        store_ctx_from(k_ctx_branch_pc, "%xbranch");
        line("  %reason = load i32, ptr %xreason");
        line("  ret i32 %reason");
        line("}");
    }

private:
    void line(const std::string& text)
    {
        m_out += text;
        m_out += '\n';
    }

    std::string tmp() { return "%t" + std::to_string(m_counter++); }
    std::string label() { return "b" + std::to_string(m_counter++); }

    void store_ctx(int offset, const std::string& value)
    {
        const std::string p = tmp();
        line("  " + p + " = getelementptr i8, ptr %c, i64 " + std::to_string(offset));
        line("  store i32 " + value + ", ptr " + p);
    }

    void store_ctx_from(int offset, const std::string& slot)
    {
        const std::string v = tmp();
        line("  " + v + " = load i32, ptr " + slot);
        store_ctx(offset, v);
    }

    std::string reg(uint32_t r)
    {
        if (r == 0u)
        {
            return "0";
        }
        const std::string t = tmp();
        line("  " + t + " = load i32, ptr %g" + std::to_string(r));
        return t;
    }

    void set(uint32_t r, const std::string& value)
    {
        if (r != 0u)
        {
            line("  store i32 " + value + ", ptr %g" + std::to_string(r));
        }
    }

    void add_n(uint32_t count)
    {
        const std::string a = tmp();
        const std::string b = tmp();
        line("  " + a + " = load i32, ptr %n");
        line("  " + b + " = add i32 " + a + ", " + std::to_string(count));
        line("  store i32 " + b + ", ptr %n");
    }

    // Terminates the current block.
    void exit_to(const std::string& pc, const std::string& next,
                 const std::string& delay, const std::string& branch,
                 uint32_t reason)
    {
        line("  store i32 " + pc + ", ptr %xpc");
        line("  store i32 " + next + ", ptr %xnext");
        line("  store i32 " + delay + ", ptr %xdelay");
        line("  store i32 " + branch + ", ptr %xbranch");
        line("  store i32 " + std::to_string(reason) + ", ptr %xreason");
        line("  br label %done");
    }

    void exit_const(uint32_t pc)
    {
        exit_to(c32(pc), c32(pc + 4u), "0", "0", 0);
    }

    void interpret(const s_fail& fail)
    {
        exit_to(c32(fail.m_pc), fail.m_next, fail.m_delay ? "1" : "0",
                c32(fail.m_delay ? fail.m_branch : 0u), 1);
    }

    // if (!ok) interpret(fail); continues in a fresh block.
    void guard(const std::string& ok, const s_fail& fail)
    {
        const std::string good = label();
        const std::string bad = label();
        line("  br i1 " + ok + ", label %" + good + ", label %" + bad);
        line(bad + ":");
        interpret(fail);
        line(good + ":");
    }

    // Unconditional transfer to a constant target (terminates the block).
    void jump(uint32_t source, uint32_t target)
    {
        if (!owns(target))
        {
            exit_const(target);
            return;
        }
        // Check the slice budget on every internal transfer. Branch
        // retirement checks separately before executing its delay slot.
        (void)source;
        const std::string nv = tmp();
        const std::string over = tmp();
        const std::string stop = label();
        line("  " + nv + " = load i32, ptr %n");
        line("  " + over + " = icmp uge i32 " + nv + ", %budget");
        line("  br i1 " + over + ", label %" + stop + ", label %L_" + hex8(target));
        line(stop + ":");
        exit_const(target);
    }

    std::string sext_imm(int32_t value) { return std::to_string(value); }

    // Emits a simple instruction; on success increments n. Does not
    // terminate the block.
    void emit_simple(uint32_t pc, uint32_t w, const s_fail& fail)
    {
        const s_fields f = fields(w);
        if (f.op == 0u)
        {
            const uint32_t funct = f.funct;
            if (funct == 0x10u || funct == 0x12u)
            {
                const std::string v = tmp();
                line("  " + v + " = load i32, ptr " +
                     std::string(funct == 0x10u ? "%hi" : "%lo"));
                set(f.rd, v);
            }
            else if (funct == 0x11u || funct == 0x13u)
            {
                const std::string l = reg(f.rs);
                line("  store i32 " + l + ", ptr " +
                     std::string(funct == 0x11u ? "%hi" : "%lo"));
            }
            else if (funct == 0x00u || funct == 0x02u || funct == 0x03u)
            {
                const std::string t = reg(f.rt);
                const std::string v = tmp();
                const char* opname = funct == 0x00u ? "shl" :
                                     funct == 0x02u ? "lshr" : "ashr";
                line("  " + v + " = " + opname + " i32 " + t + ", " +
                     std::to_string(f.sa));
                set(f.rd, v);
            }
            else if (funct == 0x04u || funct == 0x06u || funct == 0x07u)
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                const std::string amount = tmp();
                const std::string v = tmp();
                const char* opname = funct == 0x04u ? "shl" :
                                     funct == 0x06u ? "lshr" : "ashr";
                line("  " + amount + " = and i32 " + l + ", 31");
                line("  " + v + " = " + opname + " i32 " + t + ", " + amount);
                set(f.rd, v);
            }
            else if (funct == 0x18u || funct == 0x19u)
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                const char* ext = funct == 0x18u ? "sext" : "zext";
                const std::string a = tmp(), b = tmp(), p = tmp();
                const std::string lo = tmp(), hs = tmp(), hi = tmp();
                line("  " + a + " = " + ext + " i32 " + l + " to i64");
                line("  " + b + " = " + ext + " i32 " + t + " to i64");
                line("  " + p + " = mul i64 " + a + ", " + b);
                line("  " + lo + " = trunc i64 " + p + " to i32");
                line("  " + hs + " = lshr i64 " + p + ", 32");
                line("  " + hi + " = trunc i64 " + hs + " to i32");
                line("  store i32 " + lo + ", ptr %lo");
                line("  store i32 " + hi + ", ptr %hi");
            }
            else if (funct == 0x1au || funct == 0x1bu)
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                line(std::string("  call void @") +
                     (funct == 0x1au ? "iop_divs" : "iop_divu") +
                     "(ptr %hi, ptr %lo, i32 " + l + ", i32 " + t + ")");
            }
            else if (funct == 0x20u || funct == 0x22u)
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                const std::string pair = tmp(), v = tmp(), o = tmp(), ok = tmp();
                const char* intrinsic = funct == 0x20u
                    ? "llvm.sadd.with.overflow.i32" : "llvm.ssub.with.overflow.i32";
                line("  " + pair + " = call {i32, i1} @" + intrinsic +
                     "(i32 " + l + ", i32 " + t + ")");
                line("  " + v + " = extractvalue {i32, i1} " + pair + ", 0");
                line("  " + o + " = extractvalue {i32, i1} " + pair + ", 1");
                line("  " + ok + " = xor i1 " + o + ", true");
                guard(ok, fail);
                set(f.rd, v);
            }
            else if (funct >= 0x30u && funct <= 0x36u)
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                const char* pred =
                    funct == 0x30u ? "slt" :   // tge: trap if !(l < r)
                    funct == 0x31u ? "ult" :   // tgeu
                    funct == 0x32u ? "sge" :   // tlt: trap if !(l >= r)
                    funct == 0x33u ? "uge" :   // tltu
                    funct == 0x34u ? "ne" :    // teq
                                     "eq";     // tne
                const std::string ok = tmp();
                line("  " + ok + " = icmp " + pred + " i32 " + l + ", " + t);
                guard(ok, fail);
            }
            else
            {
                const std::string l = reg(f.rs);
                const std::string t = reg(f.rt);
                const std::string v = tmp();
                switch (funct)
                {
                case 0x21u: line("  " + v + " = add i32 " + l + ", " + t); break;
                case 0x23u: line("  " + v + " = sub i32 " + l + ", " + t); break;
                case 0x24u: line("  " + v + " = and i32 " + l + ", " + t); break;
                case 0x25u: line("  " + v + " = or i32 " + l + ", " + t); break;
                case 0x26u: line("  " + v + " = xor i32 " + l + ", " + t); break;
                case 0x27u:
                {
                    const std::string o = tmp();
                    line("  " + o + " = or i32 " + l + ", " + t);
                    line("  " + v + " = xor i32 " + o + ", -1");
                    break;
                }
                case 0x2au:
                case 0x2bu:
                {
                    const std::string cmp = tmp();
                    line("  " + cmp + " = icmp " +
                         std::string(funct == 0x2au ? "slt" : "ult") +
                         " i32 " + l + ", " + t);
                    line("  " + v + " = zext i1 " + cmp + " to i32");
                    break;
                }
                default:
                    break;
                }
                set(f.rd, v);
            }
            add_n(1u);
            return;
        }

        switch (f.op)
        {
        case 0x08u:
        {
            const std::string l = reg(f.rs);
            const std::string pair = tmp(), v = tmp(), o = tmp(), ok = tmp();
            line("  " + pair + " = call {i32, i1} @llvm.sadd.with.overflow.i32(i32 " +
                 l + ", i32 " + sext_imm(f.simm) + ")");
            line("  " + v + " = extractvalue {i32, i1} " + pair + ", 0");
            line("  " + o + " = extractvalue {i32, i1} " + pair + ", 1");
            line("  " + ok + " = xor i1 " + o + ", true");
            guard(ok, fail);
            set(f.rt, v);
            break;
        }
        case 0x09u:
        {
            const std::string l = reg(f.rs);
            const std::string v = tmp();
            line("  " + v + " = add i32 " + l + ", " + sext_imm(f.simm));
            set(f.rt, v);
            break;
        }
        case 0x0au:
        case 0x0bu:
        {
            const std::string l = reg(f.rs);
            const std::string cmp = tmp(), v = tmp();
            line("  " + cmp + " = icmp " + std::string(f.op == 0x0au ? "slt" : "ult") +
                 " i32 " + l + ", " + sext_imm(f.simm));
            line("  " + v + " = zext i1 " + cmp + " to i32");
            set(f.rt, v);
            break;
        }
        case 0x0cu:
        case 0x0du:
        case 0x0eu:
        {
            const std::string l = reg(f.rs);
            const std::string v = tmp();
            const char* opname = f.op == 0x0cu ? "and" : f.op == 0x0du ? "or" : "xor";
            line("  " + v + " = " + opname + " i32 " + l + ", " + std::to_string(f.imm));
            set(f.rt, v);
            break;
        }
        case 0x0fu:
            set(f.rt, c32(f.imm << 16));
            break;
        case 0x20u:
        case 0x21u:
        case 0x23u:
        case 0x24u:
        case 0x25u:
        {
            const uint32_t width =
                (f.op == 0x20u || f.op == 0x24u) ? 1u :
                (f.op == 0x21u || f.op == 0x25u) ? 2u : 4u;
            const std::string l = reg(f.rs);
            const std::string address = tmp(), ok = tmp(), raw = tmp();
            line("  " + address + " = add i32 " + l + ", " + sext_imm(f.simm));
            line("  " + ok + " = call i1 @iop_ld(ptr %c, i32 " + address + ", i32 " +
                 std::to_string(width) + ", ptr %ldv)");
            guard(ok, fail);
            line("  " + raw + " = load i32, ptr %ldv");
            std::string value = raw;
            if (f.op == 0x20u || f.op == 0x21u)
            {
                const std::string narrow = tmp();
                value = tmp();
                const char* type = f.op == 0x20u ? "i8" : "i16";
                line("  " + narrow + " = trunc i32 " + raw + " to " + type);
                line("  " + value + " = sext " + type + " " + narrow + " to i32");
            }
            set(f.rt, value);
            break;
        }
        case 0x28u:
        case 0x29u:
        case 0x2bu:
        {
            const uint32_t width = f.op == 0x28u ? 1u : f.op == 0x29u ? 2u : 4u;
            const std::string l = reg(f.rs);
            const std::string t = reg(f.rt);
            const std::string address = tmp(), ok = tmp();
            line("  " + address + " = add i32 " + l + ", " + sext_imm(f.simm));
            line("  " + ok + " = call i1 @iop_st(ptr %c, i32 " + address + ", i32 " +
                 std::to_string(width) + ", i32 " + t + ")");
            guard(ok, fail);
            break;
        }
        default:
            break;
        }
        add_n(1u);
    }

    // A branch can consume the last available instruction. Preserve
    // the selected successor and delay-slot identity for the interpreter
    // instead of executing the slot beyond the kernel slice budget.
    void check_delay_budget(uint32_t branch_pc, const std::string& next)
    {
        const std::string nv = tmp(), over = tmp();
        const std::string stop = label(), execute = label();
        line("  " + nv + " = load i32, ptr %n");
        line("  " + over + " = icmp uge i32 " + nv + ", %budget");
        line("  br i1 " + over + ", label %" + stop + ", label %" + execute);
        line(stop + ":");
        exit_to(c32(branch_pc + 4u), next, "1", c32(branch_pc), 0);
        line(execute + ":");
    }

    void emit_instruction(uint32_t pc)
    {
        const uint32_t w = word(pc);
        const s_fields f = fields(w);
        const s_fail here{pc, c32(pc + 4u), false, 0u};
        if (is_simple(w))
        {
            emit_simple(pc, w, here);
            jump(pc, pc + 4u);
            return;
        }
        const uint32_t delay_pc = pc + 4u;
        if (!is_control(w) || !contains(delay_pc) || !is_simple(word(delay_pc)))
        {
            interpret(here);
            return;
        }
        const uint32_t delay = word(delay_pc);
        const uint32_t fall = pc + 8u;

        if (f.op == 0u)
        {
            // JR / JALR: capture the target before the delay slot.
            const std::string target = reg(f.rs);
            if (f.funct == 0x09u)
            {
                set(f.rd, c32(fall));
            }
            add_n(1u);
            check_delay_budget(pc, target);
            emit_simple(delay_pc, delay, s_fail{delay_pc, target, true, pc});
            line("  store i32 " + target + ", ptr %pcs");
            const std::string nv = tmp(), over = tmp(), next = tmp();
            const std::string stop = label();
            line("  " + nv + " = load i32, ptr %n");
            line("  " + over + " = icmp uge i32 " + nv + ", %budget");
            line("  br i1 " + over + ", label %" + stop + ", label %dispatch");
            line(stop + ":");
            line("  " + next + " = add i32 " + target + ", 4");
            exit_to(target, next, "0", "0", 0);
            return;
        }
        if (f.op == 0x02u || f.op == 0x03u)
        {
            const uint32_t target = ((pc + 4u) & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
            if (f.op == 0x03u)
            {
                set(31u, c32(fall));
            }
            add_n(1u);
            check_delay_budget(pc, c32(target));
            emit_simple(delay_pc, delay, s_fail{delay_pc, c32(target), true, pc});
            jump(pc, target);
            return;
        }
        const uint32_t target = pc + 4u + (static_cast<uint32_t>(f.simm) << 2);
        const std::string l = reg(f.rs);
        const std::string taken = tmp();
        switch (f.op)
        {
        case 0x04u: line("  " + taken + " = icmp eq i32 " + l + ", " + reg(f.rt)); break;
        case 0x05u: line("  " + taken + " = icmp ne i32 " + l + ", " + reg(f.rt)); break;
        case 0x06u: line("  " + taken + " = icmp sle i32 " + l + ", 0"); break;
        case 0x07u: line("  " + taken + " = icmp sgt i32 " + l + ", 0"); break;
        default:
            line("  " + taken + " = icmp " +
                 std::string(f.rt == 0x00u || f.rt == 0x10u ? "slt" : "sge") +
                 " i32 " + l + ", 0");
            break;
        }
        if (f.op == 0x01u && (f.rt == 0x10u || f.rt == 0x11u))
        {
            set(31u, c32(fall));
        }
        add_n(1u);
        const std::string next = tmp();
        line("  " + next + " = select i1 " + taken + ", i32 " + c32(target) +
             ", i32 " + c32(fall));
        check_delay_budget(pc, next);
        emit_simple(delay_pc, delay, s_fail{delay_pc, next, true, pc});
        const std::string yes = label();
        const std::string no = label();
        line("  br i1 " + taken + ", label %" + yes + ", label %" + no);
        line(yes + ":");
        jump(pc, target);
        line(no + ":");
        jump(pc, fall);
    }

    const s_iop_ir_module& m_module;
    std::string& m_out;
    const std::vector<uint32_t>* m_region_pcs = nullptr;
    uint32_t m_size = 0u;
    uint64_t m_counter = 0u;
};

const char* k_helpers = R"IR(declare {i32, i1} @llvm.sadd.with.overflow.i32(i32, i32)
declare {i32, i1} @llvm.ssub.with.overflow.i32(i32, i32)

; Same mapping as c_cpu::read/write: KUSEG/KSEG0/KSEG1, aligned, 2 MiB RAM
; mirrored through the first 8 MiB physical. Everything else (MMIO, faults)
; goes through the runtime slow path.
define internal i1 @iop_ram_ok(i32 %a, i32 %w) alwaysinline {
entry:
  %region = and i32 %a, -536870912
  %r0 = icmp eq i32 %region, 0
  %r8 = icmp eq i32 %region, -2147483648
  %ra = icmp eq i32 %region, -1610612736
  %r08 = or i1 %r0, %r8
  %rok = or i1 %r08, %ra
  %wm = sub i32 %w, 1
  %mis = and i32 %a, %wm
  %al = icmp eq i32 %mis, 0
  %phys = and i32 %a, 536870911
  %inr = icmp ult i32 %phys, 8388608
  %ok1 = and i1 %rok, %al
  %ok = and i1 %ok1, %inr
  ret i1 %ok
}

define internal ptr @iop_ram_ptr(ptr %c, i32 %a) alwaysinline {
entry:
  %ramp = getelementptr i8, ptr %c, i64 24
  %ram = load ptr, ptr %ramp
  %off = and i32 %a, 2097151
  %off64 = zext i32 %off to i64
  %p = getelementptr i8, ptr %ram, i64 %off64
  ret ptr %p
}

define internal i1 @iop_ld(ptr %c, i32 %a, i32 %w, ptr %out) alwaysinline {
entry:
  %ok = call i1 @iop_ram_ok(i32 %a, i32 %w)
  br i1 %ok, label %fast, label %slow
fast:
  %p = call ptr @iop_ram_ptr(ptr %c, i32 %a)
  switch i32 %w, label %b1 [ i32 4, label %b4
                             i32 2, label %b2 ]
b4:
  %v4 = load i32, ptr %p, align 1
  store i32 %v4, ptr %out
  ret i1 true
b2:
  %v2 = load i16, ptr %p, align 1
  %v2z = zext i16 %v2 to i32
  store i32 %v2z, ptr %out
  ret i1 true
b1:
  %v1 = load i8, ptr %p, align 1
  %v1z = zext i8 %v1 to i32
  store i32 %v1z, ptr %out
  ret i1 true
slow:
  %fpp = getelementptr i8, ptr %c, i64 40
  %fp = load ptr, ptr %fpp
  %cpup = getelementptr i8, ptr %c, i64 32
  %cpu = load ptr, ptr %cpup
  %r = call i32 %fp(ptr %cpu, i32 %a, i32 %w, ptr %out)
  %rb = icmp ne i32 %r, 0
  ret i1 %rb
}

define internal i1 @iop_st(ptr %c, i32 %a, i32 %w, i32 %v) alwaysinline {
entry:
  %ok = call i1 @iop_ram_ok(i32 %a, i32 %w)
  br i1 %ok, label %fast, label %slow
fast:
  %p = call ptr @iop_ram_ptr(ptr %c, i32 %a)
  switch i32 %w, label %b1 [ i32 4, label %b4
                             i32 2, label %b2 ]
b4:
  store i32 %v, ptr %p, align 1
  ret i1 true
b2:
  %v2 = trunc i32 %v to i16
  store i16 %v2, ptr %p, align 1
  ret i1 true
b1:
  %v1 = trunc i32 %v to i8
  store i8 %v1, ptr %p, align 1
  ret i1 true
slow:
  %fpp = getelementptr i8, ptr %c, i64 48
  %fp = load ptr, ptr %fpp
  %cpup = getelementptr i8, ptr %c, i64 32
  %cpu = load ptr, ptr %cpup
  %r = call i32 %fp(ptr %cpu, i32 %a, i32 %w, i32 %v)
  %rb = icmp ne i32 %r, 0
  ret i1 %rb
}

; c_cpu DIV/DIVU, including the R3000 divide-by-zero and overflow results.
define internal void @iop_divs(ptr %hi, ptr %lo, i32 %l, i32 %r) alwaysinline {
entry:
  %z = icmp eq i32 %r, 0
  br i1 %z, label %zero, label %nz
zero:
  %neg = icmp slt i32 %l, 0
  %q0 = select i1 %neg, i32 1, i32 -1
  store i32 %q0, ptr %lo
  store i32 %l, ptr %hi
  ret void
nz:
  %mn = icmp eq i32 %l, -2147483648
  %m1 = icmp eq i32 %r, -1
  %ovf = and i1 %mn, %m1
  br i1 %ovf, label %ov, label %norm
ov:
  store i32 %l, ptr %lo
  store i32 0, ptr %hi
  ret void
norm:
  %q = sdiv i32 %l, %r
  %m = srem i32 %l, %r
  store i32 %q, ptr %lo
  store i32 %m, ptr %hi
  ret void
}

define internal void @iop_divu(ptr %hi, ptr %lo, i32 %l, i32 %r) alwaysinline {
entry:
  %z = icmp eq i32 %r, 0
  br i1 %z, label %zero, label %nz
zero:
  store i32 -1, ptr %lo
  store i32 %l, ptr %hi
  ret void
nz:
  %q = udiv i32 %l, %r
  %m = urem i32 %l, %r
  store i32 %q, ptr %lo
  store i32 %m, ptr %hi
  ret void
}

)IR";
} // namespace

std::string iop_lift_module_ir(const s_iop_ir_module& module,
                               const std::string& symbol)
{
    std::string out;
    out.reserve(module.m_text.size() * 160u);
    out += "; IOP IRX " + module.m_name + " at 0x" + hex8(module.m_base) +
           " (" + std::to_string(module.m_text.size()) + " bytes)\n";
    out += "; Generated by iop_recomp. Do not edit.\n\n";
    out += k_helpers;
    c_emitter emitter(module, out);
    emitter.emit_function(symbol);
    return out;
}

bool iop_lift_regions_ir(const s_iop_ir_module& module,
    const std::vector<std::vector<uint32_t>>& regions,
    const std::string& symbol, std::vector<std::string>* code,
    std::string* dispatcher)
{
    if (code == nullptr || dispatcher == nullptr || regions.empty() ||
        module.m_text.empty() || (module.m_base & 3u) != 0u ||
        (module.m_text.size() & 3u) != 0u ||
        static_cast<uint64_t>(module.m_base) + module.m_text.size() > UINT32_MAX)
    {
        return false;
    }
    std::set<uint32_t> owned;
    for (const auto& region : regions)
    {
        if (region.empty() || !std::is_sorted(region.begin(), region.end()))
        {
            return false;
        }
        for (uint32_t pc : region)
        {
            if ((pc & 3u) != 0u || pc < module.m_base ||
                static_cast<uint64_t>(pc) + 4u >
                    static_cast<uint64_t>(module.m_base) + module.m_text.size() ||
                !owned.insert(pc).second)
            {
                return false;
            }
        }
    }

    std::vector<std::string> generated;
    std::string router = "; Split IOP module dispatcher. No cross-region chaining.\n";
    for (size_t i = 0u; i < regions.size(); ++i)
    {
        const std::string name = symbol + "_region_" + std::to_string(i);
        std::string ir = k_helpers;
        c_emitter emitter(module, ir, &regions[i]);
        emitter.emit_function(name);
        generated.push_back(std::move(ir));
        router += "declare i32 @" + name + "(ptr, i32)\n";
    }
    router += "define dso_local i32 @" + symbol + "(ptr %c, i32 %start) {\n"
        "entry:\n"
        "  %budgetp = getelementptr i8, ptr %c, i64 " + std::to_string(k_ctx_budget) + "\n"
        "  %budget = load i32, ptr %budgetp\n"
        "  %empty = icmp eq i32 %budget, 0\n"
        "  br i1 %empty, label %fallback, label %dispatch\n"
        "dispatch:\n"
        "  switch i32 %start, label %fallback [\n";
    for (size_t i = 0u; i < regions.size(); ++i)
    {
        for (uint32_t pc : regions[i])
        {
            router += "    i32 " + c32(pc) + ", label %region_" + std::to_string(i) + "\n";
        }
    }
    router += "  ]\n";
    for (size_t i = 0u; i < regions.size(); ++i)
    {
        const std::string n = std::to_string(i);
        router += "region_" + n + ":\n  %result_" + n + " = call i32 @" +
            symbol + "_region_" + n + "(ptr %c, i32 %start)\n"
            "  ret i32 %result_" + n + "\n";
    }
    router += "fallback:\n  %next = add i32 %start, 4\n";
    const auto store = [&](const char* name, int offset, const char* value)
    {
        router += "  %" + std::string(name) + " = getelementptr i8, ptr %c, i64 " +
            std::to_string(offset) + "\n  store i32 " + value + ", ptr %" + name + "\n";
    };
    store("executedp", k_ctx_executed, "0");
    store("pcp", k_ctx_pc, "%start");
    store("nextp", k_ctx_next_pc, "%next");
    store("delayp", k_ctx_in_delay, "0");
    store("branchp", k_ctx_branch_pc, "0");
    router += "  %reason = select i1 %empty, i32 0, i32 1\n  ret i32 %reason\n}\n";
    *code = std::move(generated);
    *dispatcher = std::move(router);
    return true;
}

std::string iop_table_module_ir(const std::vector<s_iop_ir_table_entry>& entries,
                                bool windows_exports)
{
    std::string out = "; IOP AOT module table. Generated by iop_recomp.\n\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const std::string& name = entries[i].m_name;
        out += "@name" + std::to_string(i) + " = private constant [" +
               std::to_string(name.size() + 1) + " x i8] c\"" + name + "\\00\"\n";
        out += "declare i32 @" + entries[i].m_symbol + "(ptr, i32)\n";
    }
    out += "\n@modules = private constant [" + std::to_string(entries.size()) +
           " x {ptr, i32, i32, i64, ptr}] [\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        out += "  {ptr, i32, i32, i64, ptr} {ptr @name" + std::to_string(i) +
               ", i32 " + c32(entries[i].m_base) + ", i32 " +
               c32(entries[i].m_size) + ", i64 " +
               std::to_string(static_cast<int64_t>(entries[i].m_hash)) +
               ", ptr @" + entries[i].m_symbol + "}" +
               (i + 1 < entries.size() ? ",\n" : "\n");
    }
    out += "]\n\n";
    const std::string exported = windows_exports ? "dllexport " : "";
    out += "define " + exported + "i32 @iop_aot_module_count() {\n  ret i32 " +
           std::to_string(entries.size()) + "\n}\n\n";
    out += "define " + exported + "ptr @iop_aot_modules() {\n  ret ptr @modules\n}\n";
    return out;
}
} // namespace iop
