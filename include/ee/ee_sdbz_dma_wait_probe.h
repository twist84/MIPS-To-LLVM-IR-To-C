#pragma once

#include "ee_memory.h"
#include "ee_dmac.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ee
{
// USA SLUS_214.42: 0x102560 polls guest command-state slots. The
// 0x102670 red-sprite loop is entered only after the timeout/recovery path.
// This is passive observation; it does not service the queue or read MMIO.
class c_ee_sdbz_dma_wait_probe
{
public:
    static constexpr uint32_t k_wait_pc = 0x00102560u;
    static constexpr uint32_t k_poll_pc = 0x001025a4u;
    static constexpr uint32_t k_vif1_handler_pc = 0x00102a60u;
    static constexpr uint32_t k_slot_flag_base = 0x00502368u;
    static constexpr uint32_t k_slot_selector = 0x005030acu;

    struct s_slot_observation
    {
        uint32_t m_selector = 0u;
        uint32_t m_flag0 = 0u;
        uint32_t m_flag1 = 0u;
    };

    // Sample backed RAM only. No MMIO or guest timer reads, and no
    // writes to the slot, DMA or interrupt state.
    static bool sample_slots(ee_memory* memory, s_slot_observation& sample)
    {
        if (memory == nullptr ||
            !memory->contains(k_slot_flag_base, 8u) ||
            !memory->contains(k_slot_selector, 4u))
        {
            return false;
        }
        sample.m_selector = memory->read32(k_slot_selector);
        sample.m_flag0 = memory->read32(k_slot_flag_base);
        sample.m_flag1 = memory->read32(k_slot_flag_base + 4u);
        return true;
    }

    // Only correlate the previously decoded retail handler with a
    // suspended command-slot loop. A callback boundary is not an
    // instruction-level writer trace.
    static bool is_slot_callback(bool is_intc, uint32_t cause,
                                 uint32_t handler_pc, uint32_t resume_pc)
    {
        return !is_intc && cause == 1u &&
            handler_pc == k_vif1_handler_pc && resume_pc == k_poll_pc;
    }
    static constexpr uint32_t k_red_loop_pc = 0x00102670u;
    static constexpr uint32_t k_slot_base = 0x00502350u;
    static constexpr size_t k_slot_words = 32u;
    static constexpr std::array<uint32_t, 7u> k_extra_addresses =
    {
        0x00502630u, 0x00502634u, 0x0050263cu, 0x00502640u,
        0x00503084u, 0x005030acu, 0x005023f0u
    };

    struct s_snapshot
    {
        bool m_observed = false;
        uint32_t m_pc = 0u;
        uint64_t m_dispatch = 0u;
        uint32_t m_thread = 0u;
        std::array<uint32_t, k_slot_words> m_slot_words{};
        std::array<uint32_t, k_extra_addresses.size()> m_extra_words{};
        bool m_vif1_observed = false;
        uint32_t m_vif1_chcr = 0u;
        uint32_t m_vif1_madr = 0u;
        uint32_t m_vif1_qwc = 0u;
        uint32_t m_vif1_tadr = 0u;
        uint32_t m_dmac_status = 0u;
        uint32_t m_dmac_pending = 0u;
        uint32_t m_vif1_chain_reject = 0u;
        // Four adjacent physical source qwords, before any chain
        // interpretation, for the first blocked guest DMA.
        bool m_vif1_source_valid = false;
        std::array<uint64_t, 8u> m_vif1_source_words{};

        uint32_t selected_slot() const
        {
            return m_extra_words[5];
        }

        uint32_t polled_slot() const
        {
            // In the ELF, 0x00102578 XORs the active slot with one.
            return selected_slot() ^ 1u;
        }

        uint32_t polled_flag() const
        {
            return polled_slot() == 0u ? flag0() :
                polled_slot() == 1u ? flag1() : 0u;
        }

        uint32_t flag0() const
        {
            // 0x00502368 = base+0x18
            return m_slot_words[6];
        }

        uint32_t flag1() const
        {
            // 0x0050236c = base+0x1c
            return m_slot_words[7];
        }
    };

    // Return true only for the first reached, backed snapshot at each
    // identified PC. For other games / unbacked RAM no sample is invented.
    bool observe(uint32_t pc, uint64_t dispatch, uint32_t thread,
                 ee_memory* memory, const c_ee_dmac* dmac)
    {
        const bool wait_needed = pc == k_wait_pc &&
            (!m_wait.m_observed || !m_pending_wait.m_observed);
        const bool red_needed = pc == k_red_loop_pc && !m_red.m_observed;
        if ((!wait_needed && !red_needed) || memory == nullptr ||
            !memory->contains(k_slot_base, k_slot_words * sizeof(uint32_t)))
        {
            return false;
        }
        for (uint32_t address : k_extra_addresses)
        {
            if (!memory->contains(address, sizeof(uint32_t)))
            {
                return false;
            }
        }

        s_snapshot sample{};
        sample.m_pc = pc;
        sample.m_dispatch = dispatch;
        sample.m_thread = thread;
        for (size_t i = 0u; i < sample.m_slot_words.size(); ++i)
        {
            sample.m_slot_words[i] =
                memory->read32(k_slot_base + static_cast<uint32_t>(4u * i));
        }
        for (size_t i = 0u; i < sample.m_extra_words.size(); ++i)
        {
            sample.m_extra_words[i] = memory->read32(k_extra_addresses[i]);
        }
        if (dmac != nullptr)
        {
            const auto vif1 = dmac->vif1_channel_state();
            sample.m_vif1_observed = true;
            sample.m_vif1_chcr = vif1.m_chcr;
            sample.m_vif1_madr = vif1.m_madr;
            sample.m_vif1_qwc = vif1.m_qwc;
            sample.m_vif1_tadr = vif1.m_tadr;
            sample.m_dmac_status = dmac->status();
            sample.m_dmac_pending = dmac->pending();
            sample.m_vif1_chain_reject = static_cast<uint32_t>(
                dmac->vif1_chain_last_reject());
            const uint32_t tadr = vif1.m_tadr;
            if ((tadr & 0x80000000u) == 0u &&
                memory->contains(tadr, 64u))
            {
                for (size_t i = 0u; i < 4u; ++i)
                {
                    s_ee_u128 quad{};
                    memory->read128(tadr + static_cast<uint32_t>(i * 16u),
                                    &quad);
                    sample.m_vif1_source_words[2u * i] = quad.m_lo;
                    sample.m_vif1_source_words[2u * i + 1u] = quad.m_hi;
                }
                sample.m_vif1_source_valid = true;
            }
        }
        sample.m_observed = true;
        if (wait_needed)
        {
            bool captured = false;
            if (!m_wait.m_observed)
            {
                m_wait = sample;
                captured = true;
            }
            if (!m_pending_wait.m_observed &&
                sample.polled_slot() < 2u &&
                sample.polled_flag() != 0u)
            {
                m_pending_wait = sample;
                captured = true;
            }
            return captured;
        }
        m_red = sample;
        return true;
    }

    const s_snapshot& wait() const
    {
        return m_wait;
    }

    const s_snapshot& pending_wait() const
    {
        return m_pending_wait;
    }

    const s_snapshot& red_loop() const
    {
        return m_red;
    }

    void write_json(FILE* output) const
    {
        if (output == nullptr)
        {
            return;
        }
        std::fputs("{\"schema\":\"ee-sdbz-dma-wait-v1\","
            "\"observation_unit\":\"native_dispatch_entry\","
            "\"known_elf\":\"SLUS_214.42\","
            "\"limitation\":\"read_only_guest_RAM_not_MMIO_or_causality\","
            "\"wait\":", output);
        write_snapshot_json(output, m_wait);
        std::fputs(",\"pending_wait\":", output);
        write_snapshot_json(output, m_pending_wait);
        std::fputs(",\"red_loop\":", output);
        write_snapshot_json(output, m_red);
        std::fputs("}", output);
    }

private:
    static void write_snapshot_json(FILE* output, const s_snapshot& s)
    {
        if (!s.m_observed)
        {
            std::fputs("null", output);
            return;
        }
        std::fprintf(output,
            "{\"pc\":%u,\"dispatch\":%llu,\"thread\":%u,"
            "\"selected_slot\":%u,\"polled_slot\":%u,"
            "\"polled_flag_valid\":%s,\"polled_flag\":%u,"
            "\"flag0\":%u,\"flag1\":%u,"
            "\"slot_base\":%u,\"slot_words\":[",
            s.m_pc, static_cast<unsigned long long>(s.m_dispatch),
            s.m_thread, s.selected_slot(), s.polled_slot(),
            s.polled_slot() < 2u ? "true" : "false", s.polled_flag(),
            s.flag0(), s.flag1(),
            k_slot_base);
        for (size_t i = 0u; i < s.m_slot_words.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", s.m_slot_words[i]);
        }
        std::fputs("],\"extra_addresses\":[", output);
        for (size_t i = 0u; i < k_extra_addresses.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", k_extra_addresses[i]);
        }
        std::fputs("],\"extra_words\":[", output);
        for (size_t i = 0u; i < s.m_extra_words.size(); ++i)
        {
            std::fprintf(output, "%s%u", i ? "," : "", s.m_extra_words[i]);
        }
        std::fputs("],\"vif1\":", output);
        if (s.m_vif1_observed)
        {
            std::fprintf(output,
                "{\"chcr\":%u,\"madr\":%u,\"qwc\":%u,\"tadr\":%u,"
                "\"dmac_status\":%u,\"dmac_pending\":%u,"
                "\"chain_reject\":%u}",
                s.m_vif1_chcr, s.m_vif1_madr,
                s.m_vif1_qwc, s.m_vif1_tadr,
                s.m_dmac_status, s.m_dmac_pending,
                s.m_vif1_chain_reject);
        }
        else
        {
            std::fputs("null", output);
        }
        std::fputs(",\"source_prefix_qwords\":", output);
        if (s.m_vif1_source_valid)
        {
            std::fputs("[", output);
            for (size_t i = 0u; i < s.m_vif1_source_words.size(); ++i)
            {
                std::fprintf(output, "%s\"0x%016llx\"",
                    i ? "," : "",
                    static_cast<unsigned long long>(
                        s.m_vif1_source_words[i]));
            }
            std::fputs("]", output);
        }
        else
        {
            std::fputs("null", output);
        }
        std::fputs("}", output);
    }

    s_snapshot m_wait{};
    s_snapshot m_pending_wait{};
    s_snapshot m_red{};
};

} // namespace ee
