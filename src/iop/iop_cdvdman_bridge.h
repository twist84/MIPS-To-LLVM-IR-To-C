#pragma once

#include "iop_folder_sector_source.h"
#include "iop_kernel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iop
{
// Read-only CDVDMAN HLE over the real, validated extracted-disc source.
// Guest imports execute R3000 syscall stubs; importing does not read media,
// complete a CDVD command, or publish SIF readiness. Unimplemented commands
// remain unregistered and cannot fabricate DMA/interrupt completion.
class c_cdvdman_bridge
{
public:
    static constexpr uint32_t k_search_file_stub = 0x00018200u;
    static constexpr uint32_t k_search_file_syscall = 0x494f020au;
    static constexpr uint32_t k_service_base = 0x494f0200u;
    // Separate 0x19000..0x1997f range. The previous 0x18300 + ordinal*32
    // overlapped SIFMAN #5 at 0x187a0 (CDVDMAN #37) and #6 at 0x187c0
    // (CDVDMAN #38), overwriting *already linked* guest import stubs.
    // Keep the historical #10 search-file stub at 0x18200.
    // SIFCMD's last extra stub is below 0x18ea0; its RPC finish
    // trampoline is at 0x18f80. No guest module allocations start
    // until 0x20000.
    static constexpr uint32_t k_extra_stub_base = 0x00019000u;

    c_cdvdman_bridge(c_kernel& kernel, c_folder_sector_source& source);
    bool install();
    bool handle_syscall(c_kernel& kernel, uint32_t service);

private:
    static bool ram_pointer(uint32_t guest, uint32_t size,
                            uint32_t* physical);
    bool read_path(uint32_t pointer, std::string* path) const;
    bool handle_read_clock(uint32_t destination);
    void complete_read();

    c_kernel& m_kernel;
    c_folder_sector_source& m_source;
    bool m_read_pending = false;
    uint32_t m_read_destination = 0u;
    std::vector<uint8_t> m_read_data;
    std::vector<uint32_t> m_sync_waiters;
    uint32_t m_callback = 0u;
    uint32_t m_callback_gp = 0u;
    s_guest_callback_origin m_callback_origin{};
    uint32_t m_callback_registration_reports = 0u;
    uint32_t m_requested_media_mode = 0u;
    bool m_initialized = false;
    uint32_t m_last_error = 0u;
};
} // namespace iop
