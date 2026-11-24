// Adapted from ps2-web-recomp:SDBZ by Samir Nuri (2026), MIT license.
// Upstream license: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#include "ee_vif1.h"
#include "ee_graphics_batch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ee
{
namespace
{
uint32_t element_bits(uint32_t vl)
{
    switch (vl)
    {
    case 0u: return 32u;
    case 1u: return 16u;
    case 2u: return 8u;
    default: return 16u;
    }
}

uint32_t read_element(const uint32_t* words, size_t* bit_position,
                      uint32_t bits, bool unsigned_value)
{
    const size_t index = *bit_position / 32u;
    const uint32_t shift = static_cast<uint32_t>(*bit_position & 31u);
    uint32_t value = words[index] >> shift;
    *bit_position += bits;
    if (bits == 16u)
    {
        value &= 0xffffu;
        if (!unsigned_value)
        {
            value = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int16_t>(value)));
        }
    }
    else if (bits == 8u)
    {
        value &= 0xffu;
        if (!unsigned_value)
        {
            value = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int8_t>(value)));
        }
    }
    return value;
}
} // namespace

void c_ee_vif1::bind_path2(c_ee_gif_path3* path2)
{
    m_path2 = path2;
}

void c_ee_vif1::bind_path1(c_ee_gif_path3* path1)
{
    m_path1 = path1;
}

void c_ee_vif1::bind_path3(const c_ee_gif_path3* path3)
{
    m_path3 = path3;
}

void c_ee_vif1::reset()
{
    m_machine = {};
    m_diagnostic = {};
}

const c_ee_vif1::s_diagnostic& c_ee_vif1::diagnostic() const
{
    return m_diagnostic;
}

const uint8_t* c_ee_vif1::vu1_data() const
{
    return m_machine.m_vu1_data.data();
}

const uint8_t* c_ee_vif1::vu1_code() const
{
    return m_machine.m_vu1_code.data();
}

uint32_t c_ee_vif1::command_data_words(
    const s_machine& machine, uint32_t code)
{
    const uint32_t op = (code >> 24u) & 0x7fu;
    const uint32_t num = (code >> 16u) & 0xffu;
    const uint32_t count = num != 0u ? num : 256u;
    switch (op)
    {
    case 0x20u: return 1u;
    case 0x30u:
    case 0x31u: return 4u;
    case 0x4au: return count * 2u;
    case 0x50u:
    case 0x51u:
        return (code & 0xffffu) != 0u
            ? (code & 0xffffu) * 4u : 0x10000u * 4u;
    default: break;
    }
    if ((op & 0x60u) == 0x60u)
    {
        const uint32_t vn = (code >> 26u) & 3u;
        const uint32_t vl = (code >> 24u) & 3u;
        const uint32_t cl = machine.m_cycle & 0xffu;
        const uint32_t wl = (machine.m_cycle >> 8u) & 0xffu;
        uint32_t consumed = count;
        if (wl > cl && wl != 0u)
        {
            consumed = (count / wl) * cl +
                std::min(count % wl, cl);
        }
        const uint32_t bits = vn == 3u && vl == 3u
            ? 16u : (vn + 1u) * element_bits(vl);
        return (consumed * bits + 31u) / 32u;
    }
    return 0u;
}

// VIF UNPACK sign extension, cycling, masks and VU1 memory wrap follow
// ps2-web-recomp:SDBZ runtime/src/vif.cpp::unpack.
void c_ee_vif1::unpack(s_machine& machine, uint32_t code,
                       const uint32_t* words)
{
    const uint32_t vn = (code >> 26u) & 3u;
    const uint32_t vl = (code >> 24u) & 3u;
    const bool masked = (code & 0x10000000u) != 0u;
    const bool unsigned_value = (code & 0x4000u) != 0u;
    uint32_t num = (code >> 16u) & 0xffu;
    if (num == 0u)
    {
        num = 256u;
    }

    uint32_t address = code & 0x3ffu;
    if ((code & 0x8000u) != 0u)
    {
        address += machine.m_tops;
    }
    const uint32_t cl = machine.m_cycle & 0xffu;
    const uint32_t wl = (machine.m_cycle >> 8u) & 0xffu;
    const uint32_t bits = element_bits(vl);
    size_t bit_position = 0u;
    uint32_t cycle = 0u;

    for (uint32_t n = 0u; n < num; ++n)
    {
        const bool fill = wl > cl && cycle >= cl;
        uint32_t values[4]{};
        if (!fill)
        {
            if (vn == 3u && vl == 3u)
            {
                const uint32_t packed = read_element(
                    words, &bit_position, 16u, true) & 0xffffu;
                values[0] = (packed & 0x1fu) << 3u;
                values[1] = ((packed >> 5u) & 0x1fu) << 3u;
                values[2] = ((packed >> 10u) & 0x1fu) << 3u;
                values[3] = ((packed >> 15u) & 1u) << 7u;
            }
            else
            {
                for (uint32_t lane = 0u; lane <= vn; ++lane)
                {
                    values[lane] = read_element(
                        words, &bit_position, bits, unsigned_value);
                }
                if (vn == 0u)
                {
                    values[1] = values[2] = values[3] = values[0];
                }
                else if (vn == 1u)
                {
                    values[2] = values[0];
                    values[3] = values[1];
                }
            }
        }

        uint8_t* destination = machine.m_vu1_data.data() +
            (address & 0x3ffu) * 16u;
        const uint32_t mask_row = std::min(cycle, 3u);
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            uint32_t select = masked
                ? (machine.m_mask >> ((mask_row * 4u + lane) * 2u)) & 3u
                : 0u;
            if (fill && select == 0u)
            {
                select = 1u;
            }
            if (select == 3u)
            {
                continue;
            }
            uint32_t output = values[lane];
            if (select == 1u)
            {
                output = machine.m_row[lane];
            }
            else if (select == 2u)
            {
                output = machine.m_col[mask_row];
            }
            else if (machine.m_mode == 1u || machine.m_mode == 2u)
            {
                output += machine.m_row[lane];
                if (machine.m_mode == 2u)
                {
                    machine.m_row[lane] = output;
                }
            }
            std::memcpy(destination + lane * 4u, &output, 4u);
        }

        ++address;
        ++cycle;
        if (cl >= wl)
        {
            if (cycle >= wl)
            {
                address += cl - wl;
                cycle = 0u;
            }
        }
        else if (cycle >= wl)
        {
            cycle = 0u;
        }
    }
    machine.m_num = 0u;
    ++machine.m_unpack_commands;
}

bool c_ee_vif1::execute(
    s_machine& machine, uint32_t code, const uint32_t* words,
    std::vector<uint8_t>& path2_bytes,
    std::vector<uint8_t>& path1_bytes,
    s_ee_vu1_issue* vu_issue, e_reject* reject)
{
    const uint32_t op = (code >> 24u) & 0x7fu;
    const uint32_t imm = code & 0xffffu;
    const uint32_t num = (code >> 16u) & 0xffu;
    bool accepted = true;
    machine.m_code = code;

    if ((code & 0x80000000u) != 0u && (machine.m_err & 1u) == 0u)
    {
        *reject = e_reject::irq_requires_resume;
        accepted = false;
    }
    else
    {
        switch (op)
        {
        case 0x00u: break; // NOP
        case 0x01u: machine.m_cycle = imm; break; // STCYCL
        case 0x02u: // OFFSET
            machine.m_ofst = imm & 0x3ffu;
            machine.m_dbf = false;
            machine.m_tops = machine.m_base;
            break;
        case 0x03u: machine.m_base = imm & 0x3ffu; break; // BASE
        case 0x04u: machine.m_itops = imm & 0x3ffu; break; // ITOP
        case 0x05u: machine.m_mode = imm & 3u; break; // STMOD
        case 0x07u: machine.m_mark = imm; break; // MARK
        case 0x10u: // FLUSHE
        case 0x11u: // FLUSH
        case 0x13u: // FLUSHA
            // The bounded VU engine runs synchronously to completion.
            break;
        case 0x20u: machine.m_mask = words[0]; break; // STMASK
        case 0x30u: // STROW
            std::memcpy(machine.m_row, words, sizeof(machine.m_row));
            break;
        case 0x31u: // STCOL
            std::memcpy(machine.m_col, words, sizeof(machine.m_col));
            break;
        case 0x4au: // MPG: 8 bytes per microinstruction
        {
            const uint32_t count = num != 0u ? num : 256u;
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t offset = ((imm + i) * 8u) & 0x3fffu;
                std::memcpy(machine.m_vu1_code.data() + offset,
                            words + i * 2u, 8u);
            }
            ++machine.m_mpg_uploads;
            break;
        }
        case 0x50u: // DIRECT -> GIF PATH2
        case 0x51u: // DIRECTHL -> GIF PATH2
        {
            const uint32_t qwc = imm != 0u ? imm : 0x10000u;
            const size_t previous = path2_bytes.size();
            path2_bytes.resize(previous + static_cast<size_t>(qwc) * 16u);
            for (uint32_t i = 0u; i < qwc * 4u; ++i)
            {
                const uint32_t word = words[i];
                for (uint32_t byte = 0u; byte < 4u; ++byte)
                {
                    path2_bytes[previous + static_cast<size_t>(i) * 4u + byte] =
                        static_cast<uint8_t>(word >> (byte * 8u));
                }
            }
            machine.m_direct_qwc += qwc;
            break;
        }
        case 0x06u: // MSKPATH3 needs actual GIF arbitration state.
            *reject = e_reject::unsupported_command;
            accepted = false;
            break;
        case 0x14u: // MSCAL
        case 0x15u: // MSCALF (PATH3 idle checked before execution)
        case 0x17u: // MSCNT
        {
            if (op == 0x17u && !machine.m_vu1_state.m_initialized)
            {
                *reject = e_reject::unsupported_vu_execution;
                accepted = false;
                break;
            }
            const uint32_t start = op == 0x17u
                ? machine.m_vu1_state.m_tpc : (imm & 0x7ffu) * 8u;
            accepted = c_ee_vu1::run(
                machine.m_vu1_state, machine.m_vu1_code,
                machine.m_vu1_data, start, machine.m_tops,
                machine.m_itops, path1_bytes, vu_issue);
            if (!accepted)
            {
                *reject = e_reject::unsupported_vu_execution;
            }
            else
            {
                // Match vif.cpp::run_vu: TOP records the buffer given
                // to this program, TOPS advances to the next buffer.
                machine.m_top = machine.m_tops;
                machine.m_itop = machine.m_itops;
                machine.m_dbf = !machine.m_dbf;
                machine.m_tops = machine.m_base +
                    (machine.m_dbf ? machine.m_ofst : 0u);
            }
            break;
        }
        default:
            if ((op & 0x60u) == 0x60u)
            {
                unpack(machine, code, words);
            }
            else
            {
                *reject = e_reject::unsupported_command;
                accepted = false;
            }
            break;
        }
    }
    if (accepted)
    {
        ++machine.m_commands;
        ++machine.m_committed_opcodes[op];
    }
    return accepted;
}

bool c_ee_vif1::consume(const uint8_t* data, uint32_t qwc)
{
    // A partial DIRECT can carry up to 65536 qwords plus its command.
    // Allow that residual in addition to the independently bounded batch.
    constexpr size_t k_max_pending_words =
        static_cast<size_t>(k_max_graphics_batch_qwc) * 4u + 0x40000u + 0x400u;
    // Host-only provenance. A failed DMA may be retried with identical
    // bytes, so rejection totals count attempts, not unique guest commands.
    const auto reject_packet = [this](e_reject reason, uint32_t code)
    {
        ++m_diagnostic.m_rejected_batches;
        if (m_diagnostic.m_first_rejection == e_reject::none)
        {
            m_diagnostic.m_first_rejection = reason;
            m_diagnostic.m_first_rejected_code = code;
        }
        m_diagnostic.m_reason = reason;
        m_diagnostic.m_code = code;
    };
    if (qwc > k_max_graphics_batch_qwc || (qwc != 0u && data == nullptr))
    {
        reject_packet(e_reject::invalid_packet, 0u);
        return false;
    }
    s_machine next = m_machine;
    if (next.m_pending.size() + static_cast<size_t>(qwc) * 4u >
        k_max_pending_words)
    {
        reject_packet(e_reject::pending_limit, 0u);
        return false;
    }
    const size_t previous = next.m_pending.size();
    next.m_pending.resize(previous + static_cast<size_t>(qwc) * 4u);
    for (size_t i = 0u; i < static_cast<size_t>(qwc) * 4u; ++i)
    {
        const size_t offset = i * 4u;
        next.m_pending[previous + i] =
            static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
            (static_cast<uint32_t>(data[offset + 2u]) << 16u) |
            (static_cast<uint32_t>(data[offset + 3u]) << 24u);
    }

    size_t index = 0u;
    std::vector<uint8_t> path2_bytes;
    std::vector<uint8_t> path1_bytes;
    s_ee_vu1_issue vu_issue{};
    e_reject rejection = e_reject::none;
    uint32_t rejected_code = 0u;
    while (index < next.m_pending.size())
    {
        const uint32_t code = next.m_pending[index];
        const uint32_t words = command_data_words(next, code);
        if (static_cast<size_t>(words) >
            next.m_pending.size() - index - 1u)
        {
            break; // Legitimate command continuation into the next DMA.
        }
        const uint32_t opcode = (code >> 24u) & 0x7fu;
        if (opcode == 0x14u || opcode == 0x15u || opcode == 0x17u)
        {
            ++m_diagnostic.m_vu_start_attempts;
            m_diagnostic.m_last_vu_start_code = code;
        }
        // MSCALF waits for PATH3 as well as VU1. Completed PATH3
        // transfers already reached GS in this synchronous runtime.
        // Fail closed for incomplete packets until stall/resume exists.
        if (opcode == 0x15u && m_path3 != nullptr && m_path3->state().m_active)
        {
            rejection = e_reject::mscalf_path3_busy;
            rejected_code = code;
            vu_issue = {};
            break;
        }
        if (!execute(next, code, next.m_pending.data() + index + 1u,
                     path2_bytes, path1_bytes, &vu_issue, &rejection))
        {
            rejected_code = code;
            break;
        }
        index += 1u + words;
    }

    if (rejection == e_reject::none &&
        !path1_bytes.empty() && !path2_bytes.empty())
    {
        rejection = e_reject::mixed_gif_paths;
    }
    if (rejection == e_reject::none && !path1_bytes.empty())
    {
        if (m_path1 == nullptr)
        {
            rejection = e_reject::missing_path1;
        }
        else if ((path1_bytes.size() & 15u) != 0u ||
                 path1_bytes.size() / 16u > k_max_graphics_batch_qwc ||
                 !m_path1->consume(path1_bytes.data(),
                     static_cast<uint32_t>(path1_bytes.size() / 16u)))
        {
            rejection = e_reject::path1_rejected;
        }
    }
    if (rejection == e_reject::none && !path2_bytes.empty())
    {
        if (m_path2 == nullptr)
        {
            rejection = e_reject::missing_path2;
        }
        else if (path2_bytes.size() / 16u > k_max_graphics_batch_qwc ||
                 !m_path2->consume(
                     path2_bytes.data(),
                     static_cast<uint32_t>(path2_bytes.size() / 16u)))
        {
            // Opt-in capture for reproducing a rejected DIRECT batch.
            const char* dump_path = std::getenv("EE_REJECTED_PATH2_DUMP");
            if (dump_path != nullptr && *dump_path != '\0')
            {
                if (auto* dump = std::fopen(dump_path, "wb"))
                {
                    const size_t written = std::fwrite(
                        path2_bytes.data(), 1, path2_bytes.size(), dump);
                    const int closed = std::fclose(dump);
                    if (written != path2_bytes.size() || closed != 0)
                    {
                        std::fprintf(stderr, "VIF1 rejected PATH2 dump incomplete\n");
                    }
                }
                else
                {
                    std::perror("VIF1 rejected PATH2 dump");
                }
            }
            rejection = e_reject::path2_rejected;
        }
    }
    if (rejection != e_reject::none)
    {
        reject_packet(rejection, rejected_code);
        m_diagnostic.m_last_rejected_staged_mpg = next.m_mpg_uploads - m_machine.m_mpg_uploads;
        m_diagnostic.m_last_rejected_staged_unpack = next.m_unpack_commands - m_machine.m_unpack_commands;
        if (rejection == e_reject::unsupported_vu_execution ||
            rejection == e_reject::mscalf_path3_busy ||
            rejection == e_reject::path1_rejected ||
            rejection == e_reject::missing_path1)
        {
            ++m_diagnostic.m_vu_start_rejections;
            m_diagnostic.m_last_rejected_vu_start_code =
                rejected_code != 0u ? rejected_code :
                m_diagnostic.m_last_vu_start_code;
            m_diagnostic.m_last_rejected_vu_pc = vu_issue.m_pc;
            m_diagnostic.m_last_rejected_vu_lower = vu_issue.m_lower;
            m_diagnostic.m_last_rejected_vu_upper = vu_issue.m_upper;
            m_diagnostic.m_last_rejected_vu_steps = vu_issue.m_steps;
        }
        m_diagnostic.m_pending_words =
            static_cast<uint32_t>(next.m_pending.size());
        m_diagnostic.m_vu_pc = vu_issue.m_pc;
        m_diagnostic.m_vu_lower = vu_issue.m_lower;
        m_diagnostic.m_vu_upper = vu_issue.m_upper;
        return false;
    }
    next.m_pending.erase(next.m_pending.begin(),
                         next.m_pending.begin() +
                             static_cast<std::ptrdiff_t>(index));
    m_machine = std::move(next);
    // A successful packet clears only the latest failure, not the
    // lifetime evidence of a previously rejected VU1/GIF command.
    m_diagnostic.m_reason = e_reject::none;
    m_diagnostic.m_code = 0u;
    m_diagnostic.m_vu_pc = 0u;
    m_diagnostic.m_vu_lower = 0u;
    m_diagnostic.m_vu_upper = 0u;
    m_diagnostic.m_committed_opcodes = m_machine.m_committed_opcodes;
    m_diagnostic.m_pending_words =
        static_cast<uint32_t>(m_machine.m_pending.size());
    m_diagnostic.m_commands = m_machine.m_commands;
    m_diagnostic.m_mpg_uploads = m_machine.m_mpg_uploads;
    m_diagnostic.m_unpack_commands = m_machine.m_unpack_commands;
    m_diagnostic.m_direct_qwc = m_machine.m_direct_qwc;
    return true;
}

uint32_t c_ee_vif1::read_register(uint32_t offset) const
{
    switch (offset)
    {
    case 0x00u: return m_machine.m_stat |
        (m_machine.m_dbf ? 0x80u : 0u);
    case 0x20u: return m_machine.m_err;
    case 0x30u: return m_machine.m_mark;
    case 0x40u: return m_machine.m_cycle;
    case 0x50u: return m_machine.m_mode;
    case 0x60u: return m_machine.m_num;
    case 0x70u: return m_machine.m_mask;
    case 0x80u: return m_machine.m_code;
    case 0x90u: return m_machine.m_itops;
    case 0xa0u: return m_machine.m_base;
    case 0xb0u: return m_machine.m_ofst;
    case 0xc0u: return m_machine.m_tops;
    case 0xd0u: return m_machine.m_itop;
    case 0xe0u: return m_machine.m_top;
    default:
        if (offset >= 0x100u && offset < 0x140u)
        {
            return m_machine.m_row[(offset - 0x100u) >> 4u];
        }
        if (offset >= 0x140u && offset < 0x180u)
        {
            return m_machine.m_col[(offset - 0x140u) >> 4u];
        }
        return 0u;
    }
}

bool c_ee_vif1::write_register(uint32_t offset, uint32_t value)
{
    switch (offset)
    {
    case 0x00u:
        m_machine.m_stat = (m_machine.m_stat & ~0x800000u) |
            (value & 0x800000u);
        return true;
    case 0x10u: // FBRST
        if ((value & 1u) != 0u)
        {
            // VIF FBRST resets command state, not the VU1's resident
            // code/data. ROW/COL also survive this reset in the reference.
            s_machine next{};
            std::memcpy(next.m_row, m_machine.m_row, sizeof(next.m_row));
            std::memcpy(next.m_col, m_machine.m_col, sizeof(next.m_col));
            next.m_vu1_data = m_machine.m_vu1_data;
            next.m_vu1_code = m_machine.m_vu1_code;
            next.m_vu1_state = m_machine.m_vu1_state;
            m_machine = std::move(next);
            m_diagnostic = {};
        }
        if ((value & 8u) != 0u)
        {
            m_machine.m_stat &= ~0x3f00u;
        }
        return true;
    case 0x20u: m_machine.m_err = value; return true;
    case 0x30u: m_machine.m_mark = value; return true;
    default:
        if (offset >= 0x100u && offset < 0x140u &&
            (offset & 0xfu) == 0u)
        {
            m_machine.m_row[(offset - 0x100u) >> 4u] = value;
            return true;
        }
        if (offset >= 0x140u && offset < 0x180u &&
            (offset & 0xfu) == 0u)
        {
            m_machine.m_col[(offset - 0x140u) >> 4u] = value;
            return true;
        }
        return false;
    }
}

bool c_ee_vif1::dma_transfer(void* user, uint32_t channel,
                             const uint8_t* data, uint32_t qwc)
{
    return user != nullptr && channel == 1u &&
        static_cast<c_ee_vif1*>(user)->consume(data, qwc);
}

bool c_ee_vif1::mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr)
    {
        return false;
    }
    c_ee_vif1* vif1 = static_cast<c_ee_vif1*>(user);
    if (width == 4u && (address & 3u) == 0u &&
        address >= k_register_base && address < k_register_base + 0x400u)
    {
        value->m_lo = vif1->read_register(address - k_register_base);
        value->m_hi = 0u;
        return true;
    }

    const uint8_t* source = nullptr;
    uint32_t offset = 0u;
    if (address >= k_vu1_code_base &&
        address < k_vu1_code_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_code_base;
        source = vif1->m_machine.m_vu1_code.data();
    }
    else if (address >= k_vu1_data_base &&
             address < k_vu1_data_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_data_base;
        source = vif1->m_machine.m_vu1_data.data();
    }
    if (source == nullptr ||
        (width != 1u && width != 2u && width != 4u &&
         width != 8u && width != 16u) ||
        width > k_vu1_memory_size - offset)
    {
        return false;
    }
    *value = {};
    for (uint32_t lane = 0u; lane < width; ++lane)
    {
        if (lane < 8u)
        {
            value->m_lo |= static_cast<uint64_t>(source[offset + lane])
                << (lane * 8u);
        }
        else
        {
            value->m_hi |= static_cast<uint64_t>(source[offset + lane])
                << ((lane - 8u) * 8u);
        }
    }
    return true;
}

bool c_ee_vif1::mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr)
    {
        return false;
    }
    c_ee_vif1* vif1 = static_cast<c_ee_vif1*>(user);
    if (address >= k_register_base &&
        address < k_register_base + 0x400u && width == 4u &&
        (address & 3u) == 0u)
    {
        return vif1->write_register(
            address - k_register_base,
            static_cast<uint32_t>(value->m_lo));
    }
    if (address >= k_fifo_base && address < k_fifo_base + 0x1000u &&
        width == 16u && (address & 15u) == 0u)
    {
        uint8_t quad[16]{};
        for (uint32_t lane = 0u; lane < 8u; ++lane)
        {
            quad[lane] = static_cast<uint8_t>(value->m_lo >> (lane * 8u));
            quad[lane + 8u] = static_cast<uint8_t>(
                value->m_hi >> (lane * 8u));
        }
        return vif1->consume(quad, 1u);
    }

    uint8_t* destination = nullptr;
    uint32_t offset = 0u;
    if (address >= k_vu1_code_base &&
        address < k_vu1_code_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_code_base;
        destination = vif1->m_machine.m_vu1_code.data();
    }
    else if (address >= k_vu1_data_base &&
             address < k_vu1_data_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_data_base;
        destination = vif1->m_machine.m_vu1_data.data();
    }
    if (destination == nullptr ||
        (width != 1u && width != 2u && width != 4u &&
         width != 8u && width != 16u) ||
        width > k_vu1_memory_size - offset)
    {
        return false;
    }
    for (uint32_t lane = 0u; lane < width; ++lane)
    {
        const uint64_t source = lane < 8u
            ? value->m_lo : value->m_hi;
        destination[offset + lane] = static_cast<uint8_t>(
            source >> ((lane & 7u) * 8u));
    }
    return true;
}
} // namespace ee
