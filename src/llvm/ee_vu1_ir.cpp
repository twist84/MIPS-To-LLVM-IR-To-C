#include "ee_vu1_ir.h"

namespace ee
{
namespace
{
uint32_t read_word(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

class c_integer_emitter
{
public:
    c_integer_emitter()
    {
        m_ir = "define dso_local i32 @ee_vu1_integer_program(ptr %vi, ptr %i, "
            "ptr %tpc, i32 %top, i32 %itop) {\nentry:\n";
    }

    bool emit(uint32_t lo, uint32_t hi)
    {
        // Only the canonical upper NOP with optional I/E bits. No
        // vector writes, flag producers, debug interrupts or M bit.
        if ((hi & 0x3fffffffu) != 0x000002ffu)
        {
            return false;
        }
        if ((hi & 0x80000000u) != 0u)
        {
            line("store i32 " + std::to_string(lo) + ", ptr %i, align 4");
            return true;
        }
        if (lo == 0x8000033cu)
        {
            return true;
        }

        const uint32_t major = lo >> 25u;
        const uint32_t it = (lo >> 16u) & 15u;
        const uint32_t is = (lo >> 11u) & 15u;
        const uint32_t id = (lo >> 6u) & 15u;
        const uint32_t funct = lo & 63u;
        if (major == 0x08u || major == 0x09u)
        {
            const uint32_t imm = ((lo >> 10u) & 0x7800u) | (lo & 0x7ffu);
            binary(major == 0x08u ? "add" : "sub", it, is,
                std::to_string(imm));
            return true;
        }
        if (major != 0x40u)
        {
            return false;
        }
        switch (funct)
        {
        case 0x30u: binary("add", id, is, reg(it)); return true;
        case 0x31u: binary("sub", id, is, reg(it)); return true;
        case 0x34u: binary("and", id, is, reg(it)); return true;
        case 0x35u: binary("or", id, is, reg(it)); return true;
        case 0x32u:
        {
            const int32_t raw = static_cast<int32_t>((lo >> 6u) & 31u);
            binary("add", it, is, std::to_string(raw >= 16 ? raw - 32 : raw));
            return true;
        }
        default:
            break;
        }
        if (funct >= 0x3cu)
        {
            const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (funct & 3u);
            if (index == 0x68u || index == 0x69u)
            {
                const auto value = tmp();
                line(value + " = trunc i32 " +
                    (index == 0x68u ? "%top" : "%itop") + " to i16");
                store(it, value);
                return true;
            }
        }
        return false;
    }

    std::string finish(uint32_t steps, uint32_t next_pc)
    {
        line("store i32 " + std::to_string(next_pc) + ", ptr %tpc, align 4");
        line("ret i32 " + std::to_string(steps));
        m_ir += "}\n";
        return m_ir;
    }

private:
    std::string tmp()
    {
        return "%v" + std::to_string(m_counter++);
    }

    void line(const std::string& text)
    {
        m_ir += "  " + text + "\n";
    }

    std::string pointer(uint32_t reg_index)
    {
        const auto result = tmp();
        line(result + " = getelementptr i16, ptr %vi, i32 " +
            std::to_string(reg_index));
        return result;
    }

    std::string reg(uint32_t reg_index)
    {
        if (reg_index == 0u)
        {
            return "0";
        }
        const auto address = pointer(reg_index);
        const auto value = tmp();
        line(value + " = load i16, ptr " + address + ", align 2");
        return value;
    }

    void store(uint32_t reg_index, const std::string& value)
    {
        if (reg_index != 0u)
        {
            const auto address = pointer(reg_index);
            line("store i16 " + value + ", ptr " + address + ", align 2");
        }
    }

    void binary(const char* op, uint32_t target, uint32_t source,
        const std::string& rhs)
    {
        if (target != 0u)
        {
            const auto lhs = reg(source);
            const auto result = tmp();
            // No nsw/nuw: VI arithmetic wraps at 16 bits.
            line(result + " = " + op + " i16 " + lhs + ", " + rhs);
            store(target, result);
        }
    }

    std::string m_ir;
    uint32_t m_counter = 0u;
};
} // namespace

s_ee_vu1_ir_program vu1_lift_integer_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    if ((address & 7u) != 0u)
    {
        return {};
    }
    c_integer_emitter emitter;
    uint32_t pc = address & 0x3ff8u;
    bool ending = false;
    // A straight-line program must encounter E within one revolution
    // of micro memory, followed by its actual delay-slot pair.
    for (uint32_t steps = 1u; steps <= 2049u; ++steps)
    {
        const uint32_t lo = read_word(code.data() + pc);
        const uint32_t hi = read_word(code.data() + pc + 4u);
        if (!emitter.emit(lo, hi))
        {
            return {};
        }
        pc = (pc + 8u) & 0x3fffu;
        if (ending)
        {
            // No supported operation produces a scoreboard stall.
            return {emitter.finish(steps, pc), steps, steps, pc};
        }
        ending = (hi & 0x40000000u) != 0u;
    }
    return {};
}
} // namespace ee
