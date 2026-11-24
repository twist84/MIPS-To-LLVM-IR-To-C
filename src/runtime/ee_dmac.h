#pragma once

#include "ee_runtime.h"
#include "ee_memory.h"

#include <cstdint>

namespace ee
{
class c_ee_vif1;
// EE DMA registers, normal/interleave SPR DMA, bounded SPR chains,
// and normal DMA for VIF0/VIF1/GIF through an optional host consumer.
// SIF0 channel 5 and SIF1 channel 6 registers are readable/writable,
// but genuine EE/IOP DMA transport is not connected yet.
// Includes VIF1 reverse GS image downloads (CHCR.DIR=0).
// Reference: ps2-web-recomp/runtime/src/dmac.cpp and hw.cpp.
// Transfers execute synchronously, prevalidating physical RAM ranges.
// GIF channel 2 supports bounded transactional REFE/CNT/NEXT/REF/END
// source chains in EE RAM and scratchpad-selected DMA memory.
// TTE, REFS, CALL/RET, MFIFO, general device emulation,
// arbitration and cycle-accurate completion remain unimplemented.
class c_ee_dmac
{
public:
    static constexpr uint32_t k_vif0_base = 0x10008000u; // channel 0
    static constexpr uint32_t k_vif1_base = 0x10009000u; // channel 1
    static constexpr uint32_t k_gif_base = 0x1000a000u; // channel 2
    static constexpr uint32_t k_from_ipu_base = 0x1000b000u; // channel 3, IPU -> EE
    static constexpr uint32_t k_to_ipu_base = 0x1000b400u; // channel 4, EE -> IPU
    static constexpr uint32_t k_sif0_base = 0x1000c000u; // channel 5, IOP -> EE
    static constexpr uint32_t k_sif1_base = 0x1000c400u; // channel 6, EE -> IOP
    static constexpr uint32_t k_stat_address = 0x1000e010u;
    static constexpr uint32_t k_sqwc_address = 0x1000e030u;
    static constexpr uint32_t k_channel_count = 10u;
    static constexpr uint32_t k_to_spr_base = 0x1000d400u; // channel 9
    static constexpr uint32_t k_from_spr_base = 0x1000d000u; // channel 8
    static constexpr uint32_t k_chcr_str = 0x100u;

    // Read-only bounded history of actual GIF channel MMIO writes.
    // The ring is host-owned and never changes guest DMA behavior.
    static constexpr uint32_t k_gif_trace_capacity = 32u;
    enum class e_gif_dma_reject : uint32_t
    {
        none = 0u,
        missing_memory_or_sink,
        invalid_start_state,
        invalid_tag_address,
        unsupported_tag,
        invalid_payload_span,
        chain_qwc_limit,
        chain_tag_limit,
        rejected_by_sink
    };
    struct s_gif_dma_write
    {
        uint64_t m_sequence = 0u;
        uint32_t m_offset = 0u;
        uint32_t m_value = 0u;
        uint32_t m_previous = 0u;
        uint32_t m_chcr = 0u;
        uint32_t m_madr = 0u;
        uint32_t m_qwc = 0u;
        uint32_t m_tadr = 0u;
        e_gif_dma_reject m_reject = e_gif_dma_reject::none;
    };

    struct s_gif_channel_state
    {
        uint32_t m_chcr = 0u;
        uint32_t m_madr = 0u;
        uint32_t m_qwc = 0u;
        uint32_t m_tadr = 0u;
    };

    // Physical GIF-channel state for host diagnostics (read only).
    s_gif_channel_state gif_channel_state() const;
    // The same read-only CHCR/MADR/QWC/TADR view for physical VIF1.
    // This does not advance the DMA engine or read guest MMIO.
    s_gif_channel_state vif1_channel_state() const;
    uint64_t gif_trace_count() const;
    bool gif_trace_newest(uint32_t offset, s_gif_dma_write* result) const;
    e_gif_dma_reject gif_last_reject() const;

    enum class e_vif1_chain_reject : uint32_t
    {
        none = 0u,
        missing_memory_or_sink,
        invalid_start_state,
        invalid_tag_address,
        unsupported_tag,
        invalid_payload_span,
        chain_qwc_limit,
        chain_tag_limit,
        rejected_by_sink,
        cyclic_chain
    };
    e_vif1_chain_reject vif1_chain_last_reject() const;

    void bind_cpu(s_ee_cpu_state* cpu);
    void bind_memory(ee_memory* memory);

    // Genuine SIF0 channel-5 DMA receives destination tags from the
    // IOP DMA9 hardware FIFO, never from EE TADR.
    using t_sif0_start_callback = void (*)(void* user);
    void bind_sif0_start(void* user, t_sif0_start_callback callback);

    // IPU channels follow the ps2-web-recomp pull/push model: starting either
    // channel notifies the IPU, which pulls toIPU (4) source-chain qwords as
    // its bitstream needs them and pushes decoded qwords to fromIPU (3).
    using t_ipu_start_callback = void (*)(void* user, uint32_t channel);
    void bind_ipu(void* user, t_ipu_start_callback start)
    {
        m_ipu_user = user;
        m_ipu_start = start;
    }
    bool ipu_pull(uint8_t* qword);
    bool ipu_push(const uint8_t* qword);
    bool begin_sif0_packet(uint32_t tag, uint32_t destination);
    bool commit_sif0_qwords(uint32_t qwc);

    // Genuine SIF1 channel-6 DMA uses an attached hardware FIFO engine.
    // A CHCR start notifies the engine; it never implies completion.
    using t_sif1_start_callback = void (*)(void* user);
    void bind_sif1_start(void* user, t_sif1_start_callback callback);
    // The FIFO engine has moved exactly qwc complete source quadwords.
    bool commit_sif1_qwords(uint32_t qwc);
    // A chain-mode sender reads EE source tags only as a consequence
    // of a guest DMA start. False means malformed/unsupported tag.
    bool prepare_sif1_chain_tag();

    // A host-owned, synchronous data consumer. channel is 0=VIF0,
    // 1=VIF1 (RAM-to-device), 2=GIF PATH3. data is a contiguous,
    // little-endian array of 16*qwc bytes valid only during the call.
    // Return true only after consuming the ENTIRE transfer. A false
    // return means the host rejected it without side effects; the DMA
    // controller leaves CHCR.STR, MADR and QWC unchanged.
    using t_device_transfer_callback = bool (*)(
        void* user, uint32_t channel, const uint8_t* data, uint32_t qwc
    );
    void bind_device_transfer(void* user, t_device_transfer_callback callback);

    // Optional deferred channel-1 forward transfers. The bound VIF
    // must own a worker, and the controller must service it on the EE
    // thread. STR, MADR/QWC/TADR and D_STAT remain uncommitted until
    // the whole VIF transaction (including GIF publication) succeeds.
    enum class e_deferred_vif1
    {
        idle,
        pending,
        completed,
        rejected
    };
    void bind_deferred_vif1(c_ee_vif1* vif1);
    bool deferred_vif1_pending() const { return m_vif1_pending; }
    e_deferred_vif1 service_deferred_vif1(bool wait = false);
    void cancel_deferred_vif1();

    // VIF1 channel 1, CHCR.DIR=0: device-to-EE-RAM DMA.
    // The producer fills all 16*qwc bytes or rejects the entire
    // transfer without changing its internal state. On rejection,
    // DMAC leaves EE RAM, MADR, QWC, STR and D_STAT unchanged.
    using t_device_download_callback = bool (*)(
        void* user, uint32_t channel, uint8_t* data, uint32_t qwc
    );
    void bind_device_download(
        void* user, t_device_download_callback callback
    );

    void reset();
    bool raise(uint32_t channel);
    bool acknowledge(uint32_t channel);

    uint32_t status() const;
    uint32_t pending() const;

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value);
    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value);

private:
    struct s_spr_channel
    {
        uint32_t m_chcr = 0;
        uint32_t m_madr = 0;
        uint32_t m_qwc = 0;
        uint32_t m_tadr = 0;
        uint32_t m_sadr = 0;
        uint32_t m_asr[2]{};
    };

    e_deferred_vif1 submit_deferred_vif1(const uint8_t* data, uint32_t qwc,
        const s_spr_channel& next, bool chain);
    void sync_interrupt_line();
    void trace_gif_write(uint32_t offset, uint32_t value,
                         uint32_t previous);
    bool read_spr_channel(uint32_t address, uint32_t* value) const;
    bool write_spr_channel(uint32_t address, uint32_t value);
    bool transfer_device_normal(uint32_t channel);
    bool transfer_gif_source_chain();
    bool transfer_vif1_source_chain();
    bool transfer_spr_normal(uint32_t channel);
    bool transfer_spr_interleave(uint32_t channel);
    bool transfer_from_spr_chain();
    bool transfer_to_spr_chain();
    bool walk_to_spr_chain(s_spr_channel& channel, bool copy_data);
    bool transfer_to_spr_span(s_spr_channel& channel, uint32_t qwc, bool copy_data);
    bool walk_from_spr_chain(s_spr_channel& channel, bool copy_data);
    bool transfer_from_spr_span(s_spr_channel& channel, uint32_t qwc, bool copy_data);

    s_ee_cpu_state* m_cpu = nullptr;
    void* m_sif0_user = nullptr;
    t_sif0_start_callback m_sif0_start = nullptr;
    void* m_sif1_user = nullptr;
    t_sif1_start_callback m_sif1_start = nullptr;
    ee_memory* m_memory = nullptr;
    void* m_device_user = nullptr;
    t_device_transfer_callback m_device_transfer = nullptr;
    c_ee_vif1* m_deferred_vif1 = nullptr;
    bool m_vif1_pending = false;
    bool m_vif1_pending_chain = false;
    s_spr_channel m_vif1_pending_next{};
    void* m_download_user = nullptr;
    t_device_download_callback m_device_download = nullptr;
    uint32_t m_status = 0;
    uint32_t m_sqwc = 0;
    s_gif_dma_write m_gif_trace[k_gif_trace_capacity]{};
    uint64_t m_gif_trace_count = 0u;
    e_gif_dma_reject m_gif_last_reject = e_gif_dma_reject::none;
    e_vif1_chain_reject m_vif1_chain_last_reject =
        e_vif1_chain_reject::none;
    s_spr_channel m_device_channels[3]{}; // VIF0, VIF1, GIF PATH3
    s_spr_channel m_sif0_channel{}; // SIF0, guest IOP DMA9 producer
    bool m_sif0_chain_end = false;
    s_spr_channel m_sif1_channel{}; // SIF1, no IOP receiver yet
    bool m_sif1_chain_end = false;
    uint32_t m_sif1_chain_tags = 0u;
    s_spr_channel m_spr_channels[2]{}; // fromSPR channel 8, toSPR channel 9
    s_spr_channel m_ipu_channels[2]{}; // fromIPU channel 3, toIPU channel 4
    bool m_to_ipu_tag_end = false;
    void* m_ipu_user = nullptr;
    t_ipu_start_callback m_ipu_start = nullptr;
    void finish_ipu(uint32_t channel);
    uint32_t ipu_dma_address(uint32_t address) const;
};
} // namespace ee
