#include "ee_vu1_ir.h"
#include "ee_vu1_dependency.h"
#include "ee_vu1_aot.h"

#include <utility>

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

// Integer-only VU arithmetic matching ee_vu1_fp.h. No host FP,
// fast-math, or out-of-range shifts. These helpers are module-local.
constexpr const char* k_fp_ir = R"IR(
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define internal i32 @vu_flush(i32 %x) alwaysinline {
  %exp = and i32 %x, 2139095040
  %zero = icmp eq i32 %exp, 0
  %sign = and i32 %x, -2147483648
  %result = select i1 %zero, i32 %sign, i32 %x
  ret i32 %result
}

define internal i32 @vu_pack(i32 %sign, i32 %e, i32 %m) alwaysinline {
  %overflow = icmp sgt i32 %e, 255
  %underflow = icmp sle i32 %e, 0
  %exp = shl i32 %e, 23
  %mantissa = and i32 %m, 8388607
  %bits = or i32 %exp, %mantissa
  %normal = select i1 %underflow, i32 0, i32 %bits
  %mag = select i1 %overflow, i32 2147483647, i32 %normal
  %result = or i32 %sign, %mag
  ret i32 %result
}

define internal i32 @vu_add(i32 %a0, i32 %b0) alwaysinline {
  %a = call i32 @vu_flush(i32 %a0)
  %b = call i32 @vu_flush(i32 %b0)
  %am = and i32 %a, 2147483647
  %bm = and i32 %b, 2147483647
  %az = icmp eq i32 %am, 0
  %bz = icmp eq i32 %bm, 0
  br i1 %bz, label %zero_b, label %check_a
zero_b:
  %both_sign = and i32 %a, %b
  %zero_result = select i1 %az, i32 %both_sign, i32 %a
  ret i32 %zero_result
check_a:
  br i1 %az, label %zero_a, label %calc
zero_a:
  ret i32 %b
calc:
  %swap = icmp ugt i32 %bm, %am
  %large = select i1 %swap, i32 %b, i32 %a
  %small = select i1 %swap, i32 %a, i32 %b
  %large_mag = and i32 %large, 2147483647
  %small_mag = and i32 %small, 2147483647
  %e = lshr i32 %large_mag, 23
  %se = lshr i32 %small_mag, 23
  %diff = sub i32 %e, %se
  %far = icmp ugt i32 %diff, 25
  %safe_diff = select i1 %far, i32 0, i32 %diff
  %ma0 = and i32 %large_mag, 8388607
  %mb0 = and i32 %small_mag, 8388607
  %ma1 = or i32 %ma0, 8388608
  %mb1 = or i32 %mb0, 8388608
  %ma = shl i32 %ma1, 1
  %mb2 = shl i32 %mb1, 1
  %mb3 = lshr i32 %mb2, %safe_diff
  %mb = select i1 %far, i32 0, i32 %mb3
  %sign = and i32 %large, -2147483648
  %xor = xor i32 %large, %small
  %opposite = icmp slt i32 %xor, 0
  br i1 %opposite, label %subtract, label %same
subtract:
  %delta = sub i32 %ma, %mb
  %cancel = icmp eq i32 %delta, 0
  br i1 %cancel, label %cancelled, label %normalize
cancelled:
  ret i32 0
normalize:
  %lz = call i32 @llvm.ctlz.i32(i32 %delta, i1 false)
  %shift = sub i32 %lz, 7
  %norm = shl i32 %delta, %shift
  %ne = sub i32 %e, %shift
  %nm = lshr i32 %norm, 1
  %nr = call i32 @vu_pack(i32 %sign, i32 %ne, i32 %nm)
  ret i32 %nr
same:
  %sum = add i32 %ma, %mb
  %carry_bit = and i32 %sum, 33554432
  %carry = icmp ne i32 %carry_bit, 0
  %half = lshr i32 %sum, 1
  %adjust = select i1 %carry, i32 %half, i32 %sum
  %inc = zext i1 %carry to i32
  %ee = add i32 %e, %inc
  %mm = lshr i32 %adjust, 1
  %rr = call i32 @vu_pack(i32 %sign, i32 %ee, i32 %mm)
  ret i32 %rr
}

define internal i32 @vu_sub(i32 %a, i32 %b) alwaysinline {
  %neg = xor i32 %b, -2147483648
  %r = call i32 @vu_add(i32 %a, i32 %neg)
  ret i32 %r
}

define internal i32 @vu_mul(i32 %a, i32 %b) alwaysinline {
  %xor = xor i32 %a, %b
  %sign = and i32 %xor, -2147483648
  %ae = lshr i32 %a, 23
  %be = lshr i32 %b, 23
  %ea = and i32 %ae, 255
  %eb = and i32 %be, 255
  %az = icmp eq i32 %ea, 0
  %bz = icmp eq i32 %eb, 0
  %zero = or i1 %az, %bz
  br i1 %zero, label %signed_zero, label %calc
signed_zero:
  ret i32 %sign
calc:
  %a0 = and i32 %a, 8388607
  %b0 = and i32 %b, 8388607
  %a1 = or i32 %a0, 8388608
  %b1 = or i32 %b0, 8388608
  %a2 = zext i32 %a1 to i64
  %b2 = zext i32 %b1 to i64
  %product = mul i64 %a2, %b2
  %high = lshr i64 %product, 47
  %carry = icmp ne i64 %high, 0
  %shift = select i1 %carry, i64 24, i64 23
  %mantissa = lshr i64 %product, %shift
  %m = trunc i64 %mantissa to i32
  %e0 = add i32 %ea, %eb
  %e1 = sub i32 %e0, 127
  %inc = zext i1 %carry to i32
  %e = add i32 %e1, %inc
  %r = call i32 @vu_pack(i32 %sign, i32 %e, i32 %m)
  ret i32 %r
}

define internal i32 @vu_key(i32 %raw) alwaysinline {
  %x = call i32 @vu_flush(i32 %raw)
  %mag = and i32 %x, 2147483647
  %negative = icmp slt i32 %x, 0
  %neg = sub i32 0, %mag
  %r = select i1 %negative, i32 %neg, i32 %mag
  ret i32 %r
}

define internal i32 @vu_max(i32 %a, i32 %b) alwaysinline {
  %ak = call i32 @vu_key(i32 %a)
  %bk = call i32 @vu_key(i32 %b)
  %pick_a = icmp sge i32 %ak, %bk
  %r = select i1 %pick_a, i32 %a, i32 %b
  ret i32 %r
}

define internal i32 @vu_min(i32 %a, i32 %b) alwaysinline {
  %ak = call i32 @vu_key(i32 %a)
  %bk = call i32 @vu_key(i32 %b)
  %pick_a = icmp sle i32 %ak, %bk
  %r = select i1 %pick_a, i32 %a, i32 %b
  ret i32 %r
}
)IR";

class c_straight_line_emitter
{
public:
    c_straight_line_emitter()
    {
        m_ir = k_fp_ir;
        m_ir += "define dso_local i32 @ee_vu1_program(ptr %vi, ptr %i, "
            "ptr %tpc, i32 %top, i32 %itop, ptr %vf, i32 %q, "
            "ptr %mac, ptr %status, ptr %data) {\nentry:\n";
    }

    bool emit(uint32_t lo, uint32_t hi)
    {
        // Compute upper operands before the lower executes, then
        // retire upper writes after lower writeback, as in the interpreter.
        m_upper_mask = 0u;
        m_upper_flags = false;
        if ((hi & 0x38000000u) != 0u || !emit_upper(hi) ||
            !emit_lower(lo, hi))
        {
            return false;
        }
        commit_upper();
        return true;
    }

    bool emit_lower(uint32_t lo, uint32_t hi)
    {
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
        const uint32_t ft = (lo >> 16u) & 31u;
        const uint32_t fs = (lo >> 11u) & 31u;
        const uint32_t mask = (lo >> 21u) & 15u;
        const int32_t raw = static_cast<int32_t>(lo & 0x7ffu);
        const int32_t offset = raw >= 1024 ? raw - 2048 : raw;
        if (major == 0x00u || major == 0x01u ||
            major == 0x04u || major == 0x05u)
        {
            const auto address = data_address(major == 0x01u ? it : is, offset);
            switch (major)
            {
            case 0x00u: load_vector(ft, mask, address); break;
            case 0x01u: store_vector(fs, mask, address); break;
            case 0x04u: load_integer(it, mask, address, false); break;
            case 0x05u: store_integer(it, mask, address); break;
            }
            return true;
        }
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
            if (index == 0x34u || index == 0x36u)
            {
                // LQD pre-decrements; LQI post-increments, including
                // suppressed VI0 updates and 16-bit integer wrapping.
                if (index == 0x36u)
                {
                    binary("sub", is, is, "1");
                }
                load_vector(ft, mask, data_address(is, 0));
                if (index == 0x34u)
                {
                    binary("add", is, is, "1");
                }
                return true;
            }
            if (index == 0x35u || index == 0x37u)
            {
                if (index == 0x37u)
                {
                    binary("sub", it, it, "1");
                }
                store_vector(fs, mask, data_address(it, 0));
                if (index == 0x35u)
                {
                    binary("add", it, it, "1");
                }
                return true;
            }
            if (index == 0x3eu)
            {
                load_integer(it, mask, data_address(is, 0), true);
                return true;
            }
            if (index == 0x3fu)
            {
                store_integer(it, mask, data_address(is, 0));
                return true;
            }
            if (index == 0x30u || index == 0x31u)
            {
                std::array<std::string, 4> values{};
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((mask & (8u >> lane)) != 0u)
                    {
                        values[lane] = vf(fs, index == 0x30u ? lane : (lane + 1u) & 3u);
                    }
                }
                write_vector(ft, mask, values);
                return true;
            }
            if (index == 0x3cu) // MTIR
            {
                store(it, operation("trunc i32 " + vf(fs, (lo >> 21u) & 3u) + " to i16"));
                return true;
            }
            if (index == 0x3du) // MFIR
            {
                const auto value = operation("sext i16 " + reg(is) + " to i32");
                const std::array<std::string, 4> values{value, value, value, value};
                write_vector(ft, mask, values);
                return true;
            }
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

    bool writes_data() const { return m_writes_data; }

    std::string finish(uint32_t steps, uint32_t next_pc)
    {
        line("store i32 " + std::to_string(next_pc) + ", ptr %tpc, align 4");
        line("ret i32 " + std::to_string(steps));
        m_ir += "}\n";
        return m_ir;
    }

private:
    std::string operation(const std::string& instruction)
    {
        const auto value = tmp();
        line(value + " = " + instruction);
        return value;
    }

    std::string vf_pointer(uint32_t reg_index, uint32_t lane)
    {
        return operation("getelementptr i32, ptr %vf, i32 " +
            std::to_string(reg_index * 4u + lane));
    }

    std::string vf(uint32_t reg_index, uint32_t lane)
    {
        if (reg_index == 0u)
        {
            return lane == 3u ? "1065353216" : "0";
        }
        return operation("load i32, ptr " + vf_pointer(reg_index, lane) +
            ", align 4");
    }

    std::string data_address(uint32_t reg_index, int32_t offset)
    {
        const auto base = operation("zext i16 " + reg(reg_index) + " to i32");
        const auto sum = operation("add i32 " + base + ", " + std::to_string(offset));
        const auto wrapped = operation("and i32 " + sum + ", 1023");
        return operation("shl i32 " + wrapped + ", 4");
    }

    std::string data_pointer(const std::string& address, uint32_t lane)
    {
        const auto offset = operation("add i32 " + address + ", " +
            std::to_string(lane * 4u));
        return operation("getelementptr i8, ptr %data, i32 " + offset);
    }

    std::string data_lane(const std::string& address, uint32_t lane)
    {
        return operation("load i32, ptr " + data_pointer(address, lane) + ", align 1");
    }

    void store_data_lane(const std::string& address, uint32_t lane,
        const std::string& value)
    {
        line("store i32 " + value + ", ptr " + data_pointer(address, lane) +
            ", align 1");
        m_writes_data = true;
    }

    void store_vector(uint32_t source, uint32_t mask, const std::string& address)
    {
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, vf(source, lane));
            }
        }
    }

    void store_integer(uint32_t source, uint32_t mask, const std::string& address)
    {
        const auto value = operation("zext i16 " + reg(source) + " to i32");
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, value);
            }
        }
    }

    void write_vector(uint32_t target, uint32_t mask,
        const std::array<std::string, 4>& values)
    {
        if (target != 0u)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    line("store i32 " + values[lane] + ", ptr " +
                        vf_pointer(target, lane) + ", align 4");
                }
            }
        }
    }

    void load_vector(uint32_t target, uint32_t mask, const std::string& address)
    {
        std::array<std::string, 4> values{};
        if (target != 0u)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    values[lane] = data_lane(address, lane);
                }
            }
            write_vector(target, mask, values);
        }
    }

    void load_integer(uint32_t target, uint32_t mask, const std::string& address,
        bool first_lane)
    {
        // ILW uses the last selected lane, ILWR the first selected lane.
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store(target, operation("trunc i32 " + data_lane(address, lane) + " to i16"));
                if (first_lane)
                {
                    break;
                }
            }
        }
    }

    void commit_upper()
    {
        write_vector(m_upper_target, m_upper_mask, m_upper_values);
        if (m_upper_flags)
        {
            emit_flags(m_upper_values, m_upper_mask);
        }
    }

    void emit_flags(const std::array<std::string, 4>& values, uint32_t mask)
    {
        std::string mac = "0";
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            const uint32_t bit = 8u >> lane;
            if ((mask & bit) == 0u)
            {
                continue;
            }
            const auto exp = operation("and i32 " + values[lane] + ", 2139095040");
            const auto zero = operation("icmp eq i32 " + exp + ", 0");
            const auto sign = operation("icmp slt i32 " + values[lane] + ", 0");
            const auto mag = operation("and i32 " + values[lane] + ", 2147483647");
            const auto overflow = operation("icmp eq i32 " + mag + ", 2147483647");
            for (const auto& test : {std::make_pair(zero, bit),
                                     std::make_pair(sign, bit << 4u),
                                     std::make_pair(overflow, bit << 12u)})
            {
                const auto flag = operation("select i1 " + test.first +
                    ", i32 " + std::to_string(test.second) + ", i32 0");
                mac = operation("or i32 " + mac + ", " + flag);
            }
        }
        line("store i32 " + mac + ", ptr %mac, align 4");
        const auto old = operation("load i32, ptr %status, align 4");
        auto status = operation("and i32 " + old + ", 4080");
        for (uint32_t group = 0u; group < 4u; ++group)
        {
            const auto bits = operation("and i32 " + mac + ", " +
                std::to_string(15u << (group * 4u)));
            const auto any = operation("icmp ne i32 " + bits + ", 0");
            const auto flag = operation("select i1 " + any + ", i32 " +
                std::to_string((1u << group) | (1u << (group + 6u))) + ", i32 0");
            status = operation("or i32 " + status + ", " + flag);
        }
        line("store i32 " + status + ", ptr %status, align 4");
    }

    bool emit_upper(uint32_t hi)
    {
        const uint32_t funct = hi & 63u;
        const bool special = funct >= 0x3cu;
        const uint32_t op = special ?
            (((hi >> 6u) & 31u) << 2u) | (funct & 3u) : funct;
        const uint32_t mask = (hi >> 21u) & 15u;
        const uint32_t fs = (hi >> 11u) & 31u;
        const uint32_t ft = (hi >> 16u) & 31u;
        const uint32_t fd = (hi >> 6u) & 31u;
        if (special && op == 0x2fu)
        {
            return true;
        }

        const bool abs = special && op == 0x1du;
        const char* helper = nullptr;
        if (!special)
        {
            if (op <= 0x03u || op == 0x20u || op == 0x22u || op == 0x28u)
            {
                helper = "vu_add";
            }
            else if ((op >= 0x04u && op <= 0x07u) ||
                op == 0x24u || op == 0x26u || op == 0x2cu)
            {
                helper = "vu_sub";
            }
            else if ((op >= 0x18u && op <= 0x1cu) || op == 0x1eu || op == 0x2au)
            {
                helper = "vu_mul";
            }
            else if ((op >= 0x10u && op <= 0x13u) || op == 0x1du || op == 0x2bu)
            {
                helper = "vu_max";
            }
            else if ((op >= 0x14u && op <= 0x17u) || op == 0x1fu || op == 0x2fu)
            {
                helper = "vu_min";
            }
        }
        if (!abs && helper == nullptr)
        {
            return false;
        }
        const bool flags = !abs && !(op >= 0x10u && op <= 0x17u) &&
            op != 0x1du && op != 0x1fu && op != 0x2bu && op != 0x2fu;
        const bool uses_q = op == 0x1cu || op == 0x20u || op == 0x24u;
        const bool uses_i = op == 0x1du || op == 0x1eu || op == 0x1fu ||
            op == 0x22u || op == 0x26u;
        std::array<std::string, 4> values{};
        // Read all lanes before any destination store (broadcast aliases
        // and source/destination overlap must see the original values).
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) == 0u)
            {
                continue;
            }
            const auto lhs = vf(fs, lane);
            if (abs)
            {
                values[lane] = operation("and i32 " + lhs + ", 2147483647");
            }
            else
            {
                const auto rhs = op <= 0x1bu ? vf(ft, funct & 3u) :
                    uses_q ? "%q" : uses_i ?
                    operation("load i32, ptr %i, align 4") : vf(ft, lane);
                values[lane] = operation(std::string("call i32 @") + helper +
                    "(i32 " + lhs + ", i32 " + rhs + ")");
            }
        }
        m_upper_target = abs ? ft : fd;
        m_upper_mask = mask;
        m_upper_values = std::move(values);
        m_upper_flags = flags;
        return true;
    }

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
    std::array<std::string, 4> m_upper_values{};
    uint32_t m_upper_target = 0u;
    uint32_t m_upper_mask = 0u;
    bool m_upper_flags = false;
    bool m_writes_data = false;
    uint32_t m_counter = 0u;
};
} // namespace

s_ee_vu1_ir_program vu1_lift_straight_line_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    if ((address & 7u) != 0u)
    {
        return {};
    }
    c_straight_line_emitter emitter;
    s_ee_vu1_dependency dependencies{};
    uint32_t cycle = 16u;
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
        // With no branches, Q/P producers or dynamic waits, the
        // same scoreboard can calculate all issue cycles at lift time.
        const auto use = s_ee_vu1_dependency::decode(lo, hi);
        cycle = dependencies.issue(use, cycle + 1u);
        dependencies.retire(use, cycle);
        pc = (pc + 8u) & 0x3fffu;
        if (ending)
        {
            return {emitter.finish(steps, pc), steps, cycle - 16u, pc,
                emitter.writes_data()};
        }
        ending = (hi & 0x40000000u) != 0u;
    }
    return {};
}
std::string vu1_aot_module_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address, bool windows_exports)
{
    auto program = vu1_lift_straight_line_ir(code, address);
    if (program.m_ir.empty())
    {
        return {};
    }
    const uint32_t start = address & 0x3ff8u;
    const std::string linkage = windows_exports ? "dso_local dllexport " : "dso_local ";
    if (windows_exports)
    {
        const auto at = program.m_ir.find("define dso_local i32 @ee_vu1_program");
        program.m_ir.replace(at, sizeof("define dso_local ") - 1u,
            "define " + linkage);
    }
    program.m_ir += "\n@vu1_metadata = private constant [5 x i32] [i32 " +
        std::to_string(k_vu1_aot_abi) + ", i32 " + std::to_string(start) +
        ", i32 " + std::to_string(program.m_steps) + ", i32 " +
        std::to_string(program.m_cycles) + ", i32 " +
        std::to_string(program.m_next_pc) + "]\n";
    const uint32_t size = program.m_steps * 8u;
    program.m_ir += "@vu1_code = private constant [" +
        std::to_string(size) + " x i8] c\"";
    constexpr char hex[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < size; ++i)
    {
        const uint8_t byte = code[(start + i) & 0x3fffu];
        program.m_ir += '\\';
        program.m_ir += hex[byte >> 4u];
        program.m_ir += hex[byte & 15u];
    }
    program.m_ir += "\"\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_info() { ret ptr @vu1_metadata }\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_code() { ret ptr @vu1_code }\n";
    return program.m_ir;
}
} // namespace ee
