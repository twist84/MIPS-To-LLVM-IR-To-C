#pragma once

#include "iop_spu2.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace iop
{
class c_cpu;
class c_memory_card;

// The first guest-visible IOP hardware registers needed for ROM0
// exploration. This is NOT a complete IOP bus: unknown devices fail
// instead of reading zero or silently accepting writes.
class c_hw
{
public:
    using t_sif_mmio_read = bool (*)(
        void*, uint32_t, uint32_t, uint32_t*
    );
    using t_sif_mmio_write = bool (*)(
        void*, uint32_t, uint32_t, uint32_t
    );
    using t_sif0_start_callback = void (*)(void* user);
    using t_sif1_start_callback = void (*)(void* user);
    using t_cdvd_start_callback = void (*)(void* user);
    static constexpr uint32_t k_scratchpad_base = 0x1f800000u;
    static constexpr uint32_t k_scratchpad_size = 1024u;
    static constexpr uint32_t k_i_stat = 0x1f801070u;
    static constexpr uint32_t k_i_mask = 0x1f801074u;
    static constexpr uint32_t k_i_ctrl = 0x1f801078u;
    static constexpr uint32_t k_cache_control = 0xfffe0130u;
    // PS2SDK SSBUSC SPU2 memory window and bus delay configuration.
    // These registers select bus windows; they are not SPU2 voices or RAM.
    static constexpr uint32_t k_ssbus_spu2_addr = 0x1f801404u;
    static constexpr uint32_t k_ssbus_spu2_addr1 = 0x1f80140cu;
    static constexpr uint32_t k_ssbus_spu2_delay = 0x1f801014u;
    static constexpr uint32_t k_ssbus_spu2_delay1 = 0x1f801414u;
    // SIO2 serial-interface control. The HLE path performs actual
    // bounded controller transfers on guest CTRL.start, not on
    // register setup, and raises IRQ17 only for a completed exchange.
    static constexpr uint32_t k_sio2_send3_base = 0x1f808200u;
    static constexpr uint32_t k_sio2_port_base = 0x1f808240u;
    static constexpr uint32_t k_sio2_tx_fifo = 0x1f808260u;
    static constexpr uint32_t k_sio2_rx_fifo = 0x1f808264u;
    static constexpr uint32_t k_sio2_ctrl = 0x1f808268u;
    static constexpr uint32_t k_sio2_stat = 0x1f80826cu;
    static constexpr uint32_t k_sio2_port_status = 0x1f808270u;
    static constexpr uint32_t k_sio2_aux_status = 0x1f808274u;
    // Original SIO2MAN IRQ17 handler reads this latch and writes the
    // value back. Bit 0 = transfer complete; bit 1 = transfer error.
    // Write-one-to-clear is independent of IOP INTC I_STAT.
    static constexpr uint32_t k_sio2_intr = 0x1f808280u;
    static constexpr size_t k_sio2_tx_limit = 4096u;
    // OpenPS2BIOS emits guest TTY bytes to this emulator-facing port.
    // It is a write-only diagnostic extension, not IOP boot readiness.
    static constexpr uint32_t k_debug_console = 0x1f802080u;
    static constexpr size_t k_debug_console_capacity = 4096u;
    // CDVD byte register: 0x00 means no inserted disc.
    // Standalone ROM0 boot has no attached media. A media backend
    // must eventually replace this default with detected disc type.
    // Timer 5 is the IOP kernel's 32-bit periodic clock (IRQ 16).
    static constexpr uint32_t k_timer5_count = 0x1f8014a0u;
    static constexpr uint32_t k_timer5_mode = 0x1f8014a4u;
    static constexpr uint32_t k_timer5_target = 0x1f8014a8u;
    static constexpr uint32_t k_timer5_irq = 16u;
    // Primary IOP DMA3 (CD-ROM) register bank, observed through KSEG1.
    static constexpr uint32_t k_cdrom_dma_base = 0x1f8010b0u;
    static constexpr uint32_t k_cdrom_dma_chcr = k_cdrom_dma_base + 8u;
    // SPU2 core 0 DMA4 and core 1 DMA7. Original PS2 IOP DMAC
    // channel slots: MADR, BCR, CHCR, TADR (16-byte register stride).
    // Supports aligned byte, halfword and word register access;
    // control writes alone never start or complete sound DMA.
    static constexpr uint32_t k_spu2_dma4_base = 0x1f8010c0u;
    static constexpr uint32_t k_spu2_dma7_base = 0x1f801500u;
    static constexpr uint32_t k_spu2_dma_register_size = 0x10u;
    static constexpr uint32_t k_spu2_dma_chcr_offset = 8u;
    static constexpr uint32_t k_spu2_dma_str_bit = 0x01000000u;
    static constexpr uint32_t k_dma_pcr = 0x1f8010f0u;
    static constexpr uint32_t k_dma_pcr2 = 0x1f801570u;
    // Secondary DMA global-enable control, used by the firmware's SIF.
    static constexpr uint32_t k_dma_global_enable = 0x1f801578u;
    // Secondary controller DMA9 (IOP -> EE SIF0) and
    // DMA10 (EE -> IOP SIF1) channel registers.
    // Secondary DMA11/12 used by real SIO2 controller exchanges.
    // Programming STR only arms a request; CTRL.start drives the transfer.
    static constexpr uint32_t k_sio2_dma_rx_base = 0x1f801540u;
    static constexpr uint32_t k_sio2_dma_tx_base = 0x1f801550u;
    static constexpr uint32_t k_sif0_dma_base = 0x1f801520u;
    static constexpr uint32_t k_sif1_dma_base = 0x1f801530u;
    // IOP-side SIF bus mirror of the EE's SBUS F200-F230 registers.
    static constexpr uint32_t k_sif_mscom = 0x1d000000u;
    static constexpr uint32_t k_sif_smcom = 0x1d000010u;
    static constexpr uint32_t k_sif_msflag = 0x1d000020u;
    static constexpr uint32_t k_sif_smflag = 0x1d000030u;
    static constexpr uint32_t k_dma_icr = 0x1f8010f4u;
    static constexpr uint32_t k_dma_icr2 = 0x1f801574u;
    static constexpr uint32_t k_dma_irq = 3u;
    // CDVD N-command status: bit 6=controller ready, bit 7=busy.
    // A controller can be idle/ready without a disc inserted.
    static constexpr uint32_t k_cdvd_ncmd_command = 0x1f402004u;
    // This address reads N-ready status, but writes command params.
    static constexpr uint32_t k_cdvd_ncmd_status = 0x1f402005u;
    static constexpr uint32_t k_cdvd_ncmd_param_capacity = 16u;
    static constexpr uint8_t k_cdvd_ncmd_ready = 0x40u;
    static constexpr uint8_t k_cdvd_ncmd_busy = 0x80u;
    // CDVD I_STAT: byte-wide interrupt-reason register, write-one-to-clear.
    static constexpr uint32_t k_cdvd_i_stat = 0x1f402008u;
    // CDVD decryption configuration byte. This is register storage,
    // not a disc-sector decryption implementation.
    static constexpr uint32_t k_cdvd_dec_set = 0x1f40203au;
    static constexpr uint32_t k_cdvd_disc_type = 0x1f40200fu;
    static constexpr uint8_t k_cdvd_no_disc = 0x00u;

    c_hw();
    void reset();
    // Link to the EE's *real* SIF register model only when a live
    // EE runtime is attached. Local SIF state remains standalone.
    void bind_sif_mmio(void* user, t_sif_mmio_read read,
                       t_sif_mmio_write write);
    void bind_sif0_start(void* user, t_sif0_start_callback callback);
    void bind_sif1_start(void* user, t_sif1_start_callback callback);
    void bind_cdvd_start(void* user, t_cdvd_start_callback callback);
    // Only a connected IOP CPU permits actual SPU2 DMA4/7 transfers.
    // Unbound MMIO remains register-only for standalone hardware tests.
    void bind_dma_cpu(c_cpu* cpu);
    void bind_memory_card(c_memory_card* card) { m_memory_card = card; }
    bool complete_memory_card_auth_reset(uint32_t port, uint32_t slot);
    // Optional host-provided input probe. Values count only authentic
    // port-0 SIO2 command 0x42 polls; zero duration disables it.
    // No IOP event, packet or completion is generated by configuring it.
    void set_sio2_pad0_input(uint16_t buttons, uint8_t right_x,
        uint8_t right_y, uint8_t left_x, uint8_t left_y);
    void configure_sio2_pad0_cross_pulse(
        uint32_t first_poll, uint32_t poll_count);
    // Optional scripted holds (SDBZ_PAD0_SCRIPT). Each step ORs its PS2
    // button bits into authentic port-0 0x42 poll replies numbered
    // [first_poll, first_poll + poll_count). Poll numbering is shared with
    // the Cross pulse; configuring a script never queues a packet.
    struct s_pad0_script_step
    {
        uint32_t m_first_poll = 0u;
        uint32_t m_poll_count = 0u;
        uint16_t m_buttons = 0u;
    };
    void configure_sio2_pad0_script(const std::vector<s_pad0_script_step>& steps)
    {
        m_sio2_pad0_script = steps;
        m_sio2_pad0_script_pressed_polls = 0u;
    }
    uint32_t sio2_pad0_poll_count() const { return m_sio2_pad0_poll_count; }
    uint32_t sio2_pad0_script_pressed_polls() const
    {
        return m_sio2_pad0_script_pressed_polls;
    }
    // Completed EE dispatches at the start of this runner iteration.
    void set_pad_trace_dispatch(uint64_t dispatch)
    {
        m_pad_trace_dispatch = dispatch;
    }
    uint64_t pad_trace_dispatch() const
    {
        return m_pad_trace_dispatch;
    }
    uint32_t pad0_polls() const
    {
        return m_sio2_pad0_poll_count;
    }
    bool pad_trace_window() const;
    bool spu2_ram_word(uint32_t address, uint16_t* value) const;
    // A copied sound DMA is a real device event even when no IOP
    // guest threads are runnable. Needed for bounded idle scheduling.
    uint32_t spu2_dma_cycles_remaining() const;

    // For the ordinary MMIO/scratchpad range, address is physical.
    // The cache-control register retains its privileged full address.
    // Reads may have side effects (I_CTRL clears on read).
    bool read(uint32_t address, uint32_t width, uint32_t* value);
    bool write(uint32_t address, uint32_t width, uint32_t value);

    // IRQ bookkeeping only. Delivery through R3000 COP0 exception
    // entry is a separate milestone; this does not synthesize it.
    bool raise_interrupt(uint32_t line);
    // INTRMAN sources 0..31 use I_MASK; 32..38 and 40..45 are
    // primary/secondary DMA IRQs whose enables live in DICR/DICR2.
    // These operations only program masks, not DMA completion flags.
    static bool supports_interrupt_line(uint32_t line);
    bool set_interrupt_mask(uint32_t line, bool enabled);
    uint32_t pending_interrupts() const;
    // Resolve masked DMA channels before ordinary IRQ lines, as INTRMAN does.
    int32_t next_pending_interrupt() const;
    bool acknowledge_interrupt(uint32_t line);
    uint32_t cache_control() const;
    // Advance IOP timer clock by completed guest execution cycles.
    // The standalone R3000 runtime currently counts one cycle per instruction.
    void advance_cycles(uint32_t cycles);
    // A future actual DMA channel completion may assert its flag.
    // Merely writing ICR enables must never complete any transfer.
    bool complete_dma_channel(uint32_t channel);
    // The EE side can announce a real request and acknowledge IOP
    // flag bits. Standalone ROM0 never invokes these operations.
    void ee_write_mscom(uint32_t value);
    void ee_set_msflag(uint32_t bits);
    void ee_clear_smflag(uint32_t bits);
    uint32_t sif_smcom() const;
    uint32_t sif_smflag() const;
    const std::string& debug_console() const;
    bool debug_console_truncated() const;
    // Expose pending work to an eventual real CDVD sector provider.
    bool cdvd_ncmd_pending() const;
    uint8_t cdvd_ncmd_command() const;
    uint32_t cdvd_ncmd_parameter_count() const;
    bool cdvd_ncmd_parameter(uint32_t index, uint8_t* value) const;
    bool cdrom_dma_register(uint32_t index, uint32_t* value) const;
    bool complete_cdvd_read(uint8_t interrupt_reason);
    size_t sio2_pending_tx_bytes() const;
    bool sio2_tx_byte(size_t index, uint8_t* value) const;
    bool sio2_transfer_requested() const;
    // Observability only: real guest MMIO requests and completed
    // SIO2 transactions. Reading these values has no guest side effects.
    uint32_t sio2_ctrl_starts() const;
    uint32_t sio2_packets() const;
    uint32_t sio2_completed_transfers() const;

    // Host-only rejection attribution. The guest's real SIO2 CTRL.start
    // may fail preflight without IRQ17; do not infer a button press from
    // merely receiving SEND3 packets.
    enum class e_sio2_rejection : uint32_t
    {
        none = 0,
        cpu_unavailable,
        dma11_geometry,
        dma11_source,
        dma12_geometry,
        dma12_destination,
        packet_incomplete,
        no_processed_packet,
        dma12_packet_count,
        dma12_reply_underflow,
        dma12_block_too_small,
        dma12_write
    };
    struct s_sio2_packet_observation
    {
        uint32_t m_port = 0u;
        uint32_t m_send3 = 0u;
        uint32_t m_input_size = 0u;
        uint32_t m_output_size = 0u;
        uint32_t m_first_tx_byte = 0u;
        uint32_t m_command = 0u;
        bool m_pad_present = false;
    };
    struct s_sio2_diagnostics
    {
        uint32_t m_ctrl = 0u;
        uint32_t m_stat = 0u;
        uint32_t m_intr = 0u;
        uint32_t m_packet_index = 0u;
        uint32_t m_last_start_guest_pc = 0u;
        uint32_t m_last_start_guest_ra = 0u;
        uint32_t m_packets_at_last_start = 0u;
        size_t m_tx_bytes_at_last_start = 0u;
        size_t m_rx_bytes_at_last_start = 0u;
        uint32_t m_first_packet_count = 0u;
        s_sio2_packet_observation m_first_packets[4]{};
        uint32_t m_send3[4]{};
        uint32_t m_dma11[3]{};
        uint32_t m_dma12[3]{};
        size_t m_tx_bytes = 0u;
        size_t m_rx_bytes = 0u;
        uint32_t m_empty_rx_reads = 0u;
        uint32_t m_pad0_polls = 0u;
        uint32_t m_pad0_cross_pressed_polls = 0u;
        uint32_t m_pad0_cross_first_poll = 0u;
        uint32_t m_pad0_cross_poll_count = 0u;
        uint32_t m_pad0_cross_bytes_pio_read = 0u;
        uint32_t m_pad0_cross_bytes_dma_written = 0u;
        uint32_t m_pad0_cross_bytes_fifo_discarded = 0u;
        e_sio2_rejection m_last_rejection = e_sio2_rejection::none;
        uint32_t m_rejected_starts = 0u;
    };
    s_sio2_diagnostics sio2_diagnostics() const;
    const char* sio2_rejection_name() const;

private:
    bool scratchpad_range(uint32_t address, uint32_t width) const;
    bool sif_dma_register(uint32_t address, uint32_t* channel,
                          uint32_t* reg) const;
    bool spu2_dma_register(uint32_t address, uint32_t width,
                           uint32_t* core, uint32_t* reg,
                           uint32_t* shift) const;
    uint32_t update_dma_icr(uint32_t previous, uint32_t value);
    void assert_dma_irq(uint32_t& control, bool sif_channel);
    void start_spu2_dma(uint32_t core);
    void advance_spu2_dma(uint32_t cycles);
    bool process_sio2_packet();
    bool run_sio2_transfer();

    // Guest SPU2 register bank is separate from SSBUS configuration.
    c_spu2 m_spu2;
    uint8_t m_scratchpad[k_scratchpad_size]{};
    // No CDVD command backend is connected yet, so no command may set
    // interrupt reasons spontaneously. Acknowledge writes clear bits.
    uint8_t m_cdvd_i_stat = 0u;
    uint8_t m_cdvd_dec_set = 0u;
    uint8_t m_cdvd_ncmd_command = 0u;
    uint8_t m_cdvd_ncmd_params[k_cdvd_ncmd_param_capacity]{};
    uint32_t m_cdvd_ncmd_param_count = 0u;
    bool m_cdvd_ncmd_pending = false;
    uint32_t m_i_stat = 0u;
    uint32_t m_i_mask = 0u;
    uint32_t m_i_ctrl = 1u;
    uint32_t m_cache_control = 0u;
    uint32_t m_ssbus_spu2_addr = 0u;
    uint32_t m_ssbus_spu2_addr1 = 0u;
    uint32_t m_ssbus_spu2_delay = 0u;
    uint32_t m_ssbus_spu2_delay1 = 0u;
    struct s_sio2_pad
    {
        bool m_connected = false;
        bool m_config = false;
        bool m_locked = false;
        uint8_t m_mode = 0x41u;
        uint8_t m_poll_mask[3]{0xffu, 0xffu, 0x03u};
        uint8_t m_motors[6]{0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu};
    };
    uint32_t m_sio2_ctrl = 0u;
    uint32_t m_sio2_stat = 0u;
    uint32_t m_sio2_intr = 0u;
    uint32_t m_sio2_index = 0u;
    uint32_t m_sio2_send3[16]{};
    uint32_t m_sio2_port[8]{};
    s_sio2_pad m_sio2_pad[2]{};
    c_memory_card* m_memory_card = nullptr;
    std::vector<uint8_t> m_sio2_pending_tx;
    std::deque<uint8_t> m_sio2_rx;
    // Host-only provenance tag for each queued RX byte. A tag is set
    // only for the active-low Cross byte of a genuine port-0 0x42 reply.
    std::deque<uint8_t> m_sio2_rx_cross_tags;
    // Host-only count of genuine reads while the SIO2 output FIFO is empty.
    // Original SDBZ SIO2 register semantics return 0xff for such a read.
    uint32_t m_sio2_empty_rx_read_count = 0u;
    uint64_t m_pad_trace_dispatch = 0u;
    uint16_t m_pad0_buttons = 0u;
    uint8_t m_pad0_axes[4] = {0x80u, 0x80u, 0x80u, 0x80u};
    uint32_t m_sio2_pad0_poll_count = 0u;
    uint32_t m_sio2_pad0_cross_pressed_polls = 0u;
    uint32_t m_sio2_pad0_cross_bytes_pio_read = 0u;
    uint32_t m_sio2_pad0_cross_bytes_dma_written = 0u;
    uint32_t m_sio2_pad0_cross_bytes_fifo_discarded = 0u;
    uint32_t m_sio2_pad0_cross_first_poll = 0u;
    uint32_t m_sio2_pad0_cross_poll_count = 0u;
    std::vector<s_pad0_script_step> m_sio2_pad0_script;
    uint32_t m_sio2_pad0_script_pressed_polls = 0u;
    bool m_sio2_transfer_requested = false;
    uint32_t m_sio2_ctrl_start_count = 0u;
    uint32_t m_sio2_packet_count = 0u;
    uint32_t m_sio2_transfer_count = 0u;
    uint32_t m_sio2_last_start_guest_pc = 0u;
    uint32_t m_sio2_last_start_guest_ra = 0u;
    uint32_t m_sio2_packets_at_last_start = 0u;
    size_t m_sio2_tx_bytes_at_last_start = 0u;
    size_t m_sio2_rx_bytes_at_last_start = 0u;
    s_sio2_packet_observation m_sio2_first_packets[4]{};
    e_sio2_rejection m_sio2_last_rejection = e_sio2_rejection::none;
    uint32_t m_sio2_rejected_start_count = 0u;
    uint32_t m_timer5_count = 0u;
    uint32_t m_timer5_mode = 0u;
    uint32_t m_timer5_target = 0u;
    uint32_t m_timer5_remainder = 0u;
    bool m_timer5_irq_armed = true;
    // MADR/BCR/CHCR; no CD-ROM transfer backend exists yet.
    uint32_t m_cdrom_dma[3]{};
    // DMA4/DMA7 owned by the two distinct SPU2 cores. The guest may
    // program these registers, but no fake DMA data or finish is made.
    uint32_t m_spu2_dma[2][4]{};
    // Countdown starts only after an actual IOP <-> sound-RAM copy.
    // A guest CHCR.STR clear cancels the pending completion.
    uint32_t m_spu2_dma_remaining[2]{};
    c_cpu* m_dma_cpu = nullptr;
    uint32_t m_dma_pcr = 0u;
    uint32_t m_dma_pcr2 = 0u;
    uint32_t m_dma_global_enable = 0u;
    // MADR, BCR, CHCR and TADR; transfers remain active until the
    // connected DMA backend explicitly completes them.
    uint32_t m_sif_dma[2][4]{};
    uint32_t m_sio2_dma[2][3]{};
    uint32_t m_sif_mscom = 0u;
    uint32_t m_sif_smcom = 0u;
    uint32_t m_sif_msflag = 0u;
    uint32_t m_sif_smflag = 0u;
    void* m_sif_mmio_user = nullptr;
    void* m_sif0_user = nullptr;
    t_sif0_start_callback m_sif0_start = nullptr;
    void* m_sif1_user = nullptr;
    t_sif1_start_callback m_sif1_start = nullptr;
    t_sif_mmio_read m_sif_mmio_read = nullptr;
    t_sif_mmio_write m_sif_mmio_write = nullptr;
    void* m_cdvd_user = nullptr;
    t_cdvd_start_callback m_cdvd_start = nullptr;
    uint32_t m_dma_icr = 0u;
    uint32_t m_dma_icr2 = 0u;
    std::string m_debug_console;
    bool m_debug_console_truncated = false;
};
} // namespace iop
