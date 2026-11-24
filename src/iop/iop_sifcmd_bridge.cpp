#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace iop
{
namespace
{
bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    // The guest reaches a genuine R3000 syscall via its relocated import.
    const uint32_t instructions[5] =
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
        valid = cpu.write32(pc + i * 4u, instructions[i]) && valid;
    }
    return valid;
}
} // namespace

c_sifcmd_bridge::c_sifcmd_bridge(
    c_kernel& kernel, ee::c_ee_sif& sif, ee::ee_memory* ee_ram
)
    : m_kernel(kernel), m_sif(sif), m_ee_ram(ee_ram)
{
}

void c_sifcmd_bridge::bind_rpc_bridge(c_sif_rpc_bridge* bridge)
{
    m_rpc_bridge = bridge;
}

bool c_sifcmd_bridge::guest_ram_span(uint32_t address, uint32_t size)
{
    const uint32_t region = address & 0xe0000000u;
    const uint32_t physical = address & 0x1fffffffu;
    return address != 0u &&
        (region == 0u || region == 0x80000000u ||
         region == 0xa0000000u) &&
        physical < c_cpu::k_ram_size &&
        size <= c_cpu::k_ram_size - physical;
}

bool c_sifcmd_bridge::has_guest_rpc_server(uint32_t server_id) const
{
    return m_rpc_initialized &&
        m_rpc_servers.find(server_id) != m_rpc_servers.end() &&
        m_rpc_queue_threads.find(
            m_rpc_servers.at(server_id).m_queue) !=
                m_rpc_queue_threads.end();
}

bool c_sifcmd_bridge::can_queue_guest_rpc(
    uint32_t server_id, uint32_t send_size) const
{
    const auto found = m_rpc_servers.find(server_id);
    if (!has_guest_rpc_server(server_id) || found == m_rpc_servers.end() ||
        send_size > 4096u ||
        (send_size != 0u &&
         !guest_ram_span(found->second.m_buffer, send_size)))
    {
        return false;
    }
    const auto waiting = m_rpc_requests.find(found->second.m_queue);
    return waiting == m_rpc_requests.end() ||
        waiting->second.size() < 64u;
}

bool c_sifcmd_bridge::enqueue_guest_rpc(
    uint32_t token, uint32_t server_id, uint32_t method,
    const std::vector<uint8_t>& bytes
)
{
    if (!can_queue_guest_rpc(
            server_id, static_cast<uint32_t>(bytes.size())))
    {
        return false;
    }
    const uint32_t queue = m_rpc_servers.at(server_id).m_queue;
    m_rpc_requests[queue].push_back({token, server_id, method, bytes});
    const auto thread = m_rpc_queue_threads.find(queue);
    std::fprintf(stderr,
        "IOP guest RPC queued: token=%u server=0x%08x method=%u "
        "queue=0x%08x worker=%u bytes=%zu\n",
        token, server_id, method, queue,
        thread == m_rpc_queue_threads.end() ? 0u : thread->second,
        bytes.size());
    if (thread != m_rpc_queue_threads.end())
    {
        const s_kernel_thread* current = m_kernel.thread(thread->second);
        if (current != nullptr &&
            current->m_status == e_thread_status::blocked &&
            current->m_wait_type == 50u)
        {
            (void)m_kernel.wake_thread(thread->second);
        }
    }
    return true;
}

bool c_sifcmd_bridge::install()
{
    // Preserve original syscall entry points. Reserve separate stubs
    // for actual guest-side RPC server lifecycle operations.
    constexpr std::array<uint16_t, 15u> ordinals = {
        4u, 6u, 7u, 12u, 14u, 15u, 16u, 17u,
        18u, 19u, 22u, 23u, 24u, 25u, 28u
    };
    for (uint16_t ordinal : ordinals)
    {
        uint32_t pc = k_extra_stub_base + ordinal * 0x20u;
        if (ordinal == 4u)
        {
            pc = k_init_cmd_stub;
        }
        else if (ordinal == 6u)
        {
            pc = k_get_sreg_stub;
        }
        else if (ordinal == 7u)
        {
            pc = k_set_sreg_stub;
        }
        if (!create_stub(m_kernel.cpu(), pc, k_service_base + ordinal) ||
            !m_kernel.register_library("sifcmd", ordinal, pc))
        {
            return false;
        }
    }
    // The guest returns from its service to this trampoline only
    // after executing the actual IRX-registered RPC handler function.
    // Preserve the handler's actual v0 before the syscall stub loads
    // its service selector. R3000 k0 is reserved kernel scratch.
    m_installed = m_kernel.cpu().write32(
        k_rpc_finish_stub, 0x0040d021u) &&
        create_stub(m_kernel.cpu(), k_rpc_finish_stub + 4u,
                    k_service_base + 0xfeu);
    return m_installed;
}

bool c_sifcmd_bridge::initialize_hle_bootstrap()
{
    if (!m_installed || m_initialized || m_hle_bootstrapped)
    {
        return false;
    }

    // SIF_STAT_CMDINIT | SIF_STAT_BOOTEND. The user's ps2-web-recomp
    // SDBZ HLE IOP kernel publishes precisely 0x60000 at boot, as its
    // IOP services are host-backed. This is deliberately NOT a guest
    // R3000 signal, real ROM0 readiness, or an RPC completion. Only the
    // explicit HLE runtime initializer may establish this service state.
    m_initialized = true;
    m_hle_bootstrapped = true;
    m_sif.publish_iop_subaddr(k_command_buffer);
    m_sif.publish_iop_smflag(0x00060000u);
    return true;
}

bool c_sifcmd_bridge::take_reboot_request(std::string* argument)
{
    if (!m_reboot_pending || argument == nullptr)
    {
        return false;
    }
    *argument = m_reboot_argument;
    m_reboot_argument.clear();
    m_reboot_pending = false;
    return true;
}

void c_sifcmd_bridge::reset_for_reboot()
{
    for (uint32_t& value : m_sregs)
    {
        value = 0u;
    }
    m_ee_receive_address = 0u;
    m_initialized = false;
    m_installed = false;
    m_hle_bootstrapped = false;
    m_rpc_initialized = false;
    m_rpc_servers.clear();
    m_rpc_clients.clear();
    m_next_iop_rpc_id = 1u;
    m_rpc_queue_threads.clear();
    m_rpc_requests.clear();
    m_rpc_thread_queues.clear();
    m_rpc_thread_active.clear();
    m_reboot_pending = false;
    m_reboot_argument.clear();
}


bool c_sifcmd_bridge::handle_rpc_syscall(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t argument = cpu.m_gpr[4];
    const auto fail = [&cpu]()
    {
        cpu.m_gpr[2] = static_cast<uint32_t>(-1);
        return true;
    };

    if (ordinal == 14u) // sceSifInitRpc
    {
        if (!m_initialized)
        {
            return false;
        }
        m_rpc_initialized = true;
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (!m_rpc_initialized)
    {
        return false;
    }
    if (ordinal == 15u) // sceSifBindRpc
    {
        const uint32_t server_id = cpu.m_gpr[5];
        const uint32_t mode = cpu.m_gpr[6];
        const uint32_t thread_id = m_kernel.current_thread_id();
        if (m_rpc_bridge == nullptr ||
            !guest_ram_span(argument, 40u) ||
            server_id == 0u || (mode & ~3u) != 0u ||
            m_next_iop_rpc_id == 0u ||
            m_rpc_clients.size() >= 64u ||
            (m_rpc_clients.count(argument) != 0u &&
             m_rpc_clients.at(argument).m_pending) ||
            ((mode & 1u) == 0u && thread_id == 0u))
        {
            return fail();
        }
        const uint32_t id = m_next_iop_rpc_id;
        const std::vector<uint32_t> packet = {
            36u, 0u, c_sif_rpc_bridge::k_bind_cid, 0u,
            id, argument, id, argument, server_id
        };
        if (!m_rpc_bridge->queue_iop_command(packet))
        {
            return fail();
        }
        m_rpc_clients[argument] = s_rpc_client{
            argument, server_id, 0u, 0u, id, id,
            (mode & 1u) == 0u ? thread_id : 0u,
            c_sif_rpc_bridge::k_bind_cid, true, false};
        ++m_next_iop_rpc_id;
        if (!m_kernel.cpu().write32(argument, 1u) ||
            !m_kernel.cpu().write32(argument + 4u, id) ||
            !m_kernel.cpu().write32(argument + 20u, 0u) ||
            !m_kernel.cpu().write32(argument + 24u, 0u) ||
            !m_kernel.cpu().write32(argument + 36u, 0u))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        if ((mode & 1u) == 0u &&
            !m_kernel.block_current(51u))
        {
            return false;
        }
        return true;
    }
    if (ordinal == 16u) // sceSifCallRpc
    {
        const auto found = m_rpc_clients.find(argument);
        const uint32_t method = cpu.m_gpr[5];
        const uint32_t mode = cpu.m_gpr[6];
        const uint32_t source = cpu.m_gpr[7];
        const uint32_t sp = cpu.m_gpr[29];
        uint32_t send_size = 0u;
        uint32_t receive = 0u;
        uint32_t recv_size = 0u;
        uint32_t callback = 0u;
        uint32_t callback_arg = 0u;
        uint32_t published_server = 0u;
        if (m_rpc_bridge == nullptr ||
            found == m_rpc_clients.end() ||
            !found->second.m_bound || found->second.m_pending ||
            !guest_ram_span(argument, 40u) ||
            !guest_ram_span(sp, 36u) ||
            !m_kernel.cpu().read32(sp + 16u, &send_size) ||
            !m_kernel.cpu().read32(sp + 20u, &receive) ||
            !m_kernel.cpu().read32(sp + 24u, &recv_size) ||
            !m_kernel.cpu().read32(sp + 28u, &callback) ||
            !m_kernel.cpu().read32(sp + 32u, &callback_arg) ||
            !m_kernel.cpu().read32(argument + 36u, &published_server) ||
            published_server != found->second.m_server_data ||
            (mode & ~3u) != 0u ||
            send_size > 4096u || recv_size > 4096u ||
            (send_size != 0u && !guest_ram_span(source, send_size)) ||
            (recv_size != 0u && !guest_ram_span(receive, recv_size)) ||
            // A non-null end-function must execute in guest R3000
            // context. Do not claim to have invoked it.
            callback != 0u ||
            m_next_iop_rpc_id == 0u ||
            ((mode & 1u) == 0u &&
             m_kernel.current_thread_id() == 0u))
        {
            return fail();
        }
        const uint32_t ee_address = found->second.m_ee_buffer;
        const uint32_t ee_region = ee_address & 0xe0000000u;
        const uint32_t ee_physical = ee_address & 0x1fffffffu;
        if (send_size != 0u &&
            ((ee_region != 0u && ee_region != 0x80000000u &&
              ee_region != 0xa0000000u) ||
             ee_physical == 0u || m_ee_ram == nullptr ||
             !m_ee_ram->contains(ee_physical, send_size)))
        {
            return fail();
        }
        std::vector<uint8_t> bytes(send_size);
        for (uint32_t i = 0u; i < send_size; ++i)
        {
            if (!m_kernel.cpu().read8(source + i, &bytes[i]))
            {
                return fail();
            }
        }
        const uint32_t id = m_next_iop_rpc_id;
        const std::vector<uint32_t> packet = {
            56u, 0u, c_sif_rpc_bridge::k_call_cid, 0u,
            id, argument, id, argument, method, send_size,
            receive, recv_size, 1u,
            found->second.m_server_data
        };
        if (!m_rpc_bridge->queue_iop_command(
                packet, send_size == 0u ? 0u : ee_physical, bytes))
        {
            return fail();
        }
        s_rpc_client& client = found->second;
        client.m_packet_id = id;
        client.m_rpc_id = id;
        client.m_pending = true;
        client.m_expected_command = c_sif_rpc_bridge::k_call_cid;
        client.m_wait_thread = (mode & 1u) == 0u
            ? m_kernel.current_thread_id() : 0u;
        ++m_next_iop_rpc_id;
        if (!m_kernel.cpu().write32(argument, 1u) ||
            !m_kernel.cpu().write32(argument + 4u, id) ||
            !m_kernel.cpu().write32(argument + 28u, callback) ||
            !m_kernel.cpu().write32(argument + 32u, callback_arg))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        if ((mode & 1u) == 0u &&
            !m_kernel.block_current(51u))
        {
            return false;
        }
        return true;
    }
    if (ordinal == 18u) // sceSifCheckStatRpc
    {
        uint32_t status = 0u;
        if (!guest_ram_span(argument, 4u) ||
            !m_kernel.cpu().read32(argument, &status))
        {
            return fail();
        }
        cpu.m_gpr[2] = status != 0u ? 1u : 0u;
        return true;
    }
    if (ordinal == 19u) // sceSifSetRpcQueue(queue*, thread_id)
    {
        const uint32_t thread_id = cpu.m_gpr[5];
        if (!guest_ram_span(argument, 4u) ||
            m_kernel.thread(thread_id) == nullptr ||
            m_rpc_queue_threads.count(argument) != 0u)
        {
            return fail();
        }
        m_rpc_queue_threads[argument] = thread_id;
        m_rpc_thread_queues[thread_id] = argument;
        // Queue assignment and server registration may occur in either
        // order. Only a guest-created server becomes eligible for replies.
        if (m_rpc_bridge != nullptr)
        {
            for (const auto& entry : m_rpc_servers)
            {
                if (entry.second.m_queue == argument)
                {
                    m_rpc_bridge->on_guest_rpc_server_registered(entry.first);
                }
            }
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal == 17u) // sceSifRegisterRpc
    {
        const uint32_t server_id = cpu.m_gpr[5];
        const uint32_t handler = cpu.m_gpr[6];
        const uint32_t buffer = cpu.m_gpr[7];
        const uint32_t sp = cpu.m_gpr[29];
        uint32_t queue = 0u;
        uint32_t instruction = 0u;
        if (!guest_ram_span(argument, 12u) ||
            !guest_ram_span(sp, 28u) ||
            !m_kernel.cpu().read32(sp + 24u, &queue) ||
            !guest_ram_span(queue, 4u) ||
            !guest_ram_span(handler, 4u) ||
            (handler & 3u) != 0u ||
            !m_kernel.cpu().read32(handler, &instruction) ||
            (buffer != 0u && !guest_ram_span(buffer, 1u)) ||
            server_id == 0u || m_rpc_servers.count(server_id) != 0u ||
            m_rpc_servers.size() >= 64u)
        {
            return fail();
        }
        if (!m_kernel.cpu().write32(argument, server_id) ||
            !m_kernel.cpu().write32(argument + 4u, handler) ||
            !m_kernel.cpu().write32(argument + 8u, buffer))
        {
            return false;
        }
        m_rpc_servers.emplace(
            server_id, s_rpc_server{argument, handler, buffer, queue});
        if (m_rpc_bridge != nullptr)
        {
            m_rpc_bridge->on_guest_rpc_server_registered(server_id);
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal == 22u) // sceSifRpcLoop
    {
        const uint32_t thread_id = m_kernel.current_thread_id();
        const auto queue_thread = m_rpc_queue_threads.find(argument);
        if (thread_id == 0u ||
            queue_thread == m_rpc_queue_threads.end() ||
            queue_thread->second != thread_id ||
            m_rpc_thread_active.count(thread_id) != 0u)
        {
            return false;
        }
        auto& requests = m_rpc_requests[argument];
        if (requests.empty())
        {
            if (!m_kernel.block_current(50u))
            {
                return false;
            }
            // The thread resumes *inside* the HLE RPC loop, rather
            // than returning to an IRX caller while no work exists.
            cpu.m_next_pc =
                k_extra_stub_base + 22u * 0x20u;
            return true;
        }
        const s_rpc_request request = std::move(requests.front());
        const auto server = m_rpc_servers.find(request.m_server_id);
        if (server == m_rpc_servers.end() ||
            server->second.m_queue != argument ||
            (request.m_bytes.size() != 0u &&
             !guest_ram_span(
                 server->second.m_buffer,
                 static_cast<uint32_t>(request.m_bytes.size()))))
        {
            return false;
        }
        if (!request.m_bytes.empty() &&
            !m_kernel.cpu().dma_write(
                server->second.m_buffer, request.m_bytes.data(),
                static_cast<uint32_t>(request.m_bytes.size())))
        {
            return false;
        }
        requests.pop_front();
        m_rpc_thread_active[thread_id] = request.m_token;
        std::fprintf(stderr,
            "IOP guest RPC entered: token=%u server=0x%08x "
            "method=%u thread=%u handler=0x%08x queue=0x%08x "
            "buffer=0x%08x bytes=%zu cycles=%llu\n",
            request.m_token, request.m_server_id, request.m_method,
            thread_id, server->second.m_handler, argument,
            server->second.m_buffer, request.m_bytes.size(),
            static_cast<unsigned long long>(m_kernel.cycles()));
        // SDBZ_TRACE_RPC_ARGS=<server id> dumps that server's request bytes
        // (host-only diagnostic; never alters the guest buffer or reply).
        static const char* trace_server = std::getenv("SDBZ_TRACE_RPC_ARGS");
        if (trace_server != nullptr && !request.m_bytes.empty() &&
            std::strtoul(trace_server, nullptr, 0) == request.m_server_id &&
            request.m_method != 2u)
        {
            std::fprintf(stderr, "IOP guest RPC args: token=%u method=%u bytes:",
                         request.m_token, request.m_method);
            for (size_t i = 0; i < request.m_bytes.size(); ++i)
            {
                std::fprintf(stderr, "%s%02x", i % 16u == 0u ? "\n  " : " ",
                             request.m_bytes[i]);
            }
            std::fprintf(stderr, "\n  ascii: ");
            for (const uint8_t byte : request.m_bytes)
            {
                std::fputc(byte >= 0x20u && byte < 0x7fu ? byte : '.', stderr);
            }
            std::fprintf(stderr, "\n");
        }
        cpu.m_gpr[4] = request.m_method;
        cpu.m_gpr[5] = server->second.m_buffer;
        cpu.m_gpr[6] =
            static_cast<uint32_t>(request.m_bytes.size());
        cpu.m_gpr[31] = k_rpc_finish_stub;
        cpu.m_next_pc = server->second.m_handler;
        return true;
    }
    if (ordinal == 0xfeu) // actual guest handler return
    {
        const uint32_t thread_id = m_kernel.current_thread_id();
        const auto active = m_rpc_thread_active.find(thread_id);
        const auto queue = m_rpc_thread_queues.find(thread_id);
        if (active == m_rpc_thread_active.end() ||
            queue == m_rpc_thread_queues.end() ||
            m_rpc_bridge == nullptr ||
            !m_rpc_bridge->finish_guest_rpc(active->second, cpu.m_gpr[26]))
        {
            return false;
        }
        std::fprintf(stderr,
            "IOP guest RPC returned: token=%u thread=%u "
            "guest_result=0x%08x cycles=%llu\n",
            active->second, thread_id, cpu.m_gpr[26],
            static_cast<unsigned long long>(m_kernel.cycles()));
        m_rpc_thread_active.erase(active);
        cpu.m_gpr[4] = queue->second;
        cpu.m_next_pc =
            k_extra_stub_base + 22u * 0x20u;
        return true;
    }
    if (ordinal == 23u) // sceSifGetOtherData
    {
        // Arguments: descriptor*, EE source, IOP dest, byte count.
        const uint32_t source = cpu.m_gpr[5];
        const uint32_t destination = cpu.m_gpr[6];
        const uint32_t length = cpu.m_gpr[7];
        const uint32_t region = source & 0xe0000000u;
        const uint32_t physical = source & 0x1fffffffu;
        if (m_ee_ram == nullptr || length > 65536u ||
            (region != 0u && region != 0x80000000u &&
             region != 0xa0000000u) ||
            !m_ee_ram->contains(physical, length) ||
            (length != 0u && !guest_ram_span(destination, length)))
        {
            return fail();
        }
        std::vector<uint8_t> data(length);
        for (uint32_t i = 0u; i < length; ++i)
        {
            data[i] = m_ee_ram->read8(physical + i);
        }
        if (length != 0u &&
            !m_kernel.cpu().dma_write(
                destination, data.data(), length))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal == 24u) // sceSifRemoveRpc
    {
        for (auto it = m_rpc_servers.begin();
             it != m_rpc_servers.end(); ++it)
        {
            if (it->second.m_descriptor == argument)
            {
                for (const auto& active : m_rpc_thread_active)
                {
                    if (m_rpc_thread_queues.at(active.first) ==
                        it->second.m_queue)
                    {
                        return fail();
                    }
                }
                const auto pending =
                    m_rpc_requests.find(it->second.m_queue);
                if (pending != m_rpc_requests.end() &&
                    !pending->second.empty())
                {
                    return fail();
                }
                m_rpc_servers.erase(it);
                cpu.m_gpr[2] = 0u;
                return true;
            }
        }
        return fail();
    }
    if (ordinal == 25u) // sceSifRemoveRpcQueue
    {
        const auto found = m_rpc_queue_threads.find(argument);
        if (found == m_rpc_queue_threads.end() ||
            m_rpc_thread_active.count(found->second) != 0u)
        {
            return fail();
        }
        for (const auto& pair : m_rpc_servers)
        {
            if (pair.second.m_queue == argument)
            {
                return fail();
            }
        }
        m_rpc_requests.erase(argument);
        m_rpc_thread_queues.erase(found->second);
        m_rpc_queue_threads.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    }
    return false;
}
bool c_sifcmd_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    if ((service >= k_service_base + 14u &&
         service <= k_service_base + 25u) ||
        service == k_service_base + 0xfeu)
    {
        const uint32_t ordinal = service - k_service_base;
        if (ordinal == 14u || ordinal == 17u ||
            ordinal == 15u || ordinal == 16u ||
            ordinal == 18u || ordinal == 19u ||
            ordinal == 22u || ordinal == 23u ||
            ordinal == 24u || ordinal == 25u ||
            ordinal == 0xfeu)
        {
            return handle_rpc_syscall(ordinal);
        }
    }

    switch (service)
    {
    case k_service_base + 12u: // sceSifSendCmd
    case k_service_base + 28u: // sceSifSendCmdIntr
    {
        // ABI: cid, packet, size, extra_source, extra_destination,
        // extra_length. Fifth and sixth parameters are on the
        // original R3000 caller's stack at sp+16 and sp+20.
        s_cpu_state& state = kernel.cpu().state();
        const uint32_t cid = state.m_gpr[4];
        const uint32_t packet = state.m_gpr[5];
        const uint32_t size = state.m_gpr[6];
        const uint32_t extra_source = state.m_gpr[7];
        const uint32_t sp = state.m_gpr[29];
        uint32_t extra_destination = 0u;
        uint32_t extra_size = 0u;
        if (!m_initialized || m_rpc_bridge == nullptr ||
            size < 16u || size > 64u || (size & 3u) != 0u ||
            !guest_ram_span(packet, size) ||
            !guest_ram_span(sp, 24u) ||
            !kernel.cpu().read32(sp + 16u, &extra_destination) ||
            !kernel.cpu().read32(sp + 20u, &extra_size) ||
            extra_size > 4096u ||
            (extra_size != 0u &&
             !guest_ram_span(extra_source, extra_size)))
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        std::vector<uint32_t> words(size / 4u);
        for (uint32_t i = 0u; i < words.size(); ++i)
        {
            if (!kernel.cpu().read32(packet + i * 4u, &words[i]))
            {
                state.m_gpr[2] = 0u;
                return true;
            }
        }
        std::vector<uint8_t> extra(extra_size);
        for (uint32_t i = 0u; i < extra_size; ++i)
        {
            if (!kernel.cpu().read8(extra_source + i, &extra[i]))
            {
                state.m_gpr[2] = 0u;
                return true;
            }
        }
        // Never trust a guest-provided SIFCMD header or claim the
        // EE acknowledged a command. Only the real queued packet
        // may reach the armed SIF0 receiver.
        words[0] = (size & 0xffu) | (extra_size << 8u);
        words[1] = extra_size != 0u ? extra_destination : 0u;
        words[2] = cid;
        state.m_gpr[2] = m_rpc_bridge->queue_iop_guest_command(
            words, extra_destination, extra) ? 1u : 0u;
        return true;
    }

    case k_init_cmd_syscall:
        // The IOP publishes SUBADDR only after its R3000 guest code
        // has invoked sceSifInitCmd; never publish the SMFLAG here.
        m_initialized = true;
        m_sif.publish_iop_subaddr(k_command_buffer);
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    case k_get_sreg_syscall:
        if (!m_initialized)
        {
            return false;
        }
        kernel.cpu().state().m_gpr[2] =
            m_sregs[kernel.cpu().state().m_gpr[4] & 31u];
        return true;

    case k_set_sreg_syscall:
        if (!m_initialized)
        {
            return false;
        }
        m_sregs[kernel.cpu().state().m_gpr[4] & 31u] =
            kernel.cpu().state().m_gpr[5];
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;

    default:
        return false;
    }
}

bool c_sifcmd_bridge::initialized() const
{
    return m_initialized;
}

bool c_sifcmd_bridge::prepare_command(
    const std::vector<uint32_t>& words,
    uint32_t size,
    s_command* command
) const
{
    // The EE initialization and CHANGE_SADDR commands carry one word
    // after the 16-byte SIFCMD header; SET_SREG carries two words.
    // DMA transfers round to 16 bytes, but the header records the
    // original 20/24-byte packet size.
    if (!m_initialized || command == nullptr ||
        (size != 16u && size != 20u && size != 24u &&
         size != 48u && size != 104u) ||
        words.size() < size / 4u ||
        (words[0] & 0xffu) != size ||
        words[1] != 0u)
    {
        return false;
    }

    const uint32_t cid = words[2];

    // A Rend packet is an EE-originated completion of a specific
    // pending IOP client operation, not an arbitrary incoming command.
    // Validate all identity fields without modifying guest memory;
    // the containing SIF1 descriptor batch must pass as a whole.
    if (cid == c_sif_rpc_bridge::k_reply_cid && size == 48u)
    {
        const uint32_t client = words[7];
        const auto found = m_rpc_clients.find(client);
        if (words[3] != 0u || words[5] != client ||
            found == m_rpc_clients.end() ||
            !found->second.m_pending ||
            found->second.m_packet_id != words[4] ||
            found->second.m_rpc_id != words[6] ||
            found->second.m_expected_command != words[8])
        {
            return false;
        }
        const uint32_t expected = found->second.m_expected_command;
        if (expected != c_sif_rpc_bridge::k_bind_cid &&
            expected != c_sif_rpc_bridge::k_call_cid)
        {
            return false;
        }
        if (expected == c_sif_rpc_bridge::k_bind_cid &&
            words[9] != 0u &&
            (m_ee_ram == nullptr ||
             !m_ee_ram->contains(
                 words[10] & 0x1fffffffu, 1u)))
        {
            return false;
        }
        s_command reply{};
        reply.m_cid = cid;
        reply.m_rpc_end = true;
        reply.m_rpc_client = client;
        reply.m_rpc_command = words[8];
        reply.m_rpc_server_data = words[9];
        reply.m_rpc_buffer = words[10];
        reply.m_rpc_cbuffer = words[11];
        *command = reply;
        return true;
    }

    // SIF_CMD_RESET_CMD is a distinct system command. It is NOT a
    // normal RPC or an acknowledgement that ROM0/UDNL has booted.
    // PS2SDK's 104-byte reset packet contains arglen at +16, mode
    // at +20, and 80 argument bytes at +24, which need not have a
    // terminating NUL. Only the supported mode is accepted.
    if (cid == 0x80000003u && size == 104u)
    {
        if (words[3] != 0u || words[4] > 80u ||
            words[5] != 0u || m_reboot_pending)
        {
            return false;
        }
        s_command reboot{};
        reboot.m_cid = cid;
        reboot.m_reboot_request = true;
        for (uint32_t byte = 0u; byte < words[4]; ++byte)
        {
            const uint8_t character = static_cast<uint8_t>(
                words[6u + byte / 4u] >> (8u * (byte % 4u))
            );
            if (character < 0x20u || character > 0x7eu)
            {
                return false;
            }
            reboot.m_reboot_argument.push_back(
                static_cast<char>(character)
            );
        }
        *command = std::move(reboot);
        return true;
    }

    // EE's 16-byte INIT_CMD with opt=1 requests an IOP->EE
    // SET_SREG command (SREG[0] = 1). ps2-web-recomp/SDBZ
    // sends that packet to the EE through the SIF0 command receiver.
    // This is not the 20-byte receive-address publication, and it
    // must never update an EE SREG without an actual reply packet.
    if (cid == 0x80000002u && size == 16u &&
        words[3] == 1u)
    {
        *command = {cid, 0u, 0u, true};
        return true;
    }

    // Every remaining command needs at least the fifth word.
    // Only the explicitly handled opt=1 handshake is 16 bytes.
    if (size == 16u || words[3] != 0u)
    {
        return false;
    }

    // The IOP consumes only validated command packets. Neither a
    // published address nor a DMA flag synthesizes an RPC reply.
    const uint32_t index_or_address = words[4];
    if (cid == 0x80000001u && size == 24u &&
        index_or_address < 32u)
    {
        *command = {cid, index_or_address, words[5]};
        return true;
    }

    // 0x80000000 = CHANGE_SADDR, 0x80000002 = INIT_CMD.
    // Keep the already-tested 24-byte INIT_CMD variant only when
    // its extra word is zero, while accepting the real 20-byte form.
    const bool receive_command =
        (cid == 0x80000000u && size == 20u) ||
        (cid == 0x80000002u &&
         (size == 20u || (size == 24u && words[5] == 0u)));
    if (receive_command &&
        index_or_address != 0u &&
        (index_or_address & 0x0fu) == 0u &&
        index_or_address < 0x02000000u)
    {
        *command = {cid, 0u, index_or_address};
        return true;
    }

    // Other CIDs require registered guest callback handlers and
    // reply scheduling. Reject them until that path is implemented.
    return false;
}

void c_sifcmd_bridge::commit_command(const s_command& command)
{
    if (command.m_rpc_end &&
        command.m_cid == c_sif_rpc_bridge::k_reply_cid)
    {
        auto found = m_rpc_clients.find(command.m_rpc_client);
        if (found == m_rpc_clients.end() ||
            !found->second.m_pending ||
            found->second.m_expected_command != command.m_rpc_command)
        {
            return;
        }
        s_rpc_client& client = found->second;
        // The EE's Rend controls the state transition. An absent
        // server-data pointer denotes an unsuccessful bind, not a
        // remotely initialized RPC service.
        if (command.m_rpc_command == c_sif_rpc_bridge::k_bind_cid)
        {
            client.m_server_data = command.m_rpc_server_data;
            client.m_ee_buffer = command.m_rpc_buffer;
            client.m_bound = command.m_rpc_server_data != 0u;
            (void)m_kernel.cpu().write32(
                command.m_rpc_client + 36u,
                client.m_server_data);
            (void)m_kernel.cpu().write32(
                command.m_rpc_client + 20u,
                client.m_ee_buffer);
            (void)m_kernel.cpu().write32(
                command.m_rpc_client + 24u,
                command.m_rpc_cbuffer);
        }
        client.m_pending = false;
        client.m_expected_command = 0u;
        (void)m_kernel.cpu().write32(command.m_rpc_client, 0u);
        if (client.m_wait_thread != 0u)
        {
            (void)m_kernel.wake_thread_with_result(
                client.m_wait_thread, 0u);
            client.m_wait_thread = 0u;
        }
        return;
    }
    if (command.m_reboot_request &&
        command.m_cid == 0x80000003u)
    {
        m_reboot_argument = command.m_reboot_argument;
        m_reboot_pending = true;
    }
    else if (command.m_cid == 0x80000001u)
    {
        m_sregs[command.m_index] = command.m_value;
    }
    else if ((command.m_cid == 0x80000000u ||
              command.m_cid == 0x80000002u) &&
             !command.m_request_sreg_reply)
    {
        m_ee_receive_address = command.m_value;
    }
}

uint32_t c_sifcmd_bridge::sreg(uint32_t index) const
{
    return index < 32u ? m_sregs[index] : 0u;
}

uint32_t c_sifcmd_bridge::ee_receive_address() const
{
    return m_ee_receive_address;
}
} // namespace iop
