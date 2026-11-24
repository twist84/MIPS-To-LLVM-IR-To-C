#pragma once

// PS2 Image Processing Unit (MPEG-1/2 macroblock decoder).
//
// Adapted from ps2-web-recomp runtime/src/ipu/ipu.cpp (MIT, see
// LICENSE.upstream; VLC tables from Play!, BSD-2-Clause, see NOTICE).
// Commands BCLR, IDEC, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC and SETTH are
// executed against a bitstream pulled from toIPU DMA (channel 4) or the input
// FIFO; decoded output is pushed to fromIPU DMA (channel 3) or read from the
// output FIFO. A command that runs out of input stays busy and resumes from
// its last committed bit position when more data arrives.

#include "ee_runtime.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
class c_ee_dmac;
class c_ee_intc;

class c_ee_ipu
{
public:
    static constexpr uint32_t k_register_base = 0x10002000u;
    static constexpr uint32_t k_out_fifo = 0x10007000u;
    static constexpr uint32_t k_in_fifo = 0x10007010u;
    static constexpr uint32_t k_intc_cause = 8u;

    c_ee_ipu();

    void bind(c_ee_dmac* dmac, c_ee_intc* intc)
    {
        m_dmac = dmac;
        m_intc = intc;
    }

    void reset();
    uint32_t read32(uint32_t address);
    void write32(uint32_t address, uint32_t value);
    void write_fifo(const uint8_t* qword);
    void read_fifo(uint8_t* qword);
    void dma_started(uint32_t channel);

    static void dma_started_callback(void* user, uint32_t channel)
    {
        static_cast<c_ee_ipu*>(user)->dma_started(channel);
    }

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value);
    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value);

    uint64_t command_count(uint32_t command) const
    {
        return command < 16u ? m_commands[command] : 0u;
    }
    uint64_t stalls() const { return m_stalls; }
    uint64_t output_qwords() const { return m_out_qwords; }
    uint64_t bad_codes() const { return m_bad_codes; }
    bool busy() const { return m_busy; }

private:
    struct s_need_data {};
    struct s_bad_code {};

    bool pull();
    bool have(size_t bits);
    size_t available() const;
    uint32_t bits_at(size_t bit, unsigned count) const;
    uint32_t peek(unsigned count);
    uint32_t get(unsigned count);
    void skip(unsigned count);
    uint32_t peek_soft(unsigned count, size_t& available_bits);
    uint32_t peek_now() const;
    void commit();
    void top_up();

    unsigned dc_precision() const { return (m_ctrl >> 16u) & 3u; }
    bool alternate_scan() const { return (m_ctrl & 0x100000u) != 0u; }
    bool intra_vlc_one() const { return (m_ctrl & 0x200000u) != 0u; }
    bool nonlinear_q() const { return (m_ctrl & 0x400000u) != 0u; }
    bool mpeg1() const { return (m_ctrl & 0x800000u) != 0u; }
    unsigned picture_type() const { return (m_ctrl >> 24u) & 7u; }

    uint32_t decode_vlc(int table);
    int read_dc_diff(int channel);
    void read_coefficient(bool table_one, int& run, int& level);
    void decode_block(int16_t* out, int channel, bool intra, uint32_t qsc);
    void decode_macroblock(int16_t* raw, bool intra, bool field,
                           bool dc_reset, uint32_t qsc);
    void convert(const uint8_t* raw, bool rgb16);
    void emit_raw16(const int16_t* raw);
    bool start_code_follows();

    template <typename t_body>
    bool atomic(t_body&& body);

    bool run_idec();
    bool run_command();
    void drain();
    void step();

    c_ee_dmac* m_dmac = nullptr;
    c_ee_intc* m_intc = nullptr;

    std::vector<uint8_t> m_in;
    size_t m_bit = 0u;
    std::vector<uint8_t> m_out;
    size_t m_out_pos = 0u;

    uint32_t m_ctrl = 0u;
    uint32_t m_result = 0u;
    uint32_t m_command = 0u;
    int m_last = -1;
    bool m_busy = false;
    bool m_decoding = false;
    bool m_interrupt = true;
    bool m_starved = false;
    int m_phase = 0;
    uint32_t m_qsc = 0u;
    uint32_t m_count = 0u;
    bool m_first = true;
    uint8_t m_intra_iq[64]{};
    uint8_t m_nonintra_iq[64]{};
    uint16_t m_vqclut[16]{};
    uint16_t m_th0 = 0u;
    uint16_t m_th1 = 0u;
    int16_t m_dc[3]{};

    uint64_t m_commands[16]{};
    uint64_t m_stalls = 0u;
    uint64_t m_out_qwords = 0u;
    uint64_t m_bad_codes = 0u;
    uint8_t m_out_half[16]{};
    uint64_t m_trace_limit = 0u;
    uint64_t m_traced = 0u;
    void trace(const char* event, uint32_t value);
};

} // namespace ee
