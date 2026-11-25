#include "iop_sif_rpc_bridge.h"
#include "iop_folder_sector_source.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <sstream>
#include <limits>
#include <utility>

namespace iop
{
namespace
{
constexpr uint32_t k_loadfile_failure = 0xffffffffu;

bool normalize_ee_ram(uint32_t pointer, uint32_t* physical)
{
    if (physical == nullptr)
    {
        return false;
    }
    if (pointer < 0x02000000u)
    {
        *physical = pointer;
        return true;
    }
    if ((pointer >= 0x80000000u && pointer < 0xc0000000u) ||
        ((pointer >> 28u) == 2u || (pointer >> 28u) == 3u))
    {
        *physical = pointer & 0x1fffffffu;
        return *physical < 0x02000000u;
    }
    return false;
}

bool read_guest_path(
    ee::ee_memory& memory, uint32_t buffer,
    uint32_t size, std::string* output,
    std::vector<std::string>* arguments
)
{
    // PS2SDK _lf_module_load_arg, exactly 512 bytes in retail:
    // +0 arg_len (0..252), +4 modres, +8 path[252],
    // +260 args[252]. A short zero-argument test envelope remains
    // valid, but nonempty arguments require their staged bytes.
    if (output == nullptr || arguments == nullptr || size < 260u ||
        !memory.contains(buffer, size))
    {
        return false;
    }

    const uint32_t argument_bytes = memory.read32(buffer);
    if (argument_bytes > 252u ||
        argument_bytes > size - 260u)
    {
        return false;
    }
    std::string path;
    bool terminated = false;
    for (uint32_t index = 0u; index < 252u; ++index)
    {
        const uint8_t ch = memory.read8(buffer + 8u + index);
        if (ch == 0u)
        {
            terminated = true;
            break;
        }
        if (ch < 0x20u || ch > 0x7eu)
        {
            return false;
        }
        path.push_back(static_cast<char>(ch));
    }
    if (!terminated || path.empty())
    {
        return false;
    }

    std::vector<std::string> parsed;
    std::string word;
    for (uint32_t index = 0u; index < argument_bytes; ++index)
    {
        const uint8_t ch = memory.read8(buffer + 260u + index);
        if (ch == 0u)
        {
            parsed.push_back(std::move(word));
            word.clear();
        }
        else
        {
            // Preserve the caller's literal module arguments. They
            // are delivered as guest strings, not interpreted by HLE.
            word.push_back(static_cast<char>(ch));
        }
    }
    if (!word.empty() ||
        (argument_bytes != 0u &&
         memory.read8(buffer + 260u + argument_bytes - 1u) != 0u))
    {
        // Partial final argument would leak beyond the DMA payload.
        return false;
    }
    *output = std::move(path);
    *arguments = std::move(parsed);
    return true;
}
} // namespace

c_sif_rpc_bridge::c_sif_rpc_bridge(
    c_disc_loader& loader, c_kernel& kernel,
    c_sifcmd_bridge& sifcmd, ee::ee_memory& ee_ram,
    ee::c_ee_dmac& dmac, c_sector_source* sectors
)
    : m_loader(loader), m_kernel(kernel), m_sifcmd(sifcmd),
      m_ee_ram(ee_ram), m_dmac(dmac), m_sectors(sectors)
{
    m_sifcmd.bind_rpc_bridge(this);
}

bool c_sif_rpc_bridge::configure_reboot_image_version(
    const std::string& reboot_command
)
{
    m_loadfile_version_known = false;
    std::fill(std::begin(m_loadfile_version),
              std::end(m_loadfile_version), uint8_t{0});

    const auto* folder = dynamic_cast<c_folder_sector_source*>(m_sectors);
    if (folder == nullptr)
    {
        return false;
    }
    std::istringstream arguments(reboot_command);
    std::string loader;
    std::string path;
    if (!(arguments >> loader))
    {
        return false;
    }
    while (arguments >> path)
    {
        if (path != "-v")
        {
            break;
        }
    }
    s_folder_file_info image{};
    constexpr uint32_t k_max_image = 16u * 1024u * 1024u;
    if (path.empty() || !folder->find_file(path, &image) ||
        image.m_size < 16u || image.m_size > k_max_image)
    {
        return false;
    }
    const uint32_t count =
        (image.m_size + c_iso_sector_source::k_sector_size - 1u) /
        c_iso_sector_source::k_sector_size;
    std::vector<uint8_t> bytes;
    if (!m_sectors->read_sectors(image.m_lba, count, bytes) ||
        bytes.size() < image.m_size)
    {
        return false;
    }
    static constexpr char k_marker[] = "PsIIloadfile";
    const auto end = bytes.begin() + image.m_size;
    const auto marker = std::search(
        bytes.begin(), end, std::begin(k_marker), std::end(k_marker) - 1);
    if (marker == end ||
        static_cast<size_t>(end - marker) < sizeof(k_marker) - 1u + 4u)
    {
        return false;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const uint8_t digit = marker[sizeof(k_marker) - 1u + index];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        m_loadfile_version[index] = digit;
    }
    m_loadfile_version_known = true;
    return true;
}

const char* c_sif_rpc_bridge::rejection_name(e_rpc_rejection reason)
{
    switch (reason)
    {
    case e_rpc_rejection::none: return "none";
    case e_rpc_rejection::invalid_envelope: return "invalid_envelope";
    case e_rpc_rejection::unknown_server: return "unknown_server";
    case e_rpc_rejection::unsupported_method: return "unsupported_method";
    case e_rpc_rejection::unavailable_backend: return "unavailable_backend";
    case e_rpc_rejection::invalid_arguments: return "invalid_arguments";
    }
    return "invalid_reason";
}

bool c_sif_rpc_bridge::prepare_command(
    const std::vector<uint32_t>& packet,
    uint32_t size,
    uint32_t payload_source,
    uint32_t payload_length,
    s_request* request,
    e_rpc_rejection* rejected_reason
) const
{
    // Local diagnostic only. No staged guest DMA or reply can be committed
    // by this validator, even when it classifies an unsupported request.
    const auto refuse = [rejected_reason, &packet, size, payload_source,
                         payload_length](e_rpc_rejection reason) -> bool
    {
        if (rejected_reason != nullptr)
        {
            *rejected_reason = reason;
        }
        // SDBZ_TRACE_RPC_REJECT: dump the refused SIF command words.
        static const bool trace = std::getenv("SDBZ_TRACE_RPC_REJECT") != nullptr;
        if (trace)
        {
            std::fprintf(stderr,
                "IOP RPC refused: reason=%s size=%u payload=0x%08x len=%u words:",
                rejection_name(reason), size, payload_source, payload_length);
            for (size_t i = 0; i < packet.size() && i < 16u; ++i)
            {
                std::fprintf(stderr, " %08x", packet[i]);
            }
            std::fprintf(stderr, "\n");
        }
        return false;
    };
    if (rejected_reason != nullptr)
    {
        *rejected_reason = e_rpc_rejection::invalid_envelope;
    }
    if (!m_sifcmd.initialized() ||
        request == nullptr ||
        m_replies.size() + m_guest_pending.size() +
            m_deferred_binds.size() +
            m_pending_module_starts.size() >= 64u ||
        packet.size() < size / 4u ||
        (size != 36u && size != 56u && size != 64u) ||
        (packet[0] & 0xffu) != size ||
        // sifrpc uses DEADBEF0 as its IOP-side data destination.
        // Earlier synthetic packets also used zero. Neither header
        // form licenses an arbitrary IOP destination.
        (packet[1] != 0u &&
         packet[1] != k_rpc_data_destination) ||
        // The original SDBZ EE sends SIFRPC CALL with header.opt=1
        // during the CDVD RTC query (0x80000593 method 1). The
        // ps2-web-recomp/SDBZ SIF RPC dispatcher does not require zero
        // for CALL header.opt. Accept only the observed option 1 for
        // CALL packets; BIND and unknown option bits remain rejected.
        (packet[3] != 0u &&
         !(packet[2] == k_call_cid && packet[3] == 1u)) ||
        packet[4] == 0u || packet[5] == 0u ||
        packet[6] == 0u || packet[7] == 0u)
    {
        return refuse(e_rpc_rejection::invalid_envelope);
    }

    s_request candidate{};
    candidate.m_word_count = size / 4u;
    for (uint32_t index = 0u; index < candidate.m_word_count; ++index)
    {
        candidate.m_words[index] = packet[index];
    }
    const uint32_t cid = packet[2];
    // Standard EE sifrpc also emits a 64-byte bind packet. The
    // first 36 bytes carry the same header and server identity as
    // the shorter form (as decoded by ps2-web-recomp's bind()).
    // The remaining bytes are reserved/padding, not a second RPC.
    if (cid == k_bind_cid && (size == 36u || size == 64u))
    {
        // CDVDFSV's CdInit server is a built-in HLE service,
        // as in ps2-web-recomp/SDBZ. This only binds the client;
        // unsupported CDVD RPC methods must still fail closed.
        const bool cdvd_server =
            packet[8] == k_cdvd_s_server ||
            packet[8] == k_cdvd_n_server ||
            packet[8] == k_cdvd_stream_server ||
            packet[8] == k_cdvd_search_server ||
            packet[8] == k_cdvd_ready_server ||
            packet[8] == k_cdvd_ready_alt_server;
        // The retail Arika RPC IDs 0x500..0x503 belong to guest IOP
        // servers. A BIND may precede their genuine registration.
        const bool deferred_guest_server =
            packet[8] >= k_arkd_first_server &&
            packet[8] <= k_arkd_last_server &&
            !m_sifcmd.has_guest_rpc_server(packet[8]);
        if (packet[8] != k_loadfile_server &&
            packet[8] != k_iop_heap_server &&
            packet[8] != k_cdvd_init_server && !cdvd_server &&
            !m_sifcmd.has_guest_rpc_server(packet[8]) &&
            !deferred_guest_server)
        {
            return refuse(e_rpc_rejection::unknown_server);
        }
        if (cdvd_server &&
            (m_sectors == nullptr || m_sectors->sector_count() == 0u))
        {
            return refuse(e_rpc_rejection::unavailable_backend);
        }
        candidate.m_server_id = packet[8];
        candidate.m_deferred_bind = deferred_guest_server;
        *request = std::move(candidate);
        if (rejected_reason != nullptr)
        {
            *rejected_reason = e_rpc_rejection::none;
        }
        return true;
    }

    if (cid != k_call_cid)
    {
        return refuse(e_rpc_rejection::invalid_envelope);
    }
    if ((size != 56u && size != 64u) ||
        packet[9] > 4096u ||
        packet[11] > 4096u ||
        packet[12] > 1u ||
        payload_length < packet[9])
    {
        return refuse(e_rpc_rejection::invalid_arguments);
    }

    const uint32_t server_id = packet[13] ^ k_server_xor;
    const uint32_t method = packet[8];
    uint32_t result = 0u;
    if (!normalize_ee_ram(packet[10], &result))
    {
        return refuse(e_rpc_rejection::invalid_arguments);
    }
    candidate.m_server_id = server_id;
    candidate.m_result_destination = result;
    if (server_id == k_loadfile_server)
    {
        if (method != 0xffu && method != 0u)
        {
            return refuse(e_rpc_rejection::unsupported_method);
        }
        if (method == 0xffu)
        {
            // The retail post-reboot request carries no payload.
            // Only verified IOPRP metadata is returned.
            if (!m_loadfile_version_known || packet[9] != 0u ||
                packet[11] < 4u ||
                !m_ee_ram.contains(result, packet[11]))
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
        }
        else if (method != 0u || packet[9] < 260u ||
                 packet[11] < 8u ||
                 !m_ee_ram.contains(result, 8u) ||
                 !m_ee_ram.contains(payload_source, packet[9]) ||
                 !read_guest_path(
                     m_ee_ram, payload_source, packet[9],
                     &candidate.m_guest_path,
                     &candidate.m_module_arguments))
        {
            return refuse(e_rpc_rejection::invalid_arguments);
        }
    }
    else if (server_id == k_cdvd_init_server)
    {
        if (method != 0u)
        {
            return refuse(e_rpc_rejection::unsupported_method);
        }
        // Retail SDBZ calls CdInit method 0. The observed word[4] = 5
        // is the RPC record ID, not the method (which is word[8]).
        // The reference HLE accepts a bounded receive size. Only a
        // buffer of at least sixteen bytes receives result[3] = 0xff.
        // Other methods remain unsupported until independently verified.
        if (method != 0u ||
            !m_ee_ram.contains(result, packet[11]) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return refuse(e_rpc_rejection::invalid_arguments);
        }
    }
    else if (server_id == k_iop_heap_server)
    {
        if (method < 1u || method > 7u)
        {
            return refuse(e_rpc_rejection::unsupported_method);
        }
        // IOPHEAP 1/2/4/5/6/7 share the real kernel allocator.
        // Method 3 transfers actual mounted file bytes to a live
        // IOPHEAP buffer, rather than writing kernel or IRX memory.
        const bool has_argument =
            method == 1u || method == 2u || method == 3u ||
            method == 4u;
        if ((method < 1u || method > 7u) ||
            (has_argument && packet[9] <
                (method == 3u ? 6u : (method == 4u ? 12u : 4u))) ||
            packet[11] < 4u ||
            !m_ee_ram.contains(result, 4u) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return refuse(e_rpc_rejection::invalid_arguments);
        }
        if (has_argument)
        {
            candidate.m_heap_argument = m_ee_ram.read32(payload_source);
        }
        if (method == 3u)
        {
            const auto* folder =
                dynamic_cast<c_folder_sector_source*>(m_sectors);
            if (folder == nullptr)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            const uint32_t region =
                candidate.m_heap_argument & 0xe0000000u;
            if (region != 0u && region != 0x80000000u &&
                region != 0xa0000000u)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            candidate.m_heap_argument &= 0x1fffffffu;
            std::string path;
            bool terminated = false;
            for (uint32_t index = 4u; index < packet[9]; ++index)
            {
                const uint8_t ch =
                    m_ee_ram.read8(payload_source + index);
                if (ch == 0u)
                {
                    terminated = true;
                    break;
                }
                if (ch < 0x21u || ch > 0x7eu ||
                    path.size() >= 255u)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                path.push_back(static_cast<char>(ch));
            }
            if (!terminated || path.size() < 8u ||
                path.find("..") != std::string::npos)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            s_folder_file_info file{};
            if (!folder->find_file(path, &file))
            {
                // Legitimate filesystem miss: reply -2, not success.
                candidate.m_heap_file_found = false;
            }
            else
            {
                const uint32_t count =
                    (file.m_size + c_iso_sector_source::k_sector_size -
                     1u) / c_iso_sector_source::k_sector_size;
                if (file.m_size == 0u ||
                    !m_kernel.owns_heap_span(
                        candidate.m_heap_argument, file.m_size) ||
                    static_cast<uint64_t>(file.m_lba) + count >
                        m_sectors->sector_count() ||
                    !m_sectors->read_sectors(
                        file.m_lba, count, candidate.m_heap_file_bytes) ||
                    candidate.m_heap_file_bytes.size() < file.m_size)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                candidate.m_heap_file_size = file.m_size;
                candidate.m_heap_file_bytes.resize(file.m_size);
                candidate.m_heap_file_found = true;
            }
        }
    }
    else if (server_id == k_cdvd_s_server ||
             server_id == k_cdvd_n_server ||
             server_id == k_cdvd_stream_server ||
             server_id == k_cdvd_search_server ||
             server_id == k_cdvd_ready_server ||
             server_id == k_cdvd_ready_alt_server)
    {
        // These HLE services exist only when actual disc media is mounted.
        // Every request is fully validated before SIF1 changes guest RAM.
        if (m_sectors == nullptr || m_sectors->sector_count() == 0u)
        {
            return refuse(e_rpc_rejection::unavailable_backend);
        }
        if (!m_ee_ram.contains(result, packet[11]) ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return refuse(e_rpc_rejection::invalid_arguments);
        }
        candidate.m_payload_source = payload_source;
        const uint32_t reply_size = packet[11];
        if (server_id == k_cdvd_s_server)
        {
            uint32_t needed = 4u;
            switch (method)
            {
            case 0x01u: needed = 12u; break;
            case 0x05u: needed = 8u; break;
            case 0x27u:
                // This single-layer directory model cannot report a
                // fabricated DVD second-layer boundary.
                if (dynamic_cast<c_folder_sector_source*>(m_sectors) ==
                    nullptr)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                needed = 12u;
                break;
            case 0x03u:
            case 0x04u:
            case 0x0cu:
            case 0x16u:
            case 0x22u:
                break;
            default: return refuse(e_rpc_rejection::unsupported_method);
            }
            if (reply_size < needed)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
        }
        else if (server_id == k_cdvd_n_server)
        {
            uint32_t needed = 0u;
            bool read_media = false;
            switch (method)
            {
            case 0x01u:
            case 0x0du:
                if (packet[9] < 12u)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                candidate.m_cdvd_lba = m_ee_ram.read32(payload_source);
                candidate.m_cdvd_count =
                    m_ee_ram.read32(payload_source + 4u);
                candidate.m_cdvd_destination =
                    m_ee_ram.read32(payload_source + 8u);
                candidate.m_cdvd_to_iop = method == 0x0du;
                // libcdvd sceCdRead is SIF_RPC_M_NOWAIT with a NULL/0
                // receive buffer (ps2sdk ee/rpc/cdvd/src/ncmd.c); only a
                // caller that supplies a reply buffer gets the result word.
                needed = reply_size == 0u ? 0u : 4u;
                if (method == 0x01u && packet[9] >= 24u)
                {
                    uint32_t remainder = 0u;
                    uint32_t readpos = 0u;
                    const uint32_t remainder_guest =
                        m_ee_ram.read32(payload_source + 16u);
                    const uint32_t readpos_guest =
                        m_ee_ram.read32(payload_source + 20u);
                    if (remainder_guest != 0u &&
                        normalize_ee_ram(remainder_guest, &remainder) &&
                        m_ee_ram.contains(remainder, 16u))
                    {
                        candidate.m_cdvd_remainder_destination = remainder;
                    }
                    if (readpos_guest != 0u &&
                        normalize_ee_ram(readpos_guest, &readpos) &&
                        m_ee_ram.contains(readpos, 4u))
                    {
                        candidate.m_cdvd_readpos_destination = readpos;
                    }
                }
                read_media = true;
                break;
            case 0x09u:
                if (packet[9] < 16u)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                candidate.m_cdvd_lba = m_ee_ram.read32(payload_source);
                candidate.m_cdvd_count =
                    m_ee_ram.read32(payload_source + 4u);
                candidate.m_cdvd_destination =
                    m_ee_ram.read32(payload_source + 8u);
                candidate.m_cdvd_stream_command =
                    m_ee_ram.read32(payload_source + 12u);
                if (candidate.m_cdvd_stream_command == 2u)
                {
                    candidate.m_cdvd_lba = m_cdvd_stream_position;
                    read_media = true;
                }
                else if (candidate.m_cdvd_stream_command != 1u &&
                         candidate.m_cdvd_stream_command != 3u &&
                         candidate.m_cdvd_stream_command != 4u &&
                         candidate.m_cdvd_stream_command != 5u &&
                         candidate.m_cdvd_stream_command != 6u &&
                         candidate.m_cdvd_stream_command != 9u)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                needed = 4u;
                break;
            case 0x04u:
            case 0x0eu:
                needed = 4u;
                break;
            case 0x05u:
            case 0x08u:
                break;
            default: return refuse(e_rpc_rejection::unsupported_method);
            }
            if (reply_size < needed)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            if (read_media)
            {
                const uint64_t bytes =
                    static_cast<uint64_t>(candidate.m_cdvd_count) *
                    c_iso_sector_source::k_sector_size;
                if (candidate.m_cdvd_count == 0u ||
                    bytes > c_cpu::k_ram_size ||
                    static_cast<uint64_t>(candidate.m_cdvd_lba) +
                        candidate.m_cdvd_count > m_sectors->sector_count())
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                uint32_t destination = candidate.m_cdvd_destination;
                if (candidate.m_cdvd_to_iop)
                {
                    const uint32_t region = destination & 0xe0000000u;
                    if ((region != 0u && region != 0x80000000u &&
                         region != 0xa0000000u) ||
                        (destination & 0x1fffffffu) >
                            c_cpu::k_ram_size - bytes)
                    {
                        return refuse(e_rpc_rejection::invalid_arguments);
                    }
                }
                else if (!normalize_ee_ram(destination, &destination) ||
                         !m_ee_ram.contains(
                             destination, static_cast<size_t>(bytes)))
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                candidate.m_cdvd_destination = destination;
                if (!m_sectors->read_sectors(
                        candidate.m_cdvd_lba, candidate.m_cdvd_count,
                        candidate.m_cdvd_bytes) ||
                    candidate.m_cdvd_bytes.size() != bytes)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
            }
        }
        else if (server_id == k_cdvd_search_server)
        {
            const auto* folder =
                dynamic_cast<c_folder_sector_source*>(m_sectors);
            const uint32_t offset =
                packet[9] == 0x124u ? 0x20u : 0x24u;
            if (folder == nullptr || packet[9] <= offset ||
                reply_size < 4u)
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            std::string path;
            bool terminated = false;
            for (uint32_t index = offset; index < packet[9]; ++index)
            {
                const uint8_t ch = m_ee_ram.read8(
                    payload_source + index);
                if (ch == 0u)
                {
                    terminated = true;
                    break;
                }
                if (ch < 0x20u || ch > 0x7eu ||
                    path.size() >= 255u)
                {
                    return refuse(e_rpc_rejection::invalid_arguments);
                }
                path.push_back(static_cast<char>(ch));
            }
            if (!terminated || path.empty())
            {
                return refuse(e_rpc_rejection::invalid_arguments);
            }
            const std::string qualified =
                path.size() >= 7u &&
                (path.substr(0u, 7u) == "cdrom0:" ||
                 path.substr(0u, 7u) == "CDROM0:")
                    ? path : "cdrom0:" + path;
            s_folder_file_info file{};
            candidate.m_cdvd_lookup_found =
                folder->find_file(qualified, &file);
            if (candidate.m_cdvd_lookup_found)
            {
                candidate.m_cdvd_lookup_lba = file.m_lba;
                candidate.m_cdvd_lookup_size = file.m_size;
            }
        }
        else if (server_id == k_cdvd_stream_server)
        {
            // Reference 0x80000596 method 1 holds the RPC open;
            // scheduling that operation is not available yet.
            if (method == 1u)
            {
                return refuse(e_rpc_rejection::unavailable_backend);
            }
        }
        else if (reply_size < 4u)
        {
            return refuse(e_rpc_rejection::invalid_arguments);
        }
    }
    else if (m_sifcmd.has_guest_rpc_server(server_id))
    {
        if (!m_sifcmd.can_queue_guest_rpc(server_id, packet[9]) ||
            !m_ee_ram.contains(result, packet[11]) ||
            m_guest_pending.size() + m_replies.size() >= 64u ||
            (packet[9] != 0u &&
             !m_ee_ram.contains(payload_source, packet[9])))
        {
            return refuse(e_rpc_rejection::unavailable_backend);
        }
        candidate.m_guest_rpc = true;
        candidate.m_guest_rpc_bytes.reserve(packet[9]);
        for (uint32_t i = 0u; i < packet[9]; ++i)
        {
            candidate.m_guest_rpc_bytes.push_back(
                m_ee_ram.read8(payload_source + i));
        }
    }
    else
    {
        return refuse(e_rpc_rejection::unknown_server);
    }
    *request = std::move(candidate);
    if (rejected_reason != nullptr)
    {
        *rejected_reason = e_rpc_rejection::none;
    }
    return true;
}

const c_sif_rpc_bridge::s_rpc_observation&
c_sif_rpc_bridge::last_observation() const
{
    return m_last_observation;
}

void c_sif_rpc_bridge::commit_command(const s_request& request)
{
    // Track the actual committed packet; a static ELF path is not
    // evidence that the IOP loaded or executed a module.
    s_rpc_observation observed{};
    observed.m_sequence = ++m_total_rpc_requests;
    observed.m_cid = request.m_words[2];
    observed.m_server_id = request.m_server_id;
    observed.m_client = request.m_words[7];
    if (observed.m_cid == k_call_cid)
    {
        observed.m_method = request.m_words[8];
        observed.m_send_size = request.m_words[9];
        observed.m_recv_size = request.m_words[11];
    }
    observed.m_guest_path = request.m_guest_path;
    s_reply response{};
    // SIFCMD Rend: 48-byte header, echoed guest transaction identity,
    // and the command serviced. No reply is queued before real request
    // validation and LOADFILE backend execution.
    response.m_words[0] = 48u;
    response.m_words[1] = request.m_words[1];
    response.m_words[2] = k_reply_cid;
    response.m_words[3] = 0u;
    response.m_words[4] = request.m_words[4];
    response.m_words[5] = request.m_words[5];
    response.m_words[6] = request.m_words[6];
    response.m_words[7] = request.m_words[7];
    response.m_words[8] = request.m_words[2];

    if (request.m_guest_rpc &&
        request.m_words[2] == k_call_cid)
    {
        const uint32_t token = m_next_guest_token++;
        m_guest_pending.emplace(token, s_guest_pending{
            response, request.m_result_destination, request.m_words[11],
            request.m_server_id, request.m_words[8]});
        if (!m_sifcmd.enqueue_guest_rpc(
                token, request.m_server_id, request.m_words[8],
                request.m_guest_rpc_bytes))
        {
            m_guest_pending.erase(token);
            return;
        }
        // Only the actual R3000 handler's finish trampoline may
        // publish a reply or change the EE receive destination.
        m_last_observation = std::move(observed);
        return;
    }

    if (request.m_words[2] == k_bind_cid)
    {
        response.m_words[9] = request.m_server_id ^ k_server_xor;
        response.m_words[10] = k_rpc_data_destination;
        response.m_words[11] = 0xdeadcafeu;
        if (request.m_deferred_bind)
        {
            // Accept the real SIF1 transfer but do not fabricate
            // registration, a SIF0 Rend or a DMA completion.
            m_deferred_binds.push_back({request.m_server_id, response});
            m_last_observation = std::move(observed);
            std::fprintf(stderr,
                "IOP HLE RPC bind pending: server=0x%08x client=0x%08x\n",
                request.m_server_id, request.m_words[7]);
            return;
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_loadfile_server)
    {
        if (request.m_words[8] == 0xffu)
        {
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                m_ee_ram.write8(request.m_result_destination + index,
                                m_loadfile_version[index]);
            }
        }
        else
        {
            ++m_load_requests;
            const s_disc_load_result loaded = m_loader.load_requested(
                request.m_guest_path, true,
                c_kernel::k_default_module_priority,
                request.m_module_arguments
            );
            observed.m_load_status = loaded.m_result;
            observed.m_link_status = loaded.m_link_status;
            observed.m_module_id = loaded.m_module_id;
            observed.m_thread_id = loaded.m_thread_id;
            observed.m_unresolved_library =
                loaded.m_link_diagnostic.m_unresolved_library;
            observed.m_unresolved_ordinal =
                loaded.m_link_diagnostic.m_unresolved_ordinal;
            observed.m_irx_entry_offset =
                loaded.m_link_diagnostic.m_entry_offset;
            observed.m_irx_gp_offset =
                loaded.m_link_diagnostic.m_gp_offset;
            observed.m_irx_image_size =
                loaded.m_link_diagnostic.m_image_size;
            observed.m_module_started =
                loaded.m_result == e_disc_load_result::success &&
                loaded.m_thread_id != 0u;
            if (loaded.m_result == e_disc_load_result::success &&
                loaded.m_thread_id != 0u)
            {
                // The original guest _start must actually return.
                // Scheduling it is not permission to answer LOADFILE.
                std::fprintf(stderr,
                    "IOP LOADFILE start queued: module=%u thread=%u "
                    "sequence=%llu\n",
                    loaded.m_module_id, loaded.m_thread_id,
                    static_cast<unsigned long long>(observed.m_sequence));
                m_pending_module_starts.push_back(s_pending_module_start{
                    response, loaded.m_thread_id, loaded.m_module_id,
                    request.m_result_destination, observed.m_sequence
                });
                m_last_observation = std::move(observed);
                return;
            }
            m_ee_ram.write32(request.m_result_destination, k_loadfile_failure);
            m_ee_ram.write32(request.m_result_destination + 4u, k_loadfile_failure);
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_init_server)
    {
        // CdInit always completes, but only updates result[3] if
        // the caller provided at least sixteen receive bytes.
        // This does not mark disc hardware ready or start IRX code.
        if (request.m_words[11] >= 16u)
        {
            m_ee_ram.write32(request.m_result_destination + 12u, 0xffu);
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_s_server)
    {
        const uint32_t destination = request.m_result_destination;
        const uint32_t method = request.m_words[8];
        if (method == 0x01u)
        {
            const std::time_t current = std::time(nullptr);
            const std::tm* clock = std::gmtime(&current);
            if (clock != nullptr)
            {
                const auto bcd = [](int number) -> uint8_t
                {
                    return static_cast<uint8_t>(
                        ((number / 10) << 4) | (number % 10));
                };
                m_ee_ram.write32(destination, 1u);
                const uint8_t bytes[8] = {
                    0u, bcd(clock->tm_sec), bcd(clock->tm_min),
                    bcd((clock->tm_hour + 9) % 24), 0u,
                    bcd(clock->tm_mday), bcd(clock->tm_mon + 1),
                    bcd(clock->tm_year % 100)
                };
                for (uint32_t index = 0u; index < 8u; ++index)
                {
                    m_ee_ram.write8(destination + 4u + index, bytes[index]);
                }
            }
        }
        else
        {
            uint32_t value = 1u;
            switch (method)
            {
            case 0x03u: value = 0x14u; break;
            case 0x04u: value = 0u; break;
            case 0x0cu: value = m_cdvd_streaming ? 0x12u : 0x0au; break;
            default: break;
            }
            m_ee_ram.write32(destination, value);
            if (method == 0x05u || method == 0x27u)
            {
                m_ee_ram.write32(destination + 4u, 0u);
            }
            if (method == 0x27u)
            {
                m_ee_ram.write32(destination + 8u, 0u);
            }
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_n_server)
    {
        const uint32_t method = request.m_words[8];
        if (!request.m_cdvd_bytes.empty())
        {
            if (request.m_cdvd_to_iop)
            {
                (void)m_kernel.cpu().dma_write(
                    request.m_cdvd_destination,
                    request.m_cdvd_bytes.data(),
                    static_cast<uint32_t>(request.m_cdvd_bytes.size()));
            }
            else
            {
                for (size_t index = 0u;
                     index < request.m_cdvd_bytes.size(); ++index)
                {
                    m_ee_ram.write8(
                        request.m_cdvd_destination +
                            static_cast<uint32_t>(index),
                        request.m_cdvd_bytes[index]);
                }
            }
        }
        switch (method)
        {
        case 0x01u:
        case 0x0du:
        {
            static const bool trace_cdvd =
                std::getenv("SDBZ_TRACE_CDVD") != nullptr;
            if (trace_cdvd)
            {
                std::fprintf(stderr,
                    "IOP CDVDFSV read: fno=%u lba=%u sectors=%u dst=0x%08x "
                    "recv=%u\n", method, request.m_cdvd_lba,
                    request.m_cdvd_count, request.m_cdvd_destination,
                    request.m_words[11]);
            }
        }
            if (request.m_words[11] != 0u)
            {
                m_ee_ram.write32(request.m_result_destination, 0u);
            }
            if (request.m_cdvd_remainder_destination != 0u)
            {
                // The whole transfer already landed in the EE buffer, so
                // the head/tail fix-up lengths and pointers are zero.
                for (uint32_t offset = 0u; offset < 16u; offset += 4u)
                {
                    m_ee_ram.write32(
                        request.m_cdvd_remainder_destination + offset, 0u);
                }
            }
            if (request.m_cdvd_readpos_destination != 0u)
            {
                m_ee_ram.write32(request.m_cdvd_readpos_destination,
                    request.m_cdvd_count *
                        c_iso_sector_source::k_sector_size);
            }
            break;
        case 0x04u:
            m_ee_ram.write32(request.m_result_destination, 1u);
            break;
        case 0x09u:
            switch (request.m_cdvd_stream_command)
            {
            case 1u:
                m_cdvd_stream_position = request.m_cdvd_lba;
                m_cdvd_streaming = true;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 2u:
                m_cdvd_stream_position += request.m_cdvd_count;
                m_ee_ram.write32(
                    request.m_result_destination, request.m_cdvd_count);
                break;
            case 3u:
                m_cdvd_streaming = false;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 4u:
            case 9u:
                m_cdvd_stream_position = request.m_cdvd_lba;
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 5u:
                m_ee_ram.write32(request.m_result_destination, 1u);
                break;
            case 6u:
                m_ee_ram.write32(
                    request.m_result_destination, request.m_cdvd_lba);
                break;
            default: break;
            }
            break;
        case 0x0eu:
            m_ee_ram.write32(request.m_result_destination, 2u);
            break;
        default:
            // 0x05 has no output; 0x08 completes its own RPC.
            break;
        }
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_cdvd_search_server)
    {
        if (request.m_cdvd_lookup_found)
        {
            m_ee_ram.write32(
                request.m_payload_source, request.m_cdvd_lookup_lba);
            m_ee_ram.write32(
                request.m_payload_source + 4u, request.m_cdvd_lookup_size);
        }
        m_ee_ram.write32(
            request.m_result_destination,
            request.m_cdvd_lookup_found ? 1u : 0u);
    }
    else if (request.m_words[2] == k_call_cid &&
             (request.m_server_id == k_cdvd_ready_server ||
              request.m_server_id == k_cdvd_ready_alt_server))
    {
        m_ee_ram.write32(request.m_result_destination, 2u);
    }
    else if (request.m_words[2] == k_call_cid &&
             request.m_server_id == k_iop_heap_server)
    {
        uint32_t result = 0u;
        switch (request.m_words[8])
        {
        case 3u:
            if (request.m_heap_file_found)
            {
                const bool copied = m_kernel.cpu().dma_write(
                    request.m_heap_argument,
                    request.m_heap_file_bytes.data(),
                    request.m_heap_file_size);
                result = copied ? 0u : static_cast<uint32_t>(-1);
            }
            else
            {
                result = static_cast<uint32_t>(-2);
            }
            break;
        case 1u:
        case 4u:
            result = m_kernel.allocate_heap(request.m_heap_argument);
            if (result == 0u)
            {
                // A legitimate guest allocation failure is not a reason
                // to invent IOP RAM. Log the real allocator's available
                // spans so retail failures such as PS2RNA (99,904 bytes)
                // can be distinguished from an RPC protocol failure.
                std::fprintf(stderr,
                    "IOPHEAP allocation failed: method=%u requested=%u "
                    "largest_free=%u total_free=%u result=0\n",
                    request.m_words[8], request.m_heap_argument,
                    m_kernel.heap_max_free(),
                    m_kernel.heap_total_free());
            }
            break;
        case 2u:
            result = m_kernel.free_heap(request.m_heap_argument)
                ? 0u : static_cast<uint32_t>(-1);
            break;
        case 5u:
            result = c_cpu::k_ram_size;
            break;
        case 6u:
            result = m_kernel.heap_max_free();
            break;
        case 7u:
            result = m_kernel.heap_total_free();
            break;
        default:
            // Unreachable: prepare_command refuses unimplemented methods.
            result = static_cast<uint32_t>(-1);
            break;
        }
        m_ee_ram.write32(request.m_result_destination, result);
    }
    if (observed.m_cid == k_call_cid &&
        observed.m_recv_size >= 4u &&
        m_ee_ram.contains(request.m_result_destination, 4u))
    {
        observed.m_return_word =
            m_ee_ram.read32(request.m_result_destination);
    }
    m_last_observation = std::move(observed);
    m_replies.push_back(response);
    (void)pump();
}

bool c_sif_rpc_bridge::queue_iop_command(
    const std::vector<uint32_t>& words,
    uint32_t ee_payload_destination,
    const std::vector<uint8_t>& payload
)
{
    if (!m_sifcmd.initialized() ||
        words.size() < 9u || words.size() > 16u ||
        (words.size() != 9u && words.size() != 14u) ||
        (words[0] & 0xffu) != words.size() * 4u ||
        (words[2] != k_bind_cid && words[2] != k_call_cid) ||
        m_replies.size() + m_guest_pending.size() +
            m_deferred_binds.size() +
            m_pending_module_starts.size() >= 64u ||
        payload.size() > 4096u)
    {
        return false;
    }
    if (!payload.empty() &&
        (ee_payload_destination == 0u ||
         !m_ee_ram.contains(ee_payload_destination, payload.size())))
    {
        return false;
    }
    if (payload.empty() && ee_payload_destination != 0u)
    {
        return false;
    }
    s_reply packet{};
    packet.m_word_count = static_cast<uint32_t>(words.size());
    std::copy(words.begin(), words.end(), std::begin(packet.m_words));
    packet.m_extra_destination = ee_payload_destination;
    packet.m_extra_data = payload;
    m_replies.push_back(std::move(packet));
    (void)pump();
    return true;
}

bool c_sif_rpc_bridge::queue_iop_guest_command(
    const std::vector<uint32_t>& words,
    uint32_t ee_payload_destination,
    const std::vector<uint8_t>& payload
)
{
    // The SIF command header's low byte is the packet length,
    // while its upper 24 bits carry the optional extra DMA length.
    // SIF0 uses the same bounded, queued delivery path as RPC
    // replies, preserving receiver ownership and DMAC acknowledgments.
    if (!m_sifcmd.initialized() || words.size() < 4u ||
        words.size() > 16u ||
        (words[0] & 0xffu) != words.size() * 4u ||
        (words[0] >> 8u) != payload.size() ||
        words[1] != (payload.empty() ? 0u : ee_payload_destination) ||
        words[2] == 0u ||
        m_replies.size() + m_guest_pending.size() +
            m_deferred_binds.size() +
            m_pending_module_starts.size() >= 64u ||
        payload.size() > 4096u ||
        (!payload.empty() &&
         (ee_payload_destination == 0u ||
          !m_ee_ram.contains(ee_payload_destination, payload.size()))))
    {
        return false;
    }
    s_reply command{};
    command.m_word_count = static_cast<uint32_t>(words.size());
    std::copy(words.begin(), words.end(), std::begin(command.m_words));
    command.m_extra_destination = ee_payload_destination;
    command.m_extra_data = payload;
    m_replies.push_back(std::move(command));
    (void)pump();
    return true;
}

void c_sif_rpc_bridge::on_guest_rpc_server_registered(uint32_t server_id)
{
    if (!m_sifcmd.has_guest_rpc_server(server_id))
    {
        return;
    }
    uint32_t released = 0u;
    for (auto it = m_deferred_binds.begin();
         it != m_deferred_binds.end();)
    {
        if (it->m_server_id == server_id)
        {
            m_replies.push_back(std::move(it->m_reply));
            it = m_deferred_binds.erase(it);
            ++released;
        }
        else
        {
            ++it;
        }
    }
    if (released != 0u)
    {
        std::fprintf(stderr,
            "IOP HLE RPC guest server registered: id=0x%08x binds=%u\n",
            server_id, released);
        (void)pump();
    }
}

bool c_sif_rpc_bridge::finish_guest_rpc(
    uint32_t token, uint32_t guest_result
)
{
    const auto waiting = m_guest_pending.find(token);
    if (waiting == m_guest_pending.end() ||
        m_replies.size() >= 64u)
    {
        return false;
    }
    const s_guest_pending& call = waiting->second;
    if (call.m_recv_size != 0u && guest_result != 0u)
    {
        const uint32_t physical = guest_result & 0x1fffffffu;
        const uint32_t region = guest_result & 0xe0000000u;
        if ((region != 0u && region != 0x80000000u &&
             region != 0xa0000000u) ||
            physical == 0u || physical >= c_cpu::k_ram_size ||
            call.m_recv_size > c_cpu::k_ram_size - physical ||
            !m_ee_ram.contains(call.m_destination, call.m_recv_size))
        {
            return false;
        }
        // Validate and snapshot all guest bytes before touching EE RAM.
        std::vector<uint8_t> data(call.m_recv_size);
        for (uint32_t i = 0u; i < call.m_recv_size; ++i)
        {
            if (!m_kernel.cpu().read8(guest_result + i, &data[i]))
            {
                return false;
            }
        }
        for (uint32_t i = 0u; i < data.size(); ++i)
        {
            m_ee_ram.write8(call.m_destination + i, data[i]);
        }
        // MCSERV returns a pointer. The first copied word is the RPC
        // status, while save-file data uses separate guest transfers.
        if ((call.m_server_id == 0x80000400u ||
             call.m_server_id == 0x80000480u) && data.size() >= 4u)
        {
            const uint32_t status = static_cast<uint32_t>(data[0]) |
                (static_cast<uint32_t>(data[1]) << 8u) |
                (static_cast<uint32_t>(data[2]) << 16u) |
                (static_cast<uint32_t>(data[3]) << 24u);
            std::fprintf(stderr,
                "[mc] RPC reply: server=0x%08x method=0x%08x token=%u "
                "status=0x%08x ee_destination=0x%08x bytes=%u\n",
                call.m_server_id, call.m_method, token, status,
                call.m_destination, call.m_recv_size);
        }
    }
    // Zero return pointers still complete the guest RPC, with no
    // unsolicited changes to the EE's receive buffer.
    m_replies.push_back(call.m_reply);
    m_guest_pending.erase(waiting);
    (void)pump();
    return true;
}

void c_sif_rpc_bridge::reset_for_reboot()
{
    m_loadfile_version_known = false;
    std::fill(std::begin(m_loadfile_version),
              std::end(m_loadfile_version), uint8_t{0});
    m_cdvd_streaming = false;
    m_cdvd_stream_position = 0u;
    m_replies.clear();
    m_deferred_binds.clear();
    m_pending_module_starts.clear();
    m_guest_pending.clear();
    m_next_guest_token = 1u;
    m_load_requests = 0u;
    m_last_observation = {};
}

bool c_sif_rpc_bridge::can_queue_replies(size_t count) const
{
    return count <= 64u &&
        m_replies.size() + m_guest_pending.size() +
            m_deferred_binds.size() +
            m_pending_module_starts.size() <= 64u - count;
}

void c_sif_rpc_bridge::queue_init_sreg_reply()
{
    // This packet exists only in response to a validated EE INIT_CMD
    // opt=1 request. The guest EE handler consumes it via armed SIF0;
    // do not write its SREG or mark any IOP module as started here.
    s_reply reply{};
    reply.m_word_count = 6u;
    reply.m_words[0] = 24u;
    reply.m_words[2] = 0x80000001u; // SET_SREG
    reply.m_words[4] = 0u;          // SREG index
    reply.m_words[5] = 1u;          // initialized
    m_replies.push_back(reply);
}

void c_sif_rpc_bridge::set_ee_receive_busy(bool busy)
{
    m_ee_receive_busy = busy;
}

bool c_sif_rpc_bridge::pump_pending()
{
    // A stopped initializer can still be restarted. A deleted or faulted
    // initializer can never return, so complete its LOADFILE call with an error.
    for (auto it = m_pending_module_starts.begin();
         it != m_pending_module_starts.end();)
    {
        const s_kernel_thread* thread = m_kernel.thread(it->m_thread_id);
        const bool failed = thread == nullptr ||
            thread->m_status == e_thread_status::faulted ||
            !thread->m_module_entry || thread->m_module_id != it->m_module_id;
        if (!failed && (thread->m_status != e_thread_status::dormant ||
                        !thread->m_module_entry_returned))
        {
            ++it;
            continue;
        }
        const uint32_t module_result = failed ? k_loadfile_failure : it->m_module_id;
        const uint32_t guest_start_result = failed ? k_loadfile_failure :
            thread->m_module_entry_result;
        m_ee_ram.write32(it->m_destination, module_result);
        m_ee_ram.write32(it->m_destination + 4u,
                          guest_start_result);
        if (m_last_observation.m_sequence == it->m_sequence)
        {
            m_last_observation.m_return_word = module_result;
            if (failed)
            {
                m_last_observation.m_load_status = e_disc_load_result::thread_failed;
            }
        }
        it->m_reply.m_module_start_reply = true;
        it->m_reply.m_module_start_id = it->m_module_id;
        std::fprintf(stderr,
            "IOP LOADFILE result queued: module=%u thread=%u "
            "result=0x%08x\n",
            it->m_module_id, it->m_thread_id, guest_start_result);
        m_replies.push_back(std::move(it->m_reply));
        // LOADFILE owns this one-shot initializer. Once its reply has
        // captured the real guest return value, release the temporary
        // stack while keeping the resident IRX and its worker threads.
        if (!failed)
        {
            (void)m_kernel.delete_thread(it->m_thread_id);
        }
        it = m_pending_module_starts.erase(it);
    }
    // DMA5 may belong to another transport, and an acknowledged
    // interrupt can still have a guest handler reading the receiver.
    if (m_ee_receive_busy || (m_dmac.status() & (1u << 5u)) != 0u)
    {
        return false;
    }
    if (m_replies.empty() || !m_sifcmd.initialized())
    {
        return false;
    }

    const uint32_t destination = m_sifcmd.ee_receive_address();
    const s_reply& reply = m_replies.front();
    ee::s_ee_u128 chcr{};
    if (destination == 0u ||
        (destination & 0x0fu) != 0u ||
        !m_ee_ram.contains(destination, reply.m_word_count * 4u) ||
        !ee::c_ee_dmac::mmio_read(
            &m_dmac, ee::c_ee_dmac::k_sif0_base, 4u, &chcr) ||
        (chcr.m_lo & ee::c_ee_dmac::k_chcr_str) == 0u)
    {
        return false;
    }

    if (!reply.m_extra_data.empty())
    {
        if (!m_ee_ram.contains(
                reply.m_extra_destination, reply.m_extra_data.size()))
        {
            return false;
        }
        for (uint32_t index = 0u;
             index < reply.m_extra_data.size(); ++index)
        {
            m_ee_ram.write8(reply.m_extra_destination + index,
                            reply.m_extra_data[index]);
        }
    }
    for (uint32_t index = 0u; index < reply.m_word_count; ++index)
    {
        m_ee_ram.write32(destination + index * 4u,
                         reply.m_words[index]);
    }
    if (!m_dmac.raise(5u))
    {
        return false;
    }
    if (reply.m_module_start_reply)
    {
        std::fprintf(stderr,
            "IOP LOADFILE EE reply delivered: module=%u DMA5=1\n",
            reply.m_module_start_id);
    }
    m_replies.pop_front();
    return true;
}

size_t c_sif_rpc_bridge::pending_replies() const
{
    return m_replies.size();
}

size_t c_sif_rpc_bridge::pending_module_starts() const
{
    return m_pending_module_starts.size();
}

size_t c_sif_rpc_bridge::pending_binds() const
{
    return m_deferred_binds.size();
}

bool c_sif_rpc_bridge::has_waiting_bind_guest_work() const
{
    return !m_deferred_binds.empty() && m_kernel.has_guest_work();
}

uint32_t c_sif_rpc_bridge::load_requests() const
{
    return m_load_requests;
}
} // namespace iop
