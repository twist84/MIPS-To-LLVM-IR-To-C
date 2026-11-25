// Adapted from ps2-web-recomp:SDBZ by Samir Nuri (2026), MIT license.
// Upstream license: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#include "ee_vif1.h"
#include "ee_runtime_profile.h"
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
// PS2 words are little-endian. memcpy avoids unaligned typed accesses and
// is valid for arbitrarily aligned DMA sources and trivially copyable words.
bool host_little_endian()
{
    const uint16_t one = 1u;
    return *reinterpret_cast<const unsigned char*>(&one) == 1u;
}

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
    cancel_deferred();
    const bool was_masked = m_machine.m_path3_masked;
    m_machine = {};
    if (was_masked && m_mask_callback != nullptr)
    {
        m_mask_callback(m_mask_user, false);
    }
    m_pending_scratch.clear();
    m_path1_scratch.clear();
    m_path2_scratch.clear();
    m_diagnostic = {};
}

const uint8_t* c_ee_vif1::vu1_data() const
{
    return m_machine.m_vu1->m_data.data();
}

const uint8_t* c_ee_vif1::vu1_code() const
{
    return m_machine.m_vu1->m_code.data();
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
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::vif1_unpack);
    const uint32_t vn = (code >> 26u) & 3u;
    const uint32_t vl = (code >> 24u) & 3u;
    const bool masked = (code & 0x10000000u) != 0u;
    const bool unsigned_value = (code & 0x4000u) != 0u;
    uint32_t num = (code >> 16u) & 0xffu;
    if (num == 0u)
    {
        num = 256u;
    }

    const uint32_t address_mask = machine.m_vif0 ? 0xffu : 0x3ffu;
    uint32_t address = code & address_mask;
    if (!machine.m_vif0 && (code & 0x8000u) != 0u)
    {
        address += machine.m_tops;
    }
    const uint32_t cl = machine.m_cycle & 0xffu;
    const uint32_t wl = (machine.m_cycle >> 8u) & 0xffu;
    // Packed V4-5 ignores STMOD. Other formats use the current mode.
    const uint32_t mode = vn == 3u && vl == 3u ? 0u : machine.m_mode;
    // V4-32, unmasked, no row arithmetic and CL == WL consumes one
    // complete source vector per destination qword with no skips or fills.
    // Split at the physical 16 KiB wrap, including NUM=0 (256 vectors).
    // Keep all other cycling, masks and formats on the reference path.
    if (vn == 3u && vl == 0u && !masked &&
        mode == 0u && cl == wl &&
        host_little_endian())
    {
        const uint32_t start = address & address_mask;
        const uint32_t first = std::min(num, address_mask + 1u - start);
        std::memcpy(machine.m_vu1->m_data.data() + start * 16u,
                    words, first * 16u);
        if (first < num)
        {
            std::memcpy(machine.m_vu1->m_data.data(), words + first * 4u,
                        (num - first) * 16u);
        }
        machine.m_num = 0u;
        ++machine.m_unpack_commands;
        return;
    }
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

        uint8_t* destination = machine.m_vu1->m_data.data() +
            (address & address_mask) * 16u;
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
            else
            {
                if (mode == 1u || mode == 2u)
                {
                    output += machine.m_row[lane];
                }
                if (mode == 2u || mode == 3u)
                {
                    // Difference mode accumulates ROW; mode 3 stores raw
                    // incoming data in ROW. Masked ROW/COL/protected lanes
                    // do not update it.
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
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::vif1_command);
    const uint32_t op = (code >> 24u) & 0x7fu;
    const uint32_t imm = code & 0xffffu;
    const uint32_t num = (code >> 16u) & 0xffu;
    bool accepted = true;
    machine.m_code = code;

    // VIF0 has no VU1 double-buffer or GIF PATH2/PATH3 control. Until
    // its launch/IRQ scheduling is implemented, do not execute those
    // commands using the unrelated VU1 micro engine.
    if (machine.m_vif0 &&
        (op == 0x02u || op == 0x03u || op == 0x06u ||
         op == 0x11u || op == 0x13u || op == 0x14u ||
         op == 0x15u || op == 0x17u || op == 0x50u || op == 0x51u))
    {
        *reject = e_reject::unsupported_command;
        return false;
    }

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
        case 0x04u: machine.m_itops = imm & (machine.m_vif0 ? 0xffu : 0x3ffu); break; // ITOP
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
            c_runtime_profile_scope profile_scope(e_runtime_profile_id::vif1_mpg, count);
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t offset = ((imm + i) * 8u) &
                    (machine.m_vif0 ? 0xfffu : 0x3fffu);
                std::memcpy(machine.m_vu1->m_code.data() + offset,
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
            if (host_little_endian())
            {
                // Append initialized bytes directly, avoiding resize's
                // zero fill followed by an overwrite of the entire span.
                const auto* begin = reinterpret_cast<const uint8_t*>(words);
                path2_bytes.insert(path2_bytes.end(), begin,
                    begin + static_cast<size_t>(qwc) * 16u);
            }
            else
            {
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
            }
            machine.m_direct_qwc += qwc;
            break;
        }
        case 0x06u: // MSKPATH3, VIF1 only; published after acceptance.
            machine.m_path3_masked = (imm & 0x8000u) != 0u;
            break;
        case 0x14u: // MSCAL
        case 0x15u: // MSCALF (PATH3 idle checked before execution)
        case 0x17u: // MSCNT
        {
            if (machine.m_vu_debug_stopped ||
                (op == 0x17u && !machine.m_vu1->m_state.m_initialized))
            {
                *reject = e_reject::unsupported_vu_execution;
                accepted = false;
                break;
            }
            const uint32_t start = op == 0x17u
                ? machine.m_vu1->m_state.m_tpc : (imm & 0x7ffu) * 8u;
            accepted = c_ee_vu1::run(
                machine.m_vu1->m_state, machine.m_vu1->m_code,
                machine.m_vu1->m_data, start, machine.m_tops,
                machine.m_itops, path1_bytes, vu_issue);
            if (!accepted)
            {
                *reject = e_reject::unsupported_vu_execution;
            }
            else
            {
                complete_vu_start(machine);
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

void c_ee_vif1::complete_vu_start(s_machine& machine)
{
    // TOP describes the completed start; TOPS selects the next buffer.
    machine.m_top = machine.m_tops;
    machine.m_itop = machine.m_itops;
    machine.m_dbf = !machine.m_dbf;
    machine.m_tops = machine.m_base +
        (machine.m_dbf ? machine.m_ofst : 0u);
}

c_ee_vif1::s_transaction::s_transaction(c_ee_vif1& owner)
    : m_owner(owner)
{
    acquire();
}

c_ee_vif1::s_transaction::~s_transaction()
{
    release();
}

void c_ee_vif1::s_transaction::acquire()
{
    // prepare() overwrites the machine snapshot before processing any
    // command. Keep its resident allocation instead of zeroing 32 KiB.
    m_next.m_pending.clear();
    m_next.m_pending.swap(m_owner.m_pending_scratch);
    m_path1.swap(m_owner.m_path1_scratch);
    m_path2.swap(m_owner.m_path2_scratch);
    m_path1.clear();
    m_path2.clear();
    m_ranges.clear();
    m_index = 0u;
    m_before1 = 0u;
    m_before2 = 0u;
    m_waiting_words = 0u;
    m_issue = {};
    m_rejection = e_reject::none;
    m_rejected_code = 0u;
    m_committed = false;
    m_active = true;
}

void c_ee_vif1::s_transaction::release()
{
    if (!m_active)
    {
        return;
    }
    m_active = false;
    if (!m_committed)
    {
        m_next.m_pending.clear();
        m_next.m_pending.swap(m_owner.m_pending_scratch);
    }
    m_path1.clear();
    m_path2.clear();
    m_path1.swap(m_owner.m_path1_scratch);
    m_path2.swap(m_owner.m_path2_scratch);
    constexpr size_t limit =
        static_cast<size_t>(k_max_graphics_batch_qwc) * 16u;
    if (m_owner.m_path1_scratch.capacity() > limit)
    {
        std::vector<uint8_t>().swap(m_owner.m_path1_scratch);
    }
    if (m_owner.m_path2_scratch.capacity() > limit)
    {
        std::vector<uint8_t>().swap(m_owner.m_path2_scratch);
    }
    if (m_owner.m_pending_scratch.capacity() >
        limit / sizeof(uint32_t) + 0x40000u + 0x400u)
    {
        std::vector<uint32_t>().swap(m_owner.m_pending_scratch);
    }
    m_ranges.clear();
    if (m_ranges.capacity() > limit / sizeof(s_path_range))
    {
        std::vector<s_path_range>().swap(m_ranges);
    }
}

void c_ee_vif1::recycle_transaction(std::unique_ptr<s_transaction>& tx)
{
    tx->release();
    m_spare_transaction = std::move(tx);
}

void c_ee_vif1::reject_packet(e_reject reason, uint32_t code)
{
    ++m_diagnostic.m_rejected_batches;
    if (m_diagnostic.m_first_rejection == e_reject::none)
    {
        m_diagnostic.m_first_rejection = reason;
        m_diagnostic.m_first_rejected_code = code;
    }
    m_diagnostic.m_reason = reason;
    m_diagnostic.m_code = code;
}

bool c_ee_vif1::prepare(s_transaction& tx, const uint8_t* data, uint32_t qwc)
{
    // Bound residual command words independently of aggregate DMA size.
    constexpr size_t k_max_pending_words =
        static_cast<size_t>(k_max_graphics_batch_qwc) * 4u + 0x40000u + 0x400u;
    if (qwc > k_max_graphics_batch_qwc || (qwc != 0u && data == nullptr))
    {
        reject_packet(e_reject::invalid_packet, 0u);
        return false;
    }
    auto& next = tx.m_next;
    next = m_machine;
    next.m_vif0 = m_vu0 != nullptr;
    if (next.m_vif0)
    {
        // The EE memory aperture and COP2 operate on this same live VU0.
        // Use the transaction's private buffers only as staging storage.
        std::memcpy(next.m_vu1->m_data.data(), m_vu0->m_data_memory,
                    sizeof(m_vu0->m_data_memory));
        std::memcpy(next.m_vu1->m_code.data(), m_vu0->m_micro_memory,
                    sizeof(m_vu0->m_micro_memory));
    }
    if (next.m_pending.size() + static_cast<size_t>(qwc) * 4u >
        k_max_pending_words)
    {
        reject_packet(e_reject::pending_limit, 0u);
        return false;
    }
    const size_t previous = next.m_pending.size();
    next.m_pending.resize(previous + static_cast<size_t>(qwc) * 4u);
    if (qwc != 0u && host_little_endian())
    {
        std::memcpy(next.m_pending.data() + previous, data,
                    static_cast<size_t>(qwc) * 16u);
    }
    else
    {
        for (size_t i = 0u; i < static_cast<size_t>(qwc) * 4u; ++i)
        {
            const size_t offset = i * 4u;
            next.m_pending[previous + i] =
                static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                (static_cast<uint32_t>(data[offset + 2u]) << 16u) |
                (static_cast<uint32_t>(data[offset + 3u]) << 24u);
        }
    }

    return true;
}

bool c_ee_vif1::record_command(s_transaction& tx, uint32_t code, uint32_t words)
{
    const auto record_range = [&tx](uint32_t path, size_t before, size_t after)
    {
        if (before != after)
        {
            if (!tx.m_ranges.empty() && tx.m_ranges.back().m_path == path)
            {
                tx.m_ranges.back().m_size += after - before;
            }
            else
            {
                tx.m_ranges.push_back({path, before, after - before});
            }
        }
    };
    record_range(1u, tx.m_before1, tx.m_path1.size());
    record_range(2u, tx.m_before2, tx.m_path2.size());
    if (tx.m_path1.size() + tx.m_path2.size() >
        static_cast<size_t>(k_max_graphics_batch_qwc) * 16u)
    {
        tx.m_rejection = e_reject::pending_limit;
        tx.m_rejected_code = code;
        return false;
    }
    tx.m_index += 1u + words;
    return true;
}

// Returns false only after dispatching a detached VU1 job. No GIF
// parser or committed VIF machine is touched until publish().
bool c_ee_vif1::advance(s_transaction& tx, c_ee_vu1_worker* worker)
{
    auto& next = tx.m_next;
    while (tx.m_rejection == e_reject::none &&
           tx.m_index < next.m_pending.size())
    {
        const uint32_t code = next.m_pending[tx.m_index];
        const uint32_t words = command_data_words(next, code);
        if (static_cast<size_t>(words) >
            next.m_pending.size() - tx.m_index - 1u)
        {
            break;
        }
        const uint32_t opcode = (code >> 24u) & 0x7fu;
        if (opcode == 0x06u && m_mask_callback == nullptr)
        {
            tx.m_rejection = e_reject::unsupported_command;
            tx.m_rejected_code = code;
            break;
        }
        const bool vu_start =
            opcode == 0x14u || opcode == 0x15u || opcode == 0x17u;
        if (vu_start)
        {
            ++m_diagnostic.m_vu_start_attempts;
            m_diagnostic.m_last_vu_start_code = code;
        }
        if (opcode == 0x15u && m_path3 != nullptr && m_path3->state().m_active)
        {
            tx.m_rejection = e_reject::mscalf_path3_busy;
            tx.m_rejected_code = code;
            tx.m_issue = {};
            break;
        }
        tx.m_before1 = tx.m_path1.size();
        tx.m_before2 = tx.m_path2.size();

        // IRQ and uninitialized MSCNT retain the synchronous rejection
        // path. Following MPG, UNPACK, FLUSH and MSCNT cannot run until
        // the preceding worker result has been collected.
        if (worker != nullptr && vu_start && !next.m_vu_debug_stopped &&
            ((code & 0x80000000u) == 0u || (next.m_err & 1u) != 0u) &&
            (opcode != 0x17u || next.m_vu1->m_state.m_initialized))
        {
            auto job = std::move(m_spare_job);
            if (job == nullptr)
            {
                job = std::make_unique<s_ee_vu1_job>();
            }
            job->m_address = opcode == 0x17u ?
                next.m_vu1->m_state.m_tpc : (code & 0x7ffu) * 8u;
            job->m_top = next.m_tops;
            job->m_itop = next.m_itops;
            job->m_resident.swap(next.m_vu1);
            job->m_path1.swap(tx.m_path1);
            next.m_code = code;
            uint64_t handoff = 0u;
            // The worker cannot start before its submission timestamp.
            // Reuse that timestamp as the EE interval lower bound.
            if (worker->submit(job, m_native_executing ? &handoff : nullptr))
            {
                ++m_worker_profile.m_submitted;
                m_native_interval_start = handoff;
                tx.m_waiting_words = words;
                return false;
            }
            next.m_vu1.swap(job->m_resident);
            job->m_path1.swap(tx.m_path1);
            m_spare_job = std::move(job);
            tx.m_rejection = e_reject::unsupported_vu_execution;
            tx.m_rejected_code = code;
            tx.m_issue = {};
            break;
        }

        if (!execute(next, code, next.m_pending.data() + tx.m_index + 1u,
                     tx.m_path2, tx.m_path1, &tx.m_issue, &tx.m_rejection))
        {
            tx.m_rejected_code = code;
            break;
        }
        if (!record_command(tx, code, words))
        {
            break;
        }
    }
    return true;
}

bool c_ee_vif1::publish(s_transaction& tx)
{
    auto& next = tx.m_next;
    auto& path1_bytes = tx.m_path1;
    auto& path2_bytes = tx.m_path2;
    auto& ranges = tx.m_ranges;
    auto& rejection = tx.m_rejection;
    const auto rejected_code = tx.m_rejected_code;
    const auto& vu_issue = tx.m_issue;
    const auto index = tx.m_index;
    if (rejection == e_reject::none &&
        !path1_bytes.empty() && !path2_bytes.empty())
    {
        if (m_path1 == nullptr)
        {
            rejection = e_reject::missing_path1;
        }
        else if (m_path2 == nullptr)
        {
            rejection = e_reject::missing_path2;
        }
        else
        {
            std::vector<s_ee_gif_segment> segments;
            segments.reserve(ranges.size());
            for (const auto& range : ranges)
            {
                const auto& bytes = range.m_path == 1u ? path1_bytes : path2_bytes;
                segments.push_back({range.m_path, bytes.data() + range.m_offset,
                                    static_cast<uint32_t>(range.m_size / 16u)});
            }
            if (!c_ee_gif_path3::consume_ordered(*m_path1, *m_path2,
                    segments.data(), segments.size()))
            {
                rejection = e_reject::mixed_gif_paths;
            }
            else
            {
                // Already committed both parsers and GS atomically.
                path1_bytes.clear();
                path2_bytes.clear();
            }
        }
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
            m_diagnostic.m_last_rejected_vu_issue = vu_issue;
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
    // Preserve the old pending allocation as the next spare. The
    // staged vector becomes committed, including an incomplete command.
    m_machine.m_pending.swap(m_pending_scratch);
    const bool mask_changed = m_machine.m_path3_masked != next.m_path3_masked;
    m_machine = std::move(next);
    if (m_vu0 != nullptr)
    {
        std::memcpy(m_vu0->m_data_memory, m_machine.m_vu1->m_data.data(),
                    sizeof(m_vu0->m_data_memory));
        std::memcpy(m_vu0->m_micro_memory, m_machine.m_vu1->m_code.data(),
                    sizeof(m_vu0->m_micro_memory));
    }
    tx.m_committed = true;
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
    if (mask_changed && m_mask_callback != nullptr)
    {
        m_mask_callback(m_mask_user, m_machine.m_path3_masked);
    }
    return true;
}

bool c_ee_vif1::control_vu_break_reset(uint32_t value)
{
    if (deferred_busy())
    {
        return false;
    }
    if ((value & 0x200u) != 0u)
    {
        m_machine.m_vu_debug_stopped = false;
        m_machine.m_stat &= ~4u;
    }
    else if ((value & 0x100u) != 0u)
    {
        m_machine.m_vu_debug_stopped = true;
    }
    return true;
}

bool c_ee_vif1::run_cmsar1(uint32_t address)
{
    if (m_vu0 != nullptr || deferred_busy())
    {
        return false;
    }

    // CMSAR1 starts VU1 directly with the current TOP/ITOP, not TOPS/ITOPS.
    // Do not toggle DBF, consume pending VIF words or count an MSCAL.
    s_transaction tx(*this);
    tx.m_next = m_machine;
    auto& next = tx.m_next;
    next.m_vu_debug_stopped = false;
    if (!c_ee_vu1::run(next.m_vu1->m_state, next.m_vu1->m_code,
            next.m_vu1->m_data, (address & 0x7ffu) * 8u,
            next.m_top, next.m_itop, tx.m_path1, &tx.m_issue))
    {
        tx.m_rejection = e_reject::unsupported_vu_execution;
    }
    return publish(tx);
}

bool c_ee_vif1::consume(const uint8_t* data, uint32_t qwc)
{
    // A direct FIFO/MMIO transfer is a synchronous observation point,
    // but in worker mode it must retain the same owned AOT backend.
    if (m_worker != nullptr)
    {
        const auto accepted = begin_deferred(data, qwc);
        return accepted == e_deferred_result::completed ||
            (accepted == e_deferred_result::pending &&
             wait_deferred() == e_deferred_result::completed);
    }
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::vif1_consume, qwc);
    if (m_deferred != nullptr)
    {
        return false;
    }
    s_transaction tx(*this);
    return prepare(tx, data, qwc) && advance(tx, nullptr) && publish(tx);
}

bool c_ee_vif1::enable_deferred(std::unique_ptr<c_ee_vu1_aot> backend,
    bool detailed_profile)
{
    if (m_vu0 != nullptr || m_worker != nullptr)
    {
        return false;
    }
    m_worker = std::make_unique<c_ee_vu1_worker>(std::move(backend), detailed_profile);
    return true;
}

bool c_ee_vif1::deferred_busy() const
{
    return m_deferred != nullptr;
}

c_ee_vif1::e_deferred_result c_ee_vif1::begin_deferred(
    const uint8_t* data, uint32_t qwc)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::vif1_consume, qwc);
    if (m_deferred != nullptr)
    {
        return e_deferred_result::busy;
    }
    if (m_worker == nullptr)
    {
        return e_deferred_result::rejected;
    }
    auto tx = std::move(m_spare_transaction);
    if (tx == nullptr)
    {
        tx = std::make_unique<s_transaction>(*this);
    }
    else
    {
        tx->acquire();
    }
    if (!prepare(*tx, data, qwc))
    {
        recycle_transaction(tx);
        return e_deferred_result::rejected;
    }
    if (advance(*tx, m_worker.get()))
    {
        const bool accepted = publish(*tx);
        recycle_transaction(tx);
        return accepted ? e_deferred_result::completed :
            e_deferred_result::rejected;
    }
    m_deferred = std::move(tx);
    return e_deferred_result::pending;
}

c_ee_vif1::e_deferred_result c_ee_vif1::resume_deferred(bool wait)
{
    if (m_deferred == nullptr)
    {
        return e_deferred_result::idle;
    }
    ee::c_runtime_profile_scope profile_scope(
        ee::e_runtime_profile_id::vif1_deferred_resume);
    do
    {
        auto job = take_worker_job(wait);
        if (job == nullptr)
        {
            return e_deferred_result::pending;
        }
        record_worker_job(*job, false);
        auto& tx = *m_deferred;
        auto& next = tx.m_next;
        const uint32_t code = next.m_pending[tx.m_index];
        const uint32_t opcode = (code >> 24u) & 0x7fu;
        next.m_vu1.swap(job->m_resident);
        tx.m_path1.swap(job->m_path1);
        tx.m_issue = job->m_issue;
        if (!job->m_accepted || job->m_host_failed)
        {
            tx.m_rejection = e_reject::unsupported_vu_execution;
            tx.m_rejected_code = code;
        }
        else
        {
            complete_vu_start(next);
            ++next.m_commands;
            ++next.m_committed_opcodes[opcode];
            (void)record_command(tx, code, tx.m_waiting_words);
        }
        m_spare_job = std::move(job);
        if (advance(tx, m_worker.get()))
        {
            const bool accepted = publish(tx);
            recycle_transaction(m_deferred);
            return accepted ? e_deferred_result::completed :
                e_deferred_result::rejected;
        }
    }
    while (wait);
    return e_deferred_result::pending;
}

c_ee_vif1::e_deferred_result c_ee_vif1::wait_deferred()
{
    return resume_deferred(true);
}

void c_ee_vif1::cancel_deferred()
{
    if (m_deferred != nullptr)
    {
        auto job = take_worker_job(true);
        if (job != nullptr)
        {
            record_worker_job(*job, true);
            job->m_path1.swap(m_deferred->m_path1);
            m_spare_job = std::move(job);
        }
        recycle_transaction(m_deferred);
    }
}

std::unique_ptr<s_ee_vu1_job> c_ee_vif1::take_worker_job(bool wait)
{
    if (wait)
    {
        const uint64_t start = c_ee_vu1_worker::clock_ns();
        // Explicit joins are not EE/VU overlap, including joins in MMIO.
        record_native_overlap(start);
        ee::c_runtime_profile_scope scope(ee::e_runtime_profile_id::vu1_worker_wait);
        auto result = m_worker->wait_take();
        ++m_worker_profile.m_wait_calls;
        m_worker_profile.m_wait_ns += c_ee_vu1_worker::clock_ns() - start;
        return result;
    }
    auto result = m_worker->try_take();
    if (result != nullptr && m_native_interval_start != 0u)
    {
        finish_native_overlap(result->m_started_ns, result->m_finished_ns);
    }
    return result;
}

void c_ee_vif1::record_worker_job(const s_ee_vu1_job& job, bool discarded)
{
    auto& p = m_worker_profile;
    ++p.m_collected;
    p.m_rejected += job.m_accepted ? 0u : 1u;
    p.m_discarded += discarded ? 1u : 0u;
    p.m_host_failures += job.m_host_failed ? 1u : 0u;
    p.m_pairs += job.m_issue.m_steps;
    p.m_execution_ns += job.m_execution_ns;
    p.m_queue_ns += job.m_started_ns - job.m_submitted_ns;
    p.m_compiled_ns += job.m_compiled_ns;
    p.m_compiled_pairs += job.m_compiled_pairs;
    p.m_xgkick_ns += job.m_xgkick_ns;
}

void c_ee_vif1::write_aot_json(FILE* output) const
{
    if (m_worker != nullptr)
    {
        m_worker->write_aot_json(output);
    }
    else
    {
        write_vu1_aot_json(output);
    }
}

void c_ee_vif1::write_worker_json(FILE* output) const
{
    const auto& p = m_worker_profile;
    std::fprintf(output,
        "{\"enabled\":%s,\"pending\":%s,"
        "\"submitted_jobs\":%llu,\"collected_jobs\":%llu,"
        "\"rejected_jobs\":%llu,\"discarded_jobs\":%llu,\"host_failures\":%llu,"
        "\"attempted_pairs\":%llu,\"worker_execution_ns\":%llu,"
        "\"worker_queue_ns\":%llu,\"compiled_exclusive_ns\":%llu,"
        "\"compiled_attempted_pairs\":%llu,\"xgkick_exclusive_ns\":%llu,"
        "\"controller_wait_ns\":%llu,\"controller_wait_calls\":%llu,"
        "\"controller_poll_calls\":%llu,\"controller_empty_polls\":%llu,"
        "\"ee_native_overlap_wall_ns\":%llu,\"ee_native_overlap_intervals\":%llu,"
        "\"ee_native_pending_boundaries\":%llu,"
        "\"ee_native_completed_at_entry\":%llu,"
        "\"ee_native_completed_at_exit\":%llu}",
        m_worker != nullptr ? "true" : "false",
        m_deferred != nullptr ? "true" : "false",
        static_cast<unsigned long long>(p.m_submitted),
        static_cast<unsigned long long>(p.m_collected),
        static_cast<unsigned long long>(p.m_rejected),
        static_cast<unsigned long long>(p.m_discarded),
        static_cast<unsigned long long>(p.m_host_failures),
        static_cast<unsigned long long>(p.m_pairs),
        static_cast<unsigned long long>(p.m_execution_ns),
        static_cast<unsigned long long>(p.m_queue_ns),
        static_cast<unsigned long long>(p.m_compiled_ns),
        static_cast<unsigned long long>(p.m_compiled_pairs),
        static_cast<unsigned long long>(p.m_xgkick_ns),
        static_cast<unsigned long long>(p.m_wait_ns),
        static_cast<unsigned long long>(p.m_wait_calls),
        static_cast<unsigned long long>(p.m_poll_calls),
        static_cast<unsigned long long>(p.m_empty_polls),
        static_cast<unsigned long long>(p.m_native_overlap_ns),
        static_cast<unsigned long long>(p.m_native_overlap_intervals),
        static_cast<unsigned long long>(p.m_native_pending_boundaries),
        static_cast<unsigned long long>(p.m_native_completed_at_entry),
        static_cast<unsigned long long>(p.m_native_completed_at_exit));
}

uint32_t c_ee_vif1::read_register(uint32_t offset) const
{
    switch (offset)
    {
    case 0x00u: return m_machine.m_stat |
        (m_machine.m_dbf ? 0x80u : 0u) |
        (deferred_busy() ? 0x4u : 0u); // VEW: outstanding VU execution
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
    if (m_deferred != nullptr)
    {
        if (offset == 0x10u && (value & 1u) != 0u)
        {
            cancel_deferred();
        }
        else
        {
            return false;
        }
    }
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
            next.m_vu1 = m_machine.m_vu1;
            next.m_path3_masked = m_machine.m_path3_masked;
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
    if (user == nullptr)
    {
        return false;
    }
    auto* vif = static_cast<c_ee_vif1*>(user);
    return channel == (vif->m_vu0 != nullptr ? 0u : 1u) &&
        vif->consume(data, qwc);
}

bool c_ee_vif1::mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr)
    {
        return false;
    }
    c_ee_vif1* vif1 = static_cast<c_ee_vif1*>(user);
    const uint32_t register_base = vif1->m_vu0 != nullptr ?
        k_vif0_register_base : k_register_base;
    if (width == 4u && (address & 3u) == 0u &&
        address >= register_base && address < register_base + 0x400u)
    {
        value->m_lo = vif1->read_register(address - register_base);
        value->m_hi = 0u;
        return true;
    }

    // VU0 memory is already routed directly by ee_memory.
    if (vif1->m_vu0 != nullptr)
    {
        return false;
    }
    const uint8_t* source = nullptr;
    uint32_t offset = 0u;
    if (address >= k_vu1_code_base &&
        address < k_vu1_code_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_code_base;
        source = vif1->m_machine.m_vu1->m_code.data();
    }
    else if (address >= k_vu1_data_base &&
             address < k_vu1_data_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_data_base;
        source = vif1->m_machine.m_vu1->m_data.data();
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
    const uint32_t register_base = vif1->m_vu0 != nullptr ?
        k_vif0_register_base : k_register_base;
    const uint32_t fifo_base = vif1->m_vu0 != nullptr ?
        k_vif0_fifo_base : k_fifo_base;
    if (address >= register_base &&
        address < register_base + 0x400u && width == 4u &&
        (address & 3u) == 0u)
    {
        return vif1->write_register(
            address - register_base,
            static_cast<uint32_t>(value->m_lo));
    }
    if (address >= fifo_base && address < fifo_base + 0x1000u &&
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

    if (vif1->m_vu0 != nullptr || vif1->deferred_busy())
    {
        return false;
    }
    uint8_t* destination = nullptr;
    uint32_t offset = 0u;
    if (address >= k_vu1_code_base &&
        address < k_vu1_code_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_code_base;
        destination = vif1->m_machine.m_vu1->m_code.data();
    }
    else if (address >= k_vu1_data_base &&
             address < k_vu1_data_base + k_vu1_memory_size)
    {
        offset = address - k_vu1_data_base;
        destination = vif1->m_machine.m_vu1->m_data.data();
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
