#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"

#include <utility>
#include <vector>

namespace iop
{
namespace
{
bool normalize_ee_address(uint32_t guest_address, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }

    if (guest_address >= 0x80000000u &&
        guest_address < 0xc0000000u)
    {
        *physical = guest_address & 0x1fffffffu;
        return true;
    }

    const uint32_t region = guest_address >> 28u;
    if (region == 2u || region == 3u)
    {
        *physical = guest_address & 0x0fffffffu;
        return true;
    }

    // The EE scratchpad is a legitimate SIF source. Other MMIO and
    // TLB-mapped regions are not assumed to be ordinary memory.
    if (guest_address < 0x02000000u ||
        (guest_address >= ee::ee_memory::k_scratchpad_base &&
         guest_address - ee::ee_memory::k_scratchpad_base <
             ee::ee_memory::k_scratchpad_size))
    {
        *physical = guest_address;
        return true;
    }
    return false;
}

bool normalize_iop_address(uint32_t guest_address, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }

    const uint32_t region = guest_address & 0xe0000000u;
    if (region != 0u &&
        region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    *physical = guest_address & 0x1fffffffu;
    return true;
}

struct s_transfer
{
    uint32_t m_destination = 0u;
    std::vector<uint32_t> m_words;
    bool m_command = false;
    bool m_rpc_command = false;
    bool m_rpc_payload = false;
    c_sifcmd_bridge::s_command m_command_effect{};
    c_sif_rpc_bridge::s_request m_rpc_effect{};
};
} // namespace

c_sif1_bridge::c_sif1_bridge(
    ee::ee_memory& ee_ram, c_cpu& iop_cpu, ee::c_ee_dmac& dmac,
    c_sifcmd_bridge* sifcmd, c_sif_rpc_bridge* rpc
)
    : m_ee_ram(ee_ram), m_iop_cpu(iop_cpu), m_dmac(dmac),
      m_sifcmd(sifcmd), m_rpc(rpc)
{
}

const char* c_sif1_bridge::failure_name(e_sif1_dma_failure failure)
{
    switch (failure)
    {
    case e_sif1_dma_failure::none: return "none";
    case e_sif1_dma_failure::invalid_list: return "invalid_list";
    case e_sif1_dma_failure::invalid_size_or_attributes:
        return "invalid_size_or_attributes";
    case e_sif1_dma_failure::invalid_source: return "invalid_source";
    case e_sif1_dma_failure::invalid_destination: return "invalid_destination";
    case e_sif1_dma_failure::unsupported_iop_reboot:
        return "unsupported_iop_reboot";
    case e_sif1_dma_failure::command_buffer_overlap:
        return "command_buffer_overlap";
    case e_sif1_dma_failure::duplicate_rpc_payload:
        return "duplicate_rpc_payload";
    case e_sif1_dma_failure::unsupported_rpc_packet:
        return "unsupported_rpc_packet";
    case e_sif1_dma_failure::unsupported_sifcmd_packet:
        return "unsupported_sifcmd_packet";
    }
    return "unknown";
}

bool c_sif1_bridge::set_dma(
    uint32_t list, uint32_t count, uint32_t* dma_id,
    s_sif1_dma_diagnostic* diagnostic
)
{
    if (diagnostic != nullptr)
    {
        *diagnostic = {};
    }
    const auto refuse = [diagnostic](e_sif1_dma_failure reason) -> bool
    {
        if (diagnostic != nullptr)
        {
            diagnostic->m_failure = reason;
        }
        return false;
    };

    uint32_t list_physical = 0u;
    if (dma_id == nullptr ||
        m_next_dma_id == 0u ||
        count == 0u || count > 64u ||
        // SifDmaTransfer is four 32-bit fields; the list needs 4-byte alignment.
        (list & 3u) != 0u ||
        !normalize_ee_address(list, &list_physical) ||
        !m_ee_ram.contains(list_physical, count * 16u))
    {
        return refuse(e_sif1_dma_failure::invalid_list);
    }

    std::vector<s_transfer> transfers;
    transfers.reserve(count);
    uint32_t rpc_payload_source = 0u;
    uint32_t rpc_payload_length = 0u;
    size_t init_reply_count = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t descriptor = list_physical + index * 16u;
        const uint32_t source = m_ee_ram.read32(descriptor);
        const uint32_t destination = m_ee_ram.read32(descriptor + 4u);
        const uint32_t size = m_ee_ram.read32(descriptor + 8u) & 0x7fffffffu;
        const uint32_t attributes = m_ee_ram.read32(descriptor + 12u);

        uint32_t ee_source = 0u;
        uint32_t iop_destination = 0u;
        const bool rpc_payload =
            destination == c_sif_rpc_bridge::k_rpc_data_destination;
        const uint64_t rounded =
            (static_cast<uint64_t>(size) + 15u) & ~uint64_t(15u);

        if (diagnostic != nullptr)
        {
            diagnostic->m_descriptor_index = index;
            diagnostic->m_source = source;
            diagnostic->m_destination = destination;
            diagnostic->m_size = size;
            diagnostic->m_attributes = attributes;
        }
        // The terminal SIFCMD descriptor uses SIF_DMA_ERT |
        // SIF_DMA_INT_O (0x40 | 0x04). It is a real command-buffer
        // transfer, not an implicit command or DMA completion.
        const bool terminal_command =
            attributes == 0x44u &&
            index + 1u == count &&
            destination == c_sifcmd_bridge::k_command_buffer;
        if (size == 0u || size > 65536u ||
            (attributes != 0u && !terminal_command))
        {
            return refuse(e_sif1_dma_failure::invalid_size_or_attributes);
        }
        if ((source & 0x0fu) != 0u ||
            !normalize_ee_address(source, &ee_source) ||
            !m_ee_ram.contains(ee_source, static_cast<size_t>(rounded)))
        {
            return refuse(e_sif1_dma_failure::invalid_source);
        }
        // Record the staged packet header without changing guest RAM.
        if (diagnostic != nullptr)
        {
            // Preserve the five command-header words, when present,
            // even if command validation later rejects the packet.
            if (rounded >= 12u)
            {
                diagnostic->m_packet_size = m_ee_ram.read32(ee_source);
                diagnostic->m_packet_dest = m_ee_ram.read32(ee_source + 4u);
                diagnostic->m_packet_cid = m_ee_ram.read32(ee_source + 8u);
            }
            if (rounded >= 16u)
            {
                diagnostic->m_packet_opt = m_ee_ram.read32(ee_source + 12u);
            }
            if (rounded >= 20u)
            {
                diagnostic->m_packet_arg = m_ee_ram.read32(ee_source + 16u);
            }
        }
        if (destination == 0u)
        {
            // A reset request is not an ordinary IOP RAM write.
            return refuse(e_sif1_dma_failure::unsupported_iop_reboot);
        }
        if ((destination & 3u) != 0u ||
            (!rpc_payload &&
             !normalize_iop_address(destination, &iop_destination)) ||
            (!rpc_payload &&
             static_cast<uint64_t>(iop_destination) + rounded >
                 c_cpu::k_ram_size))
        {
            return refuse(e_sif1_dma_failure::invalid_destination);
        }
        if (!rpc_payload &&
            static_cast<uint64_t>(iop_destination) <
                c_sifcmd_bridge::k_command_buffer + 0x400u &&
            static_cast<uint64_t>(iop_destination) + rounded >
                c_sifcmd_bridge::k_command_buffer &&
            iop_destination != c_sifcmd_bridge::k_command_buffer)
        {
            return refuse(e_sif1_dma_failure::command_buffer_overlap);
        }

        s_transfer transfer{};
        transfer.m_destination = iop_destination;
        transfer.m_words.reserve(static_cast<size_t>(rounded / 4u));
        for (uint32_t offset = 0u;
             offset < rounded; offset += 4u)
        {
            transfer.m_words.push_back(
                m_ee_ram.read32(ee_source + offset)
            );
        }
        if (rpc_payload)
        {
            if (m_rpc == nullptr ||
                rpc_payload_length != 0u)
            {
                return refuse(e_sif1_dma_failure::duplicate_rpc_payload);
            }
            transfer.m_rpc_payload = true;
            rpc_payload_source = ee_source;
            rpc_payload_length = size;
        }
        else if (iop_destination == c_sifcmd_bridge::k_command_buffer)
        {
            // The guest command receiver accepts a small set of
            // explicit SIFCMD and LOADFILE RPC packet types.
            const uint32_t cid = transfer.m_words.size() >= 3u
                ? transfer.m_words[2] : 0u;
            if (diagnostic != nullptr && transfer.m_words.size() >= 9u &&
                cid == c_sif_rpc_bridge::k_bind_cid)
            {
                diagnostic->m_rpc_server = transfer.m_words[8];
                diagnostic->m_rpc_client = transfer.m_words[7];
            }
            if (diagnostic != nullptr && transfer.m_words.size() >= 14u &&
                cid == c_sif_rpc_bridge::k_call_cid)
            {
                diagnostic->m_rpc_server =
                    transfer.m_words[13] ^ c_sif_rpc_bridge::k_server_xor;
                diagnostic->m_rpc_client = transfer.m_words[7];
            }
            if (cid == c_sif_rpc_bridge::k_bind_cid ||
                cid == c_sif_rpc_bridge::k_call_cid)
            {
                if (diagnostic != nullptr)
                {
                    // Snapshot the complete guest RPC header before validation.
                    // This has no side effects, including on refusal.
                    const size_t word_count =
                        transfer.m_words.size() < 16u
                            ? transfer.m_words.size() : 16u;
                    diagnostic->m_rpc_packet_word_count =
                        static_cast<uint32_t>(word_count);
                    for (size_t word = 0u; word < word_count; ++word)
                    {
                        diagnostic->m_rpc_packet_words[word] =
                            transfer.m_words[word];
                    }
                    diagnostic->m_rpc_payload_source = rpc_payload_source;
                    diagnostic->m_rpc_payload_length = rpc_payload_length;
                }
                if (m_rpc == nullptr ||
                    !m_rpc->prepare_command(
                        transfer.m_words, size,
                        rpc_payload_source, rpc_payload_length,
                        &transfer.m_rpc_effect,
                        diagnostic == nullptr ? nullptr :
                            &diagnostic->m_rpc_rejection))
                {
                    return refuse(e_sif1_dma_failure::unsupported_rpc_packet);
                }
                transfer.m_rpc_command = true;
            }
            else
            {
                if (m_sifcmd == nullptr ||
                    !m_sifcmd->prepare_command(
                        transfer.m_words, size, &transfer.m_command_effect))
                {
                    return refuse(e_sif1_dma_failure::unsupported_sifcmd_packet);
                }
                if (transfer.m_command_effect.m_request_sreg_reply)
                {
                    ++init_reply_count;
                    if (m_rpc == nullptr ||
                        !m_rpc->can_queue_replies(init_reply_count))
                    {
                        // A valid initialization request cannot be
                        // acknowledged if no HLE SIF0 reply path exists.
                        return refuse(e_sif1_dma_failure::unsupported_sifcmd_packet);
                    }
                }
                transfer.m_command = true;
            }
        }
        transfers.push_back(std::move(transfer));
    }

    // No writes occur until all sources and IOP destinations are valid.
    // The R3000 RAM writes below cannot fail after those checks.
    for (const s_transfer& transfer : transfers)
    {
        if (!transfer.m_rpc_payload)
        {
            for (size_t index = 0u; index < transfer.m_words.size(); ++index)
            {
                m_iop_cpu.write32(
                    transfer.m_destination +
                        static_cast<uint32_t>(index * 4u),
                    transfer.m_words[index]
                );
            }
        }
    }

    // Apply the guest packets in descriptor order. LOADFILE invokes
    // the real disc loader only after *every* descriptor passed
    // validation. Replies go through armed EE SIF0, not SMFLAG.
    for (const s_transfer& transfer : transfers)
    {
        if (transfer.m_command)
        {
            m_sifcmd->commit_command(transfer.m_command_effect);
            if (transfer.m_command_effect.m_request_sreg_reply)
            {
                m_rpc->queue_init_sreg_reply();
            }
        }
        else if (transfer.m_rpc_command)
        {
            m_rpc->commit_command(transfer.m_rpc_effect);
        }
    }
    if (m_rpc != nullptr)
    {
        (void)m_rpc->pump();
    }

    m_dmac.raise(6u);
    *dma_id = m_next_dma_id++;
    return true;
}

bool c_sif1_bridge::dma_stat(
    uint32_t dma_id, int32_t* result
) const
{
    if (result == nullptr || dma_id == 0u ||
        dma_id >= m_next_dma_id)
    {
        return false;
    }

    // This transport completes synchronously. No queued or in-flight
    // transfer is reported without a real EE/IOP cycle scheduler.
    *result = -1;
    return true;
}
} // namespace iop
