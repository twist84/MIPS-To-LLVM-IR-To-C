#pragma once

#include "iop_disc_loader.h"
#include "iop_cdvd.h"
#include "iop_sifcmd_bridge.h"
#include "ee_dmac.h"
#include "ee_memory.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace iop
{
// Host-only explanation of a *rejected* real guest SIFRPC packet.
// This never authorizes HLE success or mutates the staged descriptor.
enum class e_rpc_rejection : uint8_t
{
    none = 0u,
    invalid_envelope,
    unknown_server,
    unsupported_method,
    unavailable_backend,
    invalid_arguments
};

// Bounded, explicit LOADFILE and IOPHEAP RPC services connected to guest-initialized
// IOP SIFCMD. Only actual EE SIF1 bind/call packets invoke this service.
// Outgoing replies require an EE-provided receive buffer and armed SIF0.
// No IOP firmware, SMFLAG readiness, or generic RPC dispatch is synthesized.
class c_sif_rpc_bridge
{
public:
    static constexpr uint32_t k_rpc_data_destination = 0xdeadbef0u;
    static constexpr uint32_t k_loadfile_server = 0x80000006u;
    static constexpr uint32_t k_fileio_server = 0x80000001u;
    static constexpr uint32_t k_iop_heap_server = 0x80000003u;
    static constexpr uint32_t k_cdvd_init_server = 0x80000592u;
    static constexpr uint32_t k_cdvd_s_server = 0x80000593u;
    static constexpr uint32_t k_cdvd_n_server = 0x80000595u;
    static constexpr uint32_t k_cdvd_stream_server = 0x80000596u;
    static constexpr uint32_t k_cdvd_search_server = 0x80000597u;
    static constexpr uint32_t k_cdvd_ready_server = 0x8000059au;
    static constexpr uint32_t k_cdvd_ready_alt_server = 0x8000059cu;
    static constexpr uint32_t k_bind_cid = 0x80000009u;
    static constexpr uint32_t k_call_cid = 0x8000000au;
    static constexpr uint32_t k_reply_cid = 0x80000008u;
    static constexpr uint32_t k_server_xor = 0xacacacacu;
    static constexpr uint32_t k_arkd_first_server = 0x500u;
    static constexpr uint32_t k_arkd_last_server = 0x503u;

    struct s_request
    {
        uint32_t m_words[16]{};
        uint32_t m_word_count = 0u;
        uint32_t m_result_destination = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_heap_argument = 0u;
        uint32_t m_heap_file_size = 0u;
        bool m_heap_file_found = false;
        std::vector<uint8_t> m_heap_file_bytes;
        uint32_t m_cdvd_lba = 0u;
        uint32_t m_cdvd_count = 0u;
        uint32_t m_cdvd_destination = 0u;
        uint32_t m_cdvd_stream_command = 0u;
        // sceCdRead (N fno 1) 24-byte send: IOP writes the unaligned
        // remainder struct and the read position back to these EE addresses.
        uint32_t m_cdvd_remainder_destination = 0u;
        uint32_t m_cdvd_readpos_destination = 0u;
        uint32_t m_cdvd_stream_position = 0u;
        uint32_t m_cdvd_lookup_lba = 0u;
        uint32_t m_cdvd_lookup_size = 0u;
        uint32_t m_payload_source = 0u;
        bool m_cdvd_to_iop = false;
        bool m_cdvd_lookup_found = false;
        bool m_guest_rpc = false;
        bool m_deferred_bind = false;
        std::vector<uint8_t> m_fileio_console_bytes;
        std::vector<uint8_t> m_guest_rpc_bytes;
        std::vector<uint8_t> m_cdvd_bytes;
        std::string m_guest_path;
        std::vector<std::string> m_module_arguments;
    };

    // Actual committed SIFRPC operations. This diagnostic is populated
    // only after a complete SIF1 descriptor list passes validation; it
    // never infers successful IRX startup from the static disc catalog.
    struct s_rpc_observation
    {
        uint64_t m_sequence = 0u;
        uint32_t m_cid = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_method = 0u;
        uint32_t m_client = 0u;
        uint32_t m_send_size = 0u;
        uint32_t m_recv_size = 0u;
        uint32_t m_return_word = 0u;
        e_disc_load_result m_load_status =
            e_disc_load_result::invalid_guest_path;
        e_irx_load_result m_link_status = e_irx_load_result::invalid_elf;
        uint32_t m_module_id = 0u;
        uint32_t m_thread_id = 0u;
        std::string m_unresolved_library;
        uint16_t m_unresolved_ordinal = 0u;
        uint32_t m_irx_entry_offset = 0u;
        uint32_t m_irx_gp_offset = 0u;
        uint32_t m_irx_image_size = 0u;
        bool m_module_started = false;
        std::string m_guest_path;
    };

    const s_rpc_observation& last_observation() const;

    c_sif_rpc_bridge(
        c_disc_loader& loader,
        c_kernel& kernel,
        c_sifcmd_bridge& sifcmd,
        ee::ee_memory& ee_ram,
        ee::c_ee_dmac& dmac,
        c_sector_source* sectors = nullptr
    );

    // No side effects: called during validation of the complete SIF1
    // descriptor list. Payload is EE RAM from the special SIF data DMA,
    // not an arbitrary host string or static ELF filename.
    bool prepare_command(
        const std::vector<uint32_t>& packet,
        uint32_t size,
        uint32_t payload_source,
        uint32_t payload_length,
        s_request* request,
        e_rpc_rejection* rejected_reason = nullptr
    ) const;

    static const char* rejection_name(e_rpc_rejection rejection);

    // Service execution occurs after SIF1 DMA staging/commit.
    void commit_command(const s_request& request);

    // Called only after a guest RPC server's R3000 handler returns
    // through the SIFCMD finish trampoline. Until then no SIF0 reply
    // or EE return-buffer write is manufactured.
    bool finish_guest_rpc(uint32_t token, uint32_t guest_result);
    // Pending binds can reply only after a real guest RPC registration.
    void on_guest_rpc_server_registered(uint32_t server_id);

    // Real IOP-originated BIND/CALL commands are copied into the EE's
    // armed SIF0 receiver. Optional payload is moved from IOP RAM to
    // the EE destination in that same accepted DMA delivery.
    bool queue_iop_command(
        const std::vector<uint32_t>& words,
        uint32_t ee_payload_destination = 0u,
        const std::vector<uint8_t>& payload = {}
    );
    // Actual IOP sceSifSendCmd packets, delivered through the same
    // armed SIF0 receiver as IOP-originated RPCs. Enqueueing is not
    // an acknowledgement from the EE and does not raise an IOP IRQ.
    bool queue_iop_guest_command(
        const std::vector<uint32_t>& words,
        uint32_t ee_payload_destination,
        const std::vector<uint8_t>& payload
    );

    // Invalidate outstanding RPC replies and CDVD streaming state
    // whenever the IOP kernel is actually reset.
    void reset_for_reboot();
    // Read LOADFILE's version from the actual on-disc reboot image.
    bool configure_reboot_image_version(const std::string& reboot_command);

    // The 16-byte EE INIT_CMD/opt=1 packet requests the original
    // IOP SET_SREG[0]=1 command. Reserve queue capacity during SIF1
    // validation; commit only after the entire batch is accepted.
    bool can_queue_replies(size_t count) const;
    void queue_init_sreg_reply();

    // Deliver at most one queued real service reply if SIF0 is armed.
    // Pending replies remain queued if the EE cannot accept them yet.
    // The EE can acknowledge DMA5 before the guest has copied its packet.
    void set_ee_receive_busy(bool busy);
    // The EE runner invokes this after every IOP turn. Keep the
    // empty-queue path visible to callers without requiring LTO.
    // Both queues matter: an initializer can produce a new reply.
    bool pump()
    {
        return (!m_pending_module_starts.empty() || !m_replies.empty()) &&
            pump_pending();
    }
    size_t pending_replies() const;
    // Calls pending a real R3000 SIFCMD finish trampoline.
    // A committed request alone never counts as a completed reply.
    size_t pending_guest_calls() const
    {
        return m_guest_pending.size();
    }
    // LOADFILE success is not a guest module start result until _start returns.
    size_t pending_module_starts() const;
    size_t pending_binds() const;
    // Advisory only. A raw DMA5 completion or an active EE receive
    // callback can own the mailbox even after all reply queues empty.
    bool scheduler_barrier() const
    {
        return m_ee_receive_busy || (m_dmac.status() & (1u << 5u)) != 0u ||
            !m_replies.empty() || !m_pending_module_starts.empty() ||
            !m_guest_pending.empty() || !m_deferred_binds.empty();
    }
    // A blocked EE bind permits a guest IOP scheduler turn only while
    // real R3000 threads or guest-owned timed work are runnable.
    bool has_waiting_bind_guest_work() const;
    uint32_t load_requests() const;

private:
    bool pump_pending();

    struct s_reply
    {
        uint32_t m_words[16]{};
        uint32_t m_word_count = 12u;
        uint32_t m_extra_destination = 0u;
        std::vector<uint8_t> m_extra_data;
        // True only for a reply backed by an actual guest _start return.
        bool m_module_start_reply = false;
        uint32_t m_module_start_id = 0u;
    };

    struct s_deferred_bind
    {
        uint32_t m_server_id = 0u;
        s_reply m_reply{};
    };

    struct s_guest_pending
    {
        s_reply m_reply{};
        uint32_t m_destination = 0u;
        uint32_t m_recv_size = 0u;
        uint32_t m_server_id = 0u;
        uint32_t m_method = 0u;
    };

    struct s_pending_module_start
    {
        s_reply m_reply{};
        uint32_t m_thread_id = 0u;
        uint32_t m_module_id = 0u;
        uint32_t m_destination = 0u;
        uint64_t m_sequence = 0u;
    };

    c_disc_loader& m_loader;
    c_kernel& m_kernel;
    c_sifcmd_bridge& m_sifcmd;
    ee::ee_memory& m_ee_ram;
    ee::c_ee_dmac& m_dmac;
    c_sector_source* m_sectors = nullptr;
    bool m_cdvd_streaming = false;
    bool m_loadfile_version_known = false;
    uint8_t m_loadfile_version[4]{};
    uint32_t m_cdvd_stream_position = 0u;
    std::deque<s_reply> m_replies;
    std::deque<s_deferred_bind> m_deferred_binds;
    std::deque<s_pending_module_start> m_pending_module_starts;
    std::map<uint32_t, s_guest_pending> m_guest_pending;
    uint32_t m_next_guest_token = 1u;
    uint32_t m_load_requests = 0u;
    uint64_t m_total_rpc_requests = 0u;
    s_rpc_observation m_last_observation{};
    bool m_ee_receive_busy = false;
};
} // namespace iop
