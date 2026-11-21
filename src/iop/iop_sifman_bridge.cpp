#include "iop_sifman_bridge.h"

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace iop
{
namespace
{
bool create_stub(
    c_cpu& cpu, uint32_t pc, uint32_t service
)
{
    // MIPS I: lui v0,upper; ori v0,v0,lower; syscall; jr ra; nop
    // The HLE operation is only performed if an IOP thread actually
    // reaches this syscall after calling its resolved IRX import.
    const uint32_t code[] =
    {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0x00000000u
    };
    bool valid = true;
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        valid = valid && cpu.write32(pc + i * 4u, code[i]);
    }
    return valid;
}
} // namespace

c_sifman_bridge::c_sifman_bridge(
    c_kernel& kernel, ee::c_ee_sif& sif,
    ee::c_ee_dmac* dmac, ee::ee_memory* ee_ram
)
    : m_kernel(kernel), m_sif(sif),
      m_dmac(dmac), m_ee_ram(ee_ram)
{
}

bool c_sifman_bridge::install()
{
    // Keep all existing guest stub addresses stable; this avoids
    // collisions with SIFCMD and CDVDMAN in reserved low IOP RAM.
    constexpr std::array<uint16_t, 14u> ordinals = {
        4u, 5u, 6u, 7u, 8u, 21u, 22u,
        23u, 24u, 25u, 26u, 27u, 29u, 32u
    };
    for (uint16_t ordinal : ordinals)
    {
        uint32_t pc = k_extra_stub_base + ordinal * 0x20u;
        if (ordinal == 7u)
        {
            pc = k_set_dma_stub;
        }
        else if (ordinal == 32u)
        {
            pc = k_set_dma_intr_stub;
        }
        else if (ordinal == 23u)
        {
            pc = k_get_smflag_stub;
        }
        else if (ordinal == 24u)
        {
            pc = k_set_smflag_stub;
        }
        if (!create_stub(m_kernel.cpu(), pc,
                         k_service_base + ordinal) ||
            !m_kernel.register_library("sifman", ordinal, pc))
        {
            return false;
        }
    }
    // Every successful re-install follows an actual IOP kernel reboot;
    // no SIF init, DMA chain, or transaction is inferred from install.
    m_dma2_initialized = false;
    m_initialized = false;
    m_dma_chain_configured = false;
    m_next_dma_id = 1u;
    m_dma_success_log_count = 0u;
    return true;
}

bool c_sifman_bridge::initialize_dma2()
{
    if (m_dma2_initialized)
    {
        return true;
    }
    uint32_t control = 0u;
    if (!m_kernel.hardware().read(c_hw::k_dma_pcr, 4u, &control) ||
        !m_kernel.hardware().write(c_hw::k_dma_pcr, 4u,
                                   control | 0x800u))
    {
        return false;
    }
    m_dma2_initialized = true;
    return true;
}

bool c_sifman_bridge::configure_dma_chain()
{
    uint32_t bcr = 0u;
    if (!m_kernel.hardware().read(
            c_hw::k_sif1_dma_base + 4u, 4u, &bcr) ||
        !m_kernel.hardware().write(
            c_hw::k_sif1_dma_base + 4u, 4u,
            (bcr & 0xffff0000u) | 0x20u))
    {
        return false;
    }
    // The real sceSifSetDChain arms SIF1 DMA10; in this HLE-first
    // runtime EE SIF1 packets already pass through the validated
    // transport, so merely calling the initializer MUST NOT trigger
    // a phantom DMA transfer, callback or completion flag.
    m_dma_chain_configured = true;
    return true;
}

bool c_sifman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    switch (service)
    {
    case k_service_base + 4u: // sceSifDma2Init
        if (!initialize_dma2())
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    case k_service_base + 5u: // sceSifInit
    {
        if (!m_initialized)
        {
            uint32_t dpcr2 = 0u;
            if (!initialize_dma2() ||
                !kernel.hardware().read(c_hw::k_dma_pcr2, 4u, &dpcr2) ||
                !kernel.hardware().write(
                    c_hw::k_dma_pcr2, 4u, dpcr2 | 0x8800u) ||
                !kernel.hardware().write(
                    c_hw::k_sif0_dma_base + 8u, 4u, 0u) ||
                !kernel.hardware().write(
                    c_hw::k_sif1_dma_base + 8u, 4u, 0u) ||
                !configure_dma_chain())
            {
                return false;
            }
            m_initialized = true;
        }
        // This does not independently establish SIF_STAT_SIFINIT,
        // BOOTEND, an RPC reply or an IOP->EE DMA completion.
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;
    }

    case k_service_base + 6u: // sceSifSetDChain
        if (!configure_dma_chain())
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    case k_service_base + 8u: // sceSifDmaStat
        // The supported SIFMAN #7 transfer stages/commits and raises
        // the genuine EE SIF0 channel event synchronously. No async
        // SIFMAN transaction is pending when this guest service runs.
        kernel.cpu().state().m_gpr[2] =
            static_cast<uint32_t>(-1);
        return true;

    case k_service_base + 21u: // sceSifGetMSFlag
    {
        uint32_t msflag = 0u;
        if (!m_sif.get_reg(3u, &msflag))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = msflag;
        return true;
    }

    case k_service_base + 22u: // sceSifSetMSFlag (IOP ack)
    {
        // On the IOP, writes to MSFLAG acknowledge EE-owned bits.
        m_sif.acknowledge_ee_msflag(
            kernel.cpu().state().m_gpr[4]);
        uint32_t msflag = 0u;
        if (!m_sif.get_reg(3u, &msflag))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = msflag;
        return true;
    }

    case k_service_base + 25u: // sceSifGetMainAddr
    case k_service_base + 26u: // sceSifGetSubAddr
    {
        uint32_t address = 0u;
        if (!m_sif.get_reg(service == k_service_base + 25u
                               ? 1u : 2u, &address))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = address;
        return true;
    }

    case k_service_base + 27u: // sceSifSetSubAddr
    {
        const uint32_t guest = kernel.cpu().state().m_gpr[4];
        const uint32_t region = guest & 0xe0000000u;
        const uint32_t physical = guest & 0x1fffffffu;
        if ((guest & 3u) != 0u ||
            (region != 0u && region != 0x80000000u &&
             region != 0xa0000000u) ||
            (guest != 0u && physical >= c_cpu::k_ram_size))
        {
            return false;
        }
        m_sif.publish_iop_subaddr(guest);
        kernel.cpu().state().m_gpr[2] = guest;
        return true;
    }

    case k_service_base + 29u: // sceSifCheckInit
        kernel.cpu().state().m_gpr[2] = m_initialized ? 1u : 0u;
        return true;

    case k_set_dma_syscall:
    case k_set_dma_intr_syscall:
    {
        // Both sifman #7 and #32 receive 16-byte
        // descriptors (IOP src, EE dst, byte count, attributes).
        // Unlike the reference's eager memcpy, validate and stage
        // every transfer before writing a single guest EE byte.
        const s_cpu_state& state = kernel.cpu().state();
        const uint32_t list = state.m_gpr[4];
        const uint32_t count = state.m_gpr[5];
        // sceSifSetDmaIntr(a0=list,a1=count,a2=callback,a3=data).
        // A successful DMA with a non-null callback schedules actual
        // guest code; the callback is never a synthetic completion.
        const uint32_t callback = service == k_set_dma_intr_syscall
            ? state.m_gpr[6] : 0u;
        const uint32_t callback_data = state.m_gpr[7];
        const uint32_t callback_gp = state.m_gpr[28];

        // The retail ARKD worker reaches this import only after its
        // original method-1 RPC and the 0x503:258/2 follow-up calls
        // return. On rejection preserve the real IOP/EE state and
        // identify the exact validation boundary rather than guessing
        // which DMA producer is missing or synthesizing a completion.
        const auto reject = [&](const char* reason,
                                uint32_t descriptor_index = 0u,
                                uint32_t source = 0u,
                                uint32_t destination = 0u,
                                uint32_t size = 0u,
                                uint32_t attributes = 0u)
        {
            ee::s_ee_u128 channel{};
            const bool channel_readable =
                m_dmac != nullptr &&
                ee::c_ee_dmac::mmio_read(
                    m_dmac, ee::c_ee_dmac::k_sif0_base,
                    4u, &channel);

            // The receiver may be unarmed before we normally inspect
            // the descriptor list. Read at most the first rejected
            // descriptor for diagnostic purposes without changing
            // acceptance, DMA clocks or guest memory.
            const uint32_t list_region = list & 0xe0000000u;
            const uint32_t list_physical = list & 0x1fffffffu;
            bool descriptor_readable = false;
            if (count != 0u && descriptor_index < count &&
                (list & 3u) == 0u &&
                (list_region == 0u ||
                 list_region == 0x80000000u ||
                 list_region == 0xa0000000u) &&
                list_physical <= c_cpu::k_ram_size - 16u &&
                descriptor_index <=
                    (c_cpu::k_ram_size - 16u -
                     list_physical) / 16u)
            {
                const uint32_t address =
                    list_physical + descriptor_index * 16u;
                uint32_t traced_source = 0u;
                uint32_t traced_destination = 0u;
                uint32_t traced_size = 0u;
                uint32_t traced_attributes = 0u;
                descriptor_readable =
                    kernel.cpu().read32(address, &traced_source) &&
                    kernel.cpu().read32(address + 4u,
                                        &traced_destination) &&
                    kernel.cpu().read32(address + 8u,
                                        &traced_size) &&
                    kernel.cpu().read32(address + 12u,
                                        &traced_attributes);
                if (descriptor_readable)
                {
                    source = traced_source;
                    destination = traced_destination;
                    size = traced_size;
                    attributes = traced_attributes;
                }
            }

            std::fprintf(stderr,
                "IOP HLE SifSetDma rejected: reason=%s "
                "thread=%u pc=0x%08x ra=0x%08x "
                "list=0x%08x count=%u index=%u desc_readable=%u "
                "src=0x%08x dst=0x%08x size=%u attr=0x%08x "
                "ee5_chcr=0x%08x ee5_readable=%u "
                "iop_cycles=%llu\n",
                reason, kernel.current_thread_id(),
                state.m_pc, state.m_gpr[31],
                list, count, descriptor_index,
                descriptor_readable ? 1u : 0u,
                source, destination, size, attributes,
                static_cast<uint32_t>(channel.m_lo),
                channel_readable ? 1u : 0u,
                static_cast<unsigned long long>(kernel.cycles()));
            return false;
        };

        if (m_dmac == nullptr || m_ee_ram == nullptr)
        {
            return reject("missing_ee_dma_backend");
        }
        const uint32_t list_physical = list & 0x1fffffffu;
        const uint32_t list_region = list & 0xe0000000u;
        if (count == 0u || count > 64u ||
            (list & 3u) != 0u ||
            (list_region != 0u &&
             list_region != 0x80000000u &&
             list_region != 0xa0000000u) ||
            list_physical > c_cpu::k_ram_size - count * 16u)
        {
            return reject("invalid_descriptor_list");
        }

        if (callback != 0u)
        {
            const uint32_t region = callback & 0xe0000000u;
            const uint32_t physical = callback & 0x1fffffffu;
            uint32_t instruction = 0u;
            if ((callback & 3u) != 0u ||
                (region != 0u && region != 0x80000000u &&
                 region != 0xa0000000u) ||
                physical >= c_cpu::k_ram_size ||
                !kernel.cpu().read32(callback, &instruction))
            {
                return reject("invalid_dma_callback");
            }
            if (!kernel.can_schedule_hle_event(32u))
            {
                return reject("dma_callback_queue_unavailable");
            }
        }

        ee::s_ee_u128 chcr{};
        if (!ee::c_ee_dmac::mmio_read(
                m_dmac, ee::c_ee_dmac::k_sif0_base,
                4u, &chcr))
        {
            return reject("unreadable_ee_sif0_chcr");
        }
        if ((chcr.m_lo & ee::c_ee_dmac::k_chcr_str) == 0u)
        {
            return reject("ee_sif0_receiver_not_armed");
        }

        struct s_transfer
        {
            uint32_t m_iop_source = 0u;
            uint32_t m_ee_destination = 0u;
            std::vector<uint8_t> m_bytes;
        };
        std::vector<s_transfer> transfers;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const uint32_t descriptor = list_physical + index * 16u;
            uint32_t source = 0u;
            uint32_t destination = 0u;
            uint32_t size = 0u;
            uint32_t attributes = 0u;
            if (!kernel.cpu().read32(descriptor, &source) ||
                !kernel.cpu().read32(descriptor + 4u, &destination) ||
                !kernel.cpu().read32(descriptor + 8u, &size) ||
                !kernel.cpu().read32(descriptor + 12u, &attributes))
            {
                return reject("unreadable_descriptor", index,
                              source, destination, size, attributes);
            }

            const uint32_t source_physical = source & 0x1fffffffu;
            const uint32_t source_region = source & 0xe0000000u;
            const uint32_t destination_physical =
                destination & 0x1fffffffu;
            const uint32_t destination_region =
                destination & 0xe0000000u;
            const uint64_t rounded =
                (static_cast<uint64_t>(size) + 3u) & ~uint64_t(3u);
            // Retail ARKD uses a single 96 KiB IOP-to-EE DMA descriptor.
            // The checked IOP source and EE destination spans bound
            // each transfer; a 64 KiB cap incorrectly rejects valid RAM.
            if (size == 0u ||
                attributes != 0u ||
                (source_region != 0u &&
                 source_region != 0x80000000u &&
                 source_region != 0xa0000000u) ||
                (destination_region != 0u &&
                 destination_region != 0x80000000u &&
                 destination_region != 0xa0000000u) ||
                (source_physical & 3u) != 0u ||
                (destination_physical & 3u) != 0u ||
                static_cast<uint64_t>(source_physical) + rounded >
                    c_cpu::k_ram_size ||
                !m_ee_ram->contains(destination_physical, size))
            {
                return reject("invalid_descriptor_fields", index,
                              source, destination, size, attributes);
            }

            s_transfer transfer{};
            transfer.m_iop_source = source_physical;
            transfer.m_ee_destination = destination_physical;
            transfer.m_bytes.reserve(size);
            for (uint32_t offset = 0u; offset < size; offset += 4u)
            {
                uint32_t word = 0u;
                if (!kernel.cpu().read32(
                        source_physical + offset, &word))
                {
                    return reject("unreadable_source_payload", index,
                                  source, destination, size, attributes);
                }
                for (uint32_t b = 0u;
                     b < 4u && offset + b < size; ++b)
                {
                    transfer.m_bytes.push_back(
                        static_cast<uint8_t>(word >> (b * 8u))
                    );
                }
            }
            transfers.push_back(std::move(transfer));
        }

        // All IOP reads and EE destination ranges were checked.
        // Commit every packet and deliver the hardware SIF0 event
        // only when a guest IOP thread actually invokes sifman #7.
        for (const s_transfer& transfer : transfers)
        {
            for (size_t index = 0u; index < transfer.m_bytes.size();
                 ++index)
            {
                m_ee_ram->write8(
                    transfer.m_ee_destination +
                        static_cast<uint32_t>(index),
                    transfer.m_bytes[index]
                );
            }
        }
        if (kernel.hardware().pad_trace_window())
        {
            for (const s_transfer& transfer : transfers)
            {
                // The retail EE helper selects between two 128-byte
                // records. Report actual committed records of that size.
                if (transfer.m_bytes.size() == 128u)
                {
                    const uint32_t dst = transfer.m_ee_destination;
                    std::fprintf(stderr,
                        "PAD SIF record poll=%u ee_completed=%llu thread=%u ra=%08x "
                        "src=%08x dst=%08x frame=%u payload=%02x %02x %02x %02x "
                        "ee=%02x %02x %02x %02x\n",
                        kernel.hardware().pad0_polls(),
                        static_cast<unsigned long long>(kernel.hardware().pad_trace_dispatch()),
                        kernel.current_thread_id(), state.m_gpr[31],
                        transfer.m_iop_source, dst, m_ee_ram->read32(dst + 0x58u),
                        transfer.m_bytes[0], transfer.m_bytes[1],
                        transfer.m_bytes[2], transfer.m_bytes[3],
                        m_ee_ram->read8(dst), m_ee_ram->read8(dst + 1u),
                        m_ee_ram->read8(dst + 2u), m_ee_ram->read8(dst + 3u));
                }
            }
        }
        if (!m_dmac->raise(5u))
        {
            return reject("ee_sif0_dmac_completion");
        }
        if (callback != 0u)
        {
            // All DMA checks and the real DMA5 raise have completed.
            // The 32-tick delay matches ps2-web-recomp/SDBZ's job
            // scheduling; IOP guest instructions drive this clock.
            const bool scheduled = kernel.schedule_hle_event(
                32u, [this, callback, callback_gp, callback_data]()
                {
                    if (!m_kernel.start_guest_callback(
                            callback, callback_gp, callback_data))
                    {
                        std::fprintf(stderr,
                            "IOP SIFMAN DMA callback could not enter "
                            "guest code: pc=0x%08x gp=0x%08x "
                            "data=0x%08x cycles=%llu\n",
                            callback, callback_gp, callback_data,
                            static_cast<unsigned long long>(
                                m_kernel.cycles()));
                    }
                });
            if (!scheduled)
            {
                // Preflight already reserved capacity. This cannot
                // fail in the single-threaded IOP scheduler without
                // an intervening kernel reset or invariant violation.
                return reject("dma_callback_enqueue_failed");
            }
        }
        const uint32_t dma_id = m_next_dma_id++;
        kernel.cpu().state().m_gpr[2] = dma_id;
        if (m_dma_success_log_count < 8u)
        {
            uint64_t bytes = 0u;
            for (const s_transfer& transfer : transfers)
            {
                bytes += transfer.m_bytes.size();
            }
            std::fprintf(stderr,
                "IOP HLE SifSetDma accepted: service=0x%08x "
                "thread=%u list=0x%08x count=%u bytes=%llu "
                "dma_id=%u callback=0x%08x cycles=%llu\n",
                service, kernel.current_thread_id(), list, count,
                static_cast<unsigned long long>(bytes),
                dma_id, callback,
                static_cast<unsigned long long>(kernel.cycles()));
            ++m_dma_success_log_count;
        }
        return true;
    }

    case k_get_smflag_syscall:
    {
        uint32_t value = 0u;
        if (!m_sif.get_reg(4u, &value))
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] = value;
        return true;
    }

    case k_set_smflag_syscall:
        // Only the IOP owns the producer side of SMFLAG. No value
        // is published until a guest R3000 thread invokes this stub.
        m_sif.publish_iop_smflag(
            kernel.cpu().state().m_gpr[4]
        );
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    default:
        return false;
    }
}
} // namespace iop
