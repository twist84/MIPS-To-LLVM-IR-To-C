#include "iop_kernel_hle_bridge.h"
#include "iop_folder_sector_source.h"
#include "iop_sysclib_strings.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "IOP kernel HLE imports: %s\n", reason);
    return 1;
}

void write16(std::vector<uint8_t>& data, size_t at, uint16_t value)
{
    data[at] = static_cast<uint8_t>(value);
    data[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void write32(std::vector<uint8_t>& data, size_t at, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[at + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

struct s_imports
{
    const char* m_name;
    std::vector<uint16_t> m_ordinals;
};

const std::vector<s_imports>& retail_dependencies()
{
    static const std::vector<s_imports> imports = {
        {"loadcore", {6u}},
        {"intrman", {4u, 5u, 6u, 7u, 17u, 18u}},
        {"stdio", {4u}},
        {"dmacman", {28u, 32u, 33u, 34u, 35u}},
        {"thbase", {4u, 6u, 20u}},
        {"thevent", {4u, 6u, 7u, 8u, 10u}},
        {"thsemap", {4u, 6u, 8u, 11u}},
        {"sysclib", {8u, 29u, 36u}}
    };
    return imports;
}

std::vector<uint8_t> synthetic_sio2man_dependencies()
{
    // Structurally equivalent to the original module's eight import
    // libraries and all 28 ordinals. The code and data are synthetic;
    // no game assets or copyrighted IOP instructions are embedded.
    std::vector<uint8_t> image(0x660u, 0u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1u;
    image[5] = 1u;
    image[6] = 1u;
    write16(image, 16u, 0xff80u);
    write16(image, 18u, 8u);
    write32(image, 28u, 0x34u);
    write16(image, 40u, 52u);
    write16(image, 42u, 32u);
    write16(image, 44u, 2u);

    write32(image, 0x34u, 1u); // PT_LOAD
    write32(image, 0x38u, 0x100u);
    write32(image, 0x44u, 0x360u);
    write32(image, 0x48u, 0x380u);
    write32(image, 0x54u, 0x70000080u); // PT_SCE_IOPMOD
    write32(image, 0x58u, 0x600u);
    write32(image, 0x604u, 0u);        // entry
    write32(image, 0x608u, 0x8fb0u);   // out-of-image real SIO2MAN GP
    write32(image, 0x60cu, 0x360u);
    write32(image, 0x610u, 0x10u);
    write32(image, 0x614u, 0x10u);
    write32(image, 0x100u, 0x0000000du); // guest break

    uint32_t at = 0x40u;
    for (const s_imports& group : retail_dependencies())
    {
        write32(image, 0x100u + at, 0x41e00000u);
        for (uint32_t i = 0u; group.m_name[i] != 0; ++i)
        {
            image[0x100u + at + 12u + i] =
                static_cast<uint8_t>(group.m_name[i]);
        }
        at += 20u;
        for (uint16_t ordinal : group.m_ordinals)
        {
            write32(image, 0x100u + at, 0x03e00008u);
            write32(image, 0x100u + at + 4u,
                    0x24000000u | ordinal);
            at += 8u;
        }
    }
    return image;
}

bool call_guest(
    iop::c_kernel& kernel, const std::string& library,
    uint16_t ordinal, const std::array<uint32_t, 4>& arguments,
    uint32_t* result
)
{
    static uint32_t next_program = 0x4000u;
    uint32_t entry = 0u;
    if (!kernel.lookup_library(library, ordinal, &entry) ||
        result == nullptr)
    {
        return false;
    }
    const uint32_t program = next_program;
    next_program += 0x80u;
    // Guest MIPS I: four 32-bit arguments, direct JAL to the HLE
    // import target, and a breakpoint after returning from the stub.
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!kernel.cpu().write32(
                program + i * 8u,
                0x3c040000u | (i << 16u) |
                    (arguments[i] >> 16u)) ||
            !kernel.cpu().write32(
                program + i * 8u + 4u,
                0x34840000u | (i << 21u) | (i << 16u) |
                    (arguments[i] & 0xffffu)))
        {
            return false;
        }
    }
    if (!kernel.cpu().write32(
            program + 32u,
            0x0c000000u | ((entry >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(program + 36u, 0u) ||
        !kernel.cpu().write32(program + 40u, 0x0000000du))
    {
        return false;
    }
    const uint32_t thread_id =
        kernel.create_thread(program, 0u, 1u, 0x400u);
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return false;
    }
    const iop::s_kernel_run_result ran = kernel.run(128u);
    const iop::s_kernel_thread* thread =
        kernel.thread(thread_id);
    if (ran.m_stop != iop::e_kernel_stop::breakpoint ||
        ran.m_guest_pc != program + 40u || thread == nullptr)
    {
        return false;
    }
    *result = thread->m_cpu.m_gpr[2];
    return true;
}

bool test_vblank_wait(uint16_t ordinal, bool start)
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    uint32_t import_pc = 0u;
    uint32_t unsupported = 0u;
    if (!hle.install() ||
        !kernel.lookup_library("vblank", ordinal, &import_pc) ||
        !kernel.lookup_library("vblank", 8u, &unsupported) ||
        !kernel.lookup_library("vblank", 9u, &unsupported) ||
        kernel.lookup_library("vblank", 10u, &unsupported))
    {
        return false;
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return hle.handle_syscall(active, service);
        }
    );
    constexpr uint32_t k_program = 0x3900u;
    if (!kernel.cpu().write32(
            k_program, 0x0c000000u |
                ((import_pc >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(k_program + 4u, 0u) ||
        !kernel.cpu().write32(k_program + 8u, 0x0000000du))
    {
        return false;
    }
    const uint32_t id = kernel.create_thread(
        k_program, 0u, 1u, 0x400u);
    if (id == 0u || !kernel.start_thread(id))
    {
        return false;
    }
    const iop::s_kernel_run_result blocked = kernel.run(128u);
    const iop::s_kernel_thread* thread = kernel.thread(id);
    if (blocked.m_stop != iop::e_kernel_stop::idle ||
        thread == nullptr ||
        thread->m_status != iop::e_thread_status::blocked ||
        thread->m_wait_type != (start ? 16u : 17u) ||
        kernel.has_guest_work())
    {
        return false;
    }
    // No edge and the opposite edge must not complete this import.
    if (kernel.run(16u).m_stop != iop::e_kernel_stop::idle)
    {
        return false;
    }
    hle.signal_vblank_edge(!start);
    thread = kernel.thread(id);
    if (thread == nullptr ||
        thread->m_status != iop::e_thread_status::blocked ||
        kernel.run(16u).m_stop != iop::e_kernel_stop::idle)
    {
        return false;
    }
    hle.signal_vblank_edge(start);
    const iop::s_kernel_run_result completed = kernel.run(128u);
    thread = kernel.thread(id);
    return completed.m_stop == iop::e_kernel_stop::breakpoint &&
           completed.m_guest_pc == k_program + 8u &&
           thread != nullptr &&
           thread->m_cpu.m_gpr[2] == 0u;
}
bool test_vblank_guest_callbacks()
{
    // Genuine guest code is called only by the matching scanout edge.
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!hle.install())
    {
        return false;
    }
    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return hle.handle_syscall(active, service);
    });
    constexpr uint32_t k_handler = 0x38000u;
    constexpr uint32_t k_result = 0x37000u;
    const uint32_t program[] = {
        0x3c080003u, // lui t0, 3
        0x35087000u, // ori t0, t0, 0x7000
        0xad040000u, // sw a0, 0(t0)
        0x03e00008u, // jr ra
        0u
    };
    for (uint32_t i = 0u; i < sizeof(program) / sizeof(program[0]); ++i)
    {
        if (!kernel.cpu().write32(k_handler + 4u * i, program[i]))
        {
            return false;
        }
    }
    if (!kernel.cpu().write32(k_result, 0xdeadbeefu))
    {
        return false;
    }
    uint32_t value = 0u;
    if (!call_guest(
            kernel, "vblank", 8u,
            {0u, 1u, k_handler, 0x12345678u}, &value) ||
        value != 0u || !kernel.cpu().read32(k_result, &value) ||
        value != 0xdeadbeefu)
    {
        return false;
    }
    hle.signal_vblank_edge(false);
    (void)kernel.run(32u);
    if (!kernel.cpu().read32(k_result, &value) ||
        value != 0xdeadbeefu)
    {
        return false;
    }
    hle.signal_vblank_edge(true);
    if (!kernel.cpu().read32(k_result, &value) ||
        value != 0xdeadbeefu)
    {
        return false;
    }
    (void)kernel.run(128u);
    if (!kernel.cpu().read32(k_result, &value) ||
        value != 0x12345678u)
    {
        return false;
    }
    // Remove by the original (edge, function) pair. A subsequent
    // identical edge is not permission to call a stale guest pointer.
    if (!call_guest(
            kernel, "vblank", 9u,
            {0u, k_handler, 0u, 0u}, &value) ||
        value != 0u || !kernel.cpu().write32(k_result, 0xbadc0deu))
    {
        return false;
    }
    hle.signal_vblank_edge(true);
    (void)kernel.run(128u);
    if (!kernel.cpu().read32(k_result, &value) ||
        value != 0xbadc0deu)
    {
        return false;
    }
    // Invalid callbacks cannot be registered with a success value.
    if (!call_guest(
            kernel, "vblank", 8u,
            {0u, 1u, 0x001fffffu, 3u}, &value) ||
        value != static_cast<uint32_t>(-1))
    {
        return false;
    }
    kernel.reset();
    hle.reset_for_reboot();
    return hle.install() && !kernel.has_guest_work();
}
bool test_registered_dma_guest_interrupt()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!kernel.attach_hle_hardware() || !hle.install())
    {
        return false;
    }
    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return hle.handle_syscall(active, service);
    });

    uint32_t query = 0u;
    if (!kernel.lookup_library("intrman", 23u, &query))
    {
        return false;
    }
    constexpr uint32_t handler = 0x00039000u;
    constexpr uint32_t result_addr = 0x00038000u;
    constexpr uint32_t source = 0x00037000u;
    constexpr uint32_t line = 36u;
    const uint32_t code[] = {
        0x03e08021u, // addu s0, ra, zero
        0x3c080003u, // lui t0, 3
        0x35088000u, // ori t0, t0, 0x8000
        0xad040000u, // sw a0, 0(t0)
        0x0c000000u | ((query >> 2u) & 0x03ffffffu), // jal QueryIntrContext
        0u,
        0xad020004u, // sw v0, 4(t0)
        0x0200f821u, // addu ra, s0, zero
        0x00001021u, // addu v0, zero, zero
        0x03e00008u, // jr ra
        0u
    };
    for (uint32_t i = 0u; i < sizeof(code) / sizeof(code[0]); ++i)
    {
        if (!kernel.cpu().write32(handler + i * 4u, code[i]))
        {
            return false;
        }
    }
    uint32_t value = 0u;
    if (!kernel.cpu().write32(result_addr, 0xdeadbeefu) ||
        !kernel.cpu().write32(result_addr + 4u, 0xdeadbeefu) ||
        !kernel.cpu().write32(source, 0x12345678u) ||
        !call_guest(kernel, "intrman", 4u,
                    {line, 0u, handler, 0x76543210u}, &value) ||
        value != 0u ||
        !call_guest(kernel, "intrman", 6u,
                    {line, 0u, 0u, 0u}, &value) ||
        value != 0u ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base, 4u,
                                  source) ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base + 4u,
                                  4u, 0x00010001u) ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base + 8u,
                                  4u, 0x01000001u) ||
        !kernel.cpu().read32(result_addr, &value) ||
        value != 0xdeadbeefu)
    {
        return false;
    }
    const iop::s_kernel_run_result progressed = kernel.run(256u, 32u);
    uint32_t context = 0u;
    uint32_t icr = 0u;
    if ((progressed.m_stop != iop::e_kernel_stop::idle &&
         progressed.m_stop != iop::e_kernel_stop::budget_exhausted) ||
        !kernel.cpu().read32(result_addr, &value) ||
        value != 0x76543210u ||
        !kernel.cpu().read32(result_addr + 4u, &context) ||
        context != 1u ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) ||
        (icr & (1u << 28u)) != 0u ||
        kernel.hardware().next_pending_interrupt() >= 0 ||
        kernel.in_interrupt_context())
    {
        return false;
    }
    // A real ordinary IRQ9 handler returning zero masks that source
    // as required by the primary reference, unlike a DMA line.
    if (!call_guest(kernel, "intrman", 4u,
                    {9u, 0u, handler, 0x11223344u}, &value) ||
        !call_guest(kernel, "intrman", 6u,
                    {9u, 0u, 0u, 0u}, &value) ||
        !kernel.hardware().raise_interrupt(9u) ||
        !kernel.cpu().write32(result_addr, 0xdeadbeefu))
    {
        return false;
    }
    (void)kernel.run(128u, 32u);
    uint32_t mask = 0u;
    return kernel.cpu().read32(result_addr, &value) &&
           value == 0x11223344u &&
           kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &mask) &&
           (mask & (1u << 9u)) == 0u;
}


// Original DMA4 completion must wake a guest waiter even when that
// waiter used CpuSuspendIntr before entering WaitSema. Its saved
// interrupt state must not mask the interrupt for the whole IOP.
bool test_suspended_waiter_dma4_irq_wakeup()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!kernel.attach_hle_hardware() || !hle.install())
    {
        return false;
    }
    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return hle.handle_syscall(active, service);
    });

    constexpr uint32_t k_sema_param = 0x00036000u;
    constexpr uint32_t k_dma_source = 0x00037000u;
    constexpr uint32_t k_irq_handler = 0x00039000u;
    constexpr uint32_t k_worker = 0x00044000u;
    constexpr uint32_t k_marker = 0x00047000u;
    uint32_t suspend_pc = 0u;
    uint32_t resume_pc = 0u;
    uint32_t wait_pc = 0u;
    uint32_t signal_pc = 0u;
    uint32_t sema_id = 0u;
    if (!kernel.lookup_library("intrman", 17u, &suspend_pc) ||
        !kernel.lookup_library("intrman", 18u, &resume_pc) ||
        !kernel.lookup_library("thsemap", 8u, &wait_pc) ||
        !kernel.lookup_library("thsemap", 7u, &signal_pc) ||
        !kernel.cpu().write32(k_sema_param, 0u) ||
        !kernel.cpu().write32(k_sema_param + 4u, 0u) ||
        !kernel.cpu().write32(k_sema_param + 8u, 0u) ||
        !kernel.cpu().write32(k_sema_param + 12u, 1u) ||
        !call_guest(kernel, "thsemap", 4u,
                    {k_sema_param, 0u, 0u, 0u}, &sema_id) ||
        sema_id == 0u || sema_id > 0x7fffu)
    {
        return false;
    }

    const uint32_t worker[] =
    {
        0x03e08021u, // addu s0,ra,zero
        0x24040000u, // addiu a0,zero,0
        0x0c000000u | ((suspend_pc >> 2u) & 0x03ffffffu),
        0u,
        0x24040000u | sema_id, // WaitSema(guest semaphore)
        0x0c000000u | ((wait_pc >> 2u) & 0x03ffffffu),
        0u,
        0x24040001u, // CpuResumeIntr(previously enabled)
        0x0c000000u | ((resume_pc >> 2u) & 0x03ffffffu),
        0u,
        0x3c080004u, // lui t0,4
        0x35087000u, // ori t0,t0,0x7000
        0x24090001u, // addiu t1,zero,1
        0xad090000u, // sw t1,0(t0)
        0x0200f821u, // addu ra,s0,zero
        0x03e00008u, // jr ra
        0u
    };
    const uint32_t irq[] =
    {
        0x03e08021u, // addu s0,ra,zero
        0x0c000000u | ((signal_pc >> 2u) & 0x03ffffffu),
        0u,           // iSignalSema(a0), from original INTRMAN arg
        0x0200f821u, // addu ra,s0,zero
        0x24020001u, // keep DMA4 interrupt enabled
        0x03e00008u, // jr ra
        0u
    };
    for (uint32_t i = 0u; i < sizeof(worker) / sizeof(worker[0]); ++i)
    {
        if (!kernel.cpu().write32(k_worker + i * 4u, worker[i]))
        {
            return false;
        }
    }
    for (uint32_t i = 0u; i < sizeof(irq) / sizeof(irq[0]); ++i)
    {
        if (!kernel.cpu().write32(k_irq_handler + i * 4u, irq[i]))
        {
            return false;
        }
    }
    uint32_t result = 0u;
    if (!kernel.cpu().write32(k_marker, 0u) ||
        !kernel.cpu().write32(k_dma_source, 0x11223344u) ||
        !call_guest(kernel, "intrman", 4u,
                    {36u, 0u, k_irq_handler, sema_id}, &result) ||
        result != 0u ||
        !call_guest(kernel, "intrman", 6u,
                    {36u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return false;
    }

    const uint32_t worker_id = kernel.create_thread(
        k_worker, 0u, 8u, 0x400u);
    if (worker_id == 0u || !kernel.start_thread(worker_id) ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base,
                                 4u, k_dma_source) ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base + 4u,
                                 4u, 0x00010001u) ||
        !kernel.hardware().write(iop::c_hw::k_spu2_dma4_base + 8u,
                                 4u, 0x01000001u))
    {
        return false;
    }

    const iop::s_kernel_run_result ran = kernel.run(256u, 16u);
    uint32_t marker = 0u;
    uint32_t icr = 0u;
    const iop::s_kernel_thread* completed = kernel.thread(worker_id);
    return (ran.m_stop == iop::e_kernel_stop::idle ||
            ran.m_stop == iop::e_kernel_stop::budget_exhausted) &&
           completed != nullptr &&
           completed->m_status == iop::e_thread_status::dormant &&
           kernel.cpu().read32(k_marker, &marker) && marker == 1u &&
           kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) &&
           (icr & (1u << 28u)) == 0u &&
           !kernel.interrupts_suspended();
}


bool test_stopped_suspended_guest_releases_cpu()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!hle.install())
    {
        return false;
    }
    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return hle.handle_syscall(active, service);
    });
    uint32_t suspend_pc = 0u;
    if (!kernel.lookup_library("intrman", 17u, &suspend_pc))
    {
        return false;
    }
    constexpr uint32_t k_entry = 0x00044000u;
    const uint32_t loop[] =
    {
        0x24040000u, // addiu a0,zero,0
        0x0c000000u | ((suspend_pc >> 2u) & 0x03ffffffu),
        0u,
        0x08000000u | (((k_entry + 12u) >> 2u) & 0x03ffffffu),
        0u
    };
    for (uint32_t i = 0u; i < sizeof(loop) / sizeof(loop[0]); ++i)
    {
        if (!kernel.cpu().write32(k_entry + i * 4u, loop[i]))
        {
            return false;
        }
    }
    const uint32_t id = kernel.create_thread(k_entry, 0u, 8u, 0x400u);
    if (id == 0u || !kernel.start_thread(id))
    {
        return false;
    }
    const iop::s_kernel_run_result before = kernel.run(32u, 8u);
    if (before.m_stop != iop::e_kernel_stop::budget_exhausted ||
        !kernel.interrupts_suspended() ||
        !kernel.stop_thread(id))
    {
        return false;
    }
    const iop::s_kernel_thread* stopped = kernel.thread(id);
    return !kernel.interrupts_suspended() && stopped != nullptr &&
           stopped->m_status == iop::e_thread_status::dormant &&
           !stopped->m_interrupts_suspended;
}

bool test_cpu_suspend_pins_original_guest_thread()
{
    iop::c_kernel kernel;
    iop::c_kernel_hle_bridge hle(kernel);
    if (!hle.install())
    {
        return false;
    }
    kernel.bind_syscall([&](iop::c_kernel& current, uint32_t service)
    {
        return hle.handle_syscall(current, service);
    });
    uint32_t suspend_pc = 0u;
    uint32_t resume_pc = 0u;
    if (!kernel.lookup_library("intrman", 17u, &suspend_pc) ||
        !kernel.lookup_library("intrman", 18u, &resume_pc))
    {
        return false;
    }
    constexpr uint32_t original_pc = 0x00044000u;
    constexpr uint32_t peer_pc = 0x00045000u;
    constexpr uint32_t marker = 0x00047000u;
    const uint32_t original[] = {
        0x24040000u, // addiu a0,zero,0
        0x0c000000u | ((suspend_pc >> 2u) & 0x03ffffffu),
        0u,          // JAL delay slot
        0x3c080004u, // lui t0,4
        0x35087000u, // ori t0,t0,0x7000
        0x24090001u, // addiu t1,zero,1
        0xad090000u, // sw t1,0(t0)
        0x24040001u, // resume interrupts enabled
        0x0c000000u | ((resume_pc >> 2u) & 0x03ffffffu),
        0u,          // JAL delay slot
        0x3c0a0001u, // lui t2,1
        0x354a3fc0u, // ori t2,t2,thread return trampoline
        0x01400008u, // jr t2
        0u
    };
    const uint32_t peer[] = {
        0x3c080004u, // lui t0,4
        0x35087000u, // ori t0,t0,0x7000
        0x8d090000u, // lw t1,0(t0)
        0xad090004u, // sw t1,4(t0): record what peer observed
        0x03e00008u, // jr ra
        0u
    };
    for (uint32_t i = 0u;
         i < sizeof(original) / sizeof(original[0]); ++i)
    {
        if (!kernel.cpu().write32(original_pc + 4u * i, original[i]))
        {
            return false;
        }
    }
    for (uint32_t i = 0u;
         i < sizeof(peer) / sizeof(peer[0]); ++i)
    {
        if (!kernel.cpu().write32(peer_pc + 4u * i, peer[i]))
        {
            return false;
        }
    }
    const uint32_t first = kernel.create_thread(
        original_pc, 0u, 8u, 0x400u);
    const uint32_t second = kernel.create_thread(
        peer_pc, 0u, 8u, 0x400u);
    if (first == 0u || second == 0u ||
        !kernel.cpu().write32(marker, 0u) ||
        !kernel.cpu().write32(marker + 4u, 0xffffffffu) ||
        !kernel.start_thread(first) || !kernel.start_thread(second))
    {
        return false;
    }
    const iop::s_kernel_run_result done = kernel.run(256u, 8u);
    uint32_t value = 0u;
    uint32_t observed = 0u;
    const iop::s_kernel_thread* first_thread = kernel.thread(first);
    const iop::s_kernel_thread* peer_thread = kernel.thread(second);
    return done.m_stop == iop::e_kernel_stop::idle &&
           first_thread != nullptr && peer_thread != nullptr &&
           first_thread->m_status == iop::e_thread_status::dormant &&
           peer_thread->m_status == iop::e_thread_status::dormant &&
           kernel.cpu().read32(marker, &value) && value == 1u &&
           kernel.cpu().read32(marker + 4u, &observed) &&
           observed == 1u &&
           !kernel.interrupts_suspended();
}

} // namespace

int main()
{
    iop::c_kernel kernel;
    const auto synthetic = synthetic_sio2man_dependencies();

    // Before installation the real eight-library import set must fail
    // on the genuine first unresolved provider and remain atomic.
    const iop::s_module_load_result missing =
        kernel.load_module(synthetic);
    if (missing.m_status != iop::e_irx_load_result::unresolved_import ||
        missing.m_link_diagnostic.m_unresolved_library != "loadcore" ||
        missing.m_link_diagnostic.m_unresolved_ordinal != 6u ||
        missing.m_id != 0u)
    {
        return fail("original SIO2MAN imports did not fail closed");
    }

    iop::c_kernel_hle_bridge hle(kernel);
    if (!hle.install())
    {
        return fail("could not install kernel service-family stubs");
    }
    if (kernel.has_guest_work() ||
        hle.handle_syscall(kernel, 0xffffffffu))
    {
        return fail("merely installing HLE manufactured guest execution");
    }
    for (const s_imports& group : retail_dependencies())
    {
        for (uint16_t ordinal : group.m_ordinals)
        {
            uint32_t entry = 0u;
            uint32_t instruction = 0u;
            if (!kernel.lookup_library(
                    group.m_name, ordinal, &entry) ||
                entry < iop::c_kernel_hle_bridge::k_stub_base ||
                !kernel.cpu().read32(entry + 8u, &instruction) ||
                instruction != 0x0000000cu)
            {
                return fail("original SIO2MAN import missing guest stub");
            }
        }
    }
    uint32_t missing_export = 0u;
    if (kernel.lookup_library("dmacman", 16u, &missing_export))
    {
        return fail("unimplemented HLE import was falsely registered");
    }

    const iop::s_module_load_result linked =
        kernel.load_module(synthetic);
    if (linked.m_status != iop::e_irx_load_result::success ||
        linked.m_id != 1u ||
        kernel.module(1u) == nullptr ||
        kernel.module(1u)->m_irx.m_gp !=
            kernel.module(1u)->m_irx.m_base + 0x8fb0u)
    {
        return fail("complete original SIO2MAN dependency set did not link");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return hle.handle_syscall(active, service);
        }
    );

    for (uint16_t ordinal : {4u, 5u, 6u, 7u})
    {
        if (!test_vblank_wait(
                ordinal, ordinal == 4u || ordinal == 6u))
        {
            return fail("VBLANK wait did not block until the real matching edge");
        }
    }

    if (!test_cpu_suspend_pins_original_guest_thread())
    {
        return fail("CpuSuspendIntr allowed peer scheduling before resume");
    }
    if (!test_stopped_suspended_guest_releases_cpu())
    {
        return fail("stopping suspended guest retained global IRQ mask");
    }
    if (!test_registered_dma_guest_interrupt())
    {
        return fail("real DMA interrupt guest handler / IRQ context");
    }
    if (!test_suspended_waiter_dma4_irq_wakeup())
    {
        return fail("blocked CpuSuspendIntr waiter masked actual DMA4 IRQ");
    }
    if (!test_vblank_guest_callbacks())
    {
        return fail("VBLANK registered guest callback/edge/removal failed");
    }

    // ARKD's original LIBSD worker calls INTRMAN #7 DisableIntr
    // with a0=0x24 (primary DMAC channel 4). This is a real
    // R3000 imported syscall at 0x140E8, not an unsupported IRQ.
    // DMA sources must use the DICR/DICR2 enables, not an out-of-
    // range shift in I_MASK. The calls must not start or finish DMA.
    uint32_t dma_result = 0u;
    uint32_t mask = 0u;
    uint32_t icr = 0u;
    uint32_t icr2 = 0u;
    constexpr uint32_t k_dma4_line = 36u;
    constexpr uint32_t k_dma8_line = 41u;
    constexpr uint32_t k_saved_line = 0x2f80u;
    if (!kernel.cpu().write32(k_saved_line, 0xfeedfaceu) ||
        !call_guest(
            kernel, "intrman", 7u,
            {k_dma4_line, k_saved_line, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !kernel.cpu().read32(k_saved_line, &dma_result) ||
        dma_result != k_dma4_line ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) ||
        (icr & (1u << 20u)) != 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_stat, 4u, &dma_result) ||
        dma_result != 0u ||
        !call_guest(
            kernel, "intrman", 6u,
            {k_dma4_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) ||
        (icr & ((1u << 20u) | 0x00800000u)) !=
            ((1u << 20u) | 0x00800000u) ||
        !kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &mask) ||
        (mask & (1u << iop::c_hw::k_dma_irq)) == 0u ||
        !call_guest(
            kernel, "intrman", 7u,
            {k_dma4_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) ||
        (icr & (1u << 20u)) != 0u ||
        (icr & 0x00800000u) == 0u ||
        !call_guest(
            kernel, "intrman", 6u,
            {k_dma8_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr2, 4u, &icr2) ||
        (icr2 & (1u << 17u)) == 0u ||
        !call_guest(
            kernel, "intrman", 7u,
            {k_dma8_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr2, 4u, &icr2) ||
        (icr2 & (1u << 17u)) != 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_stat, 4u, &dma_result) ||
        dma_result != 0u)
    {
        return fail("INTRMAN guest DMA mask/import semantics failed");
    }
    if (!call_guest(
            kernel, "intrman", 4u,
            {k_dma4_line, 0u, 0x3000u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !call_guest(
            kernel, "intrman", 5u,
            {k_dma4_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u ||
        !call_guest(
            kernel, "intrman", 5u,
            {k_dma4_line, 0u, 0u, 0u}, &dma_result) ||
        dma_result != 0u)
    {
        return fail("INTRMAN DMA handler registration/release failed");
    }
    // Reject invalid interrupt numbers and unwriteable output pointers
    // without modifying any mask, handler or DMA status.
    iop::s_cpu_state& interrupt_cpu = kernel.cpu().state();
    interrupt_cpu.m_gpr[4] = 39u;
    interrupt_cpu.m_gpr[5] = 0u;
    if (hle.handle_syscall(
            kernel, iop::c_kernel_hle_bridge::k_service_base +
                2u * 0x100u + 7u))
    {
        return fail("reserved DMA interrupt line 39 was accepted");
    }
    interrupt_cpu.m_gpr[4] = k_dma4_line;
    interrupt_cpu.m_gpr[5] = 0xf0000000u;
    if (hle.handle_syscall(
            kernel, iop::c_kernel_hle_bridge::k_service_base +
                2u * 0x100u + 7u) ||
        !kernel.hardware().read(iop::c_hw::k_dma_icr, 4u, &icr) ||
        (icr & (1u << 20u)) != 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_stat, 4u, &dma_result) ||
        dma_result != 0u)
    {
        return fail("invalid INTRMAN output pointer altered DMA mask");
    }

    uint32_t result = 0u;
    if (!call_guest(
            kernel, "thbase", 20u, {0u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("real guest R3000 GetThreadId import did not run");
    }


    // MODLOAD introspection operates on the actual linked IRX registry.
    // Probe the six implemented ordinals through guest R3000 stubs.
    uint32_t modload_pc = 0u;
    for (uint16_t ordinal : {10u, 13u, 16u, 17u, 18u, 22u})
    {
        if (!kernel.lookup_library("modload", ordinal, &modload_pc))
        {
            return fail("implemented MODLOAD import was not registered");
        }
    }
    for (uint16_t ordinal : {7u, 8u})
    {
        if (kernel.lookup_library("modload", ordinal, &modload_pc))
        {
            return fail("unsupported MODLOAD start/wait import registered");
        }
    }
    kernel.cpu().write32(0x2b00u, 0x0000000du);
    if (!call_guest(
            kernel, "modload", 13u, {0x2b00u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "modload", 13u, {3u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-201))
    {
        return fail("MODLOAD callback registration validation failed");
    }
    if (!call_guest(
            kernel, "modload", 16u,
            {0x2b10u, 4u, 0x2b20u, 0u}, &result) ||
        result != 0u)
    {
        return fail("MODLOAD module ID list call failed");
    }
    uint32_t module_id = 0u;
    uint32_t module_count = 0u;
    if (!kernel.cpu().read32(0x2b10u, &module_id) ||
        !kernel.cpu().read32(0x2b20u, &module_count) ||
        module_id != 1u || module_count != 1u ||
        !call_guest(
            kernel, "modload", 17u,
            {module_id, 0x2b40u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("MODLOAD module list and status failed");
    }
    uint32_t status_id = 0u;
    uint32_t status_entry = 0u;
    if (!kernel.cpu().read32(0x2b40u + 0x38u, &status_id) ||
        !kernel.cpu().read32(0x2b40u + 0x40u, &status_entry) ||
        status_id != module_id ||
        status_entry != kernel.module(module_id)->m_irx.m_entry)
    {
        return fail("MODLOAD status does not reflect the IRX");
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        const char name[] = "irx";
        kernel.cpu().write8(0x2ba0u + i,
                            static_cast<uint8_t>(name[i]));
    }
    if (!call_guest(
            kernel, "modload", 22u,
            {0x2ba0u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "modload", 18u,
            {0x2ba0u, 0x2bb0u, 4u, 0x2bc0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2bb0u, &module_id) ||
        !kernel.cpu().read32(0x2bc0u, &module_count) ||
        module_id != 1u || module_count != 1u)
    {
        return fail("MODLOAD case-insensitive name search failed");
    }
    if (!call_guest(
            kernel, "modload", 17u,
            {0xffffffffu, 0x2b40u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-202))
    {
        return fail("MODLOAD unknown module did not fail closed");
    }


    // Link a second synthetic IRX from IOP RAM. LoadModuleBuffer must
    // not execute an entrypoint, and the registry must reflect the load.
    const uint32_t guest_image = 0x6000u;
    for (uint32_t i = 0u; i < synthetic.size(); ++i)
    {
        if (!kernel.cpu().write8(guest_image + i, synthetic[i]))
        {
            return fail("could not stage guest IRX buffer");
        }
    }
    if (!call_guest(
            kernel, "modload", 10u,
            {guest_image, 0u, 0u, 0u}, &result) ||
        result != 2u || kernel.module(2u) == nullptr ||
        !call_guest(
            kernel, "modload", 16u,
            {0x2b10u, 1u, 0x2b20u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2b20u, &module_count) ||
        module_count != 2u)
    {
        return fail("MODLOAD buffer linking/count/truncation failed");
    }


    // Actual, read-only IOMAN guest file operations use the same
    // ISO9660 folder sector source as native CDVD HLE.
    namespace fs = std::filesystem;
    const fs::path media = fs::temp_directory_path() /
        "sdbz_ioman_hle_regression";
    std::error_code media_error;
    fs::remove_all(media, media_error);
    media_error.clear();
    fs::create_directories(media, media_error);
    if (media_error)
    {
        return fail("could not create IOMAN media fixture");
    }
    {
        std::ofstream file(media / "HELLO.BIN", std::ios::binary);
        file << "SDBZ DATA";
        if (!file)
        {
            return fail("could not write IOMAN media fixture");
        }
    }
    iop::c_folder_sector_source sector_source(media);
    if (!sector_source.valid())
    {
        return fail("could not mount IOMAN media fixture");
    }
    hle.attach_disc_source(&sector_source);
    const char ioman_path[] = "cdrom0:\\HELLO.BIN;1";
    for (uint32_t i = 0u; i < sizeof(ioman_path); ++i)
    {
        if (!kernel.cpu().write8(
                0x3000u + i, static_cast<uint8_t>(ioman_path[i])))
        {
            return fail("could not install IOMAN path");
        }
    }
    for (uint16_t ordinal : {4u, 5u, 6u, 7u, 8u, 16u, 20u, 21u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("ioman", ordinal, &entry))
        {
            return fail("IOMAN supported/safe boundary was not installed");
        }
    }
    for (uint16_t ordinal : {4u, 5u, 6u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("secrman", ordinal, &entry))
        {
            return fail("SECRMAN explicit failed boundary not installed");
        }
    }
    if (!call_guest(
            kernel, "ioman", 16u, {0x3000u, 0x3100u, 0u, 0u},
            &result) || result != 0u)
    {
        return fail("IOMAN getstat failed");
    }
    uint32_t file_mode = 0u;
    uint32_t file_size = 0u;
    if (!kernel.cpu().read32(0x3100u, &file_mode) ||
        !kernel.cpu().read32(0x3108u, &file_size) ||
        file_mode != 0x17u || file_size != 9u)
    {
        return fail("IOMAN getstat wrong native ISO metadata");
    }
    if (!call_guest(
            kernel, "ioman", 4u, {0x3000u, 1u, 0u, 0u},
            &result) || result != 3u)
    {
        return fail("IOMAN open failed");
    }
    if (!call_guest(
            kernel, "ioman", 6u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != 4u ||
        !call_guest(
            kernel, "ioman", 8u, {3u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "ioman", 6u, {3u, 0x3148u, 9u, 0u}, &result) ||
        result != 9u)
    {
        return fail("IOMAN read/seek failed");
    }
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        uint8_t byte = 0u;
        const char expected[] = "SDBZ DATA";
        if (!kernel.cpu().read8(0x3148u + i, &byte) ||
            byte != static_cast<uint8_t>(expected[i]))
        {
            return fail("IOMAN disc bytes mismatch");
        }
    }
    if (!call_guest(
            kernel, "ioman", 7u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != static_cast<uint32_t>(-13) ||
        !call_guest(
            kernel, "ioman", 5u, {3u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "ioman", 6u, {3u, 0x3140u, 4u, 0u}, &result) ||
        result != static_cast<uint32_t>(-9))
    {
        return fail("IOMAN failed close/readonly/EBADF semantics");
    }
    if (!call_guest(
            kernel, "ioman", 20u, {0x3100u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "ioman", 21u, {0x3000u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 4u, {0u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 5u, {0u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-1) ||
        !call_guest(
            kernel, "secrman", 6u, {0u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("IOMAN driver or SECRMAN authentication falsely succeeded");
    }
    for (uint16_t ordinal : {9u, 13u, 15u})
    {
        uint32_t entry = 0u;
        if (kernel.lookup_library("ioman", ordinal, &entry))
        {
            return fail("unimplemented IOMAN operation was registered");
        }
    }
    hle.attach_disc_source(nullptr);
    fs::remove_all(media, media_error);

    // The next game loader IRX (MCMAN) requires threadman timing,
    // not merely the original SIO2MAN thread service imports.
    for (uint16_t ordinal : {33u, 34u, 35u, 37u, 39u, 40u})
    {
        uint32_t entry = 0u;
        if (!kernel.lookup_library("thbase", ordinal, &entry) ||
            entry < iop::c_kernel_hle_bridge::k_stub_base)
        {
            return fail("MCMAN timer dependency was not registered");
        }
    }
    for (uint16_t ordinal : {36u, 38u})
    {
        uint32_t entry = 0u;
        if (kernel.lookup_library("thbase", ordinal, &entry))
        {
            return fail("unsupported interrupt-context timer was registered");
        }
    }

    // Exact PS2SDK clock conversion uses 4608/125 ticks per usec.
    // The virtual current clock and wake scheduling are deliberately
    // not claimed to be calibrated to real IOP hardware cycles.
    kernel.cpu().write32(0x2a00u, 0u);
    kernel.cpu().write32(0x2a04u, 0u);
    if (!call_guest(
            kernel, "thbase", 39u, {125u, 0x2a00u, 0u, 0u},
            &result))
    {
        return fail("USec2SysClock guest import did not execute");
    }
    uint32_t low = 0u;
    uint32_t high = 0u;
    uint32_t sec = 0u;
    uint32_t usec = 0u;
    if (!kernel.cpu().read32(0x2a00u, &low) ||
        !kernel.cpu().read32(0x2a04u, &high) ||
        low != 4608u || high != 0u ||
        !call_guest(
            kernel, "thbase", 40u,
            {0x2a00u, 0x2a08u, 0x2a0cu, 0u}, &result) ||
        !kernel.cpu().read32(0x2a08u, &sec) ||
        !kernel.cpu().read32(0x2a0cu, &usec) ||
        sec != 0u || usec != 125u ||
        !call_guest(
            kernel, "thbase", 34u,
            {0x2a10u, 0u, 0u, 0u}, &result) ||
        !kernel.cpu().read32(0x2a10u, &low) ||
        !kernel.cpu().read32(0x2a14u, &high) ||
        (low == 0u && high == 0u) ||
        !call_guest(
            kernel, "thbase", 33u, {1u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THBASE clock conversion, time or delay failed");
    }

    // A timer's effect must come from executable guest R3000 callback
    // instructions, not an artificial ready bit or ignored completion.
    const uint32_t callback_words[] = {
        0x3c080000u, // lui t0, 0
        0x35082a28u, // ori t0, t0, 0x2a28
        0x24090001u, // addiu t1, zero, 1
        0xad090000u, // sw t1, 0(t0)
        0x00001021u, // addu v0, zero, zero; disarm timer
        0x03e00008u, // jr ra
        0x00000000u
    };
    for (uint32_t i = 0u;
         i < sizeof(callback_words) / sizeof(callback_words[0]); ++i)
    {
        if (!kernel.cpu().write32(
                0x3000u + i * 4u, callback_words[i]))
        {
            return fail("cannot prepare original guest alarm callback");
        }
    }
    kernel.cpu().write32(0x2a20u, 1000u);
    kernel.cpu().write32(0x2a24u, 0u);
    kernel.cpu().write32(0x2a28u, 0u);
    if (!call_guest(
            kernel, "thbase", 35u,
            {0x2a20u, 0x3000u, 0u, 0u}, &result) ||
        result != 0u || !kernel.has_guest_work())
    {
        return fail("SetAlarm did not schedule a guest callback");
    }
    const iop::s_kernel_run_result timer_run = kernel.run(2048u);
    if (timer_run.m_stop != iop::e_kernel_stop::idle ||
        !kernel.cpu().read32(0x2a28u, &low) ||
        low != 1u || kernel.has_guest_work())
    {
        return fail("alarm callback did not execute and disarm");
    }

    kernel.cpu().write32(0x2a20u, 5000u);
    kernel.cpu().write32(0x2a28u, 0u);
    if (!call_guest(
            kernel, "thbase", 35u,
            {0x2a20u, 0x3000u, 0u, 0u}, &result) ||
        !call_guest(
            kernel, "thbase", 37u,
            {0x3000u, 0u, 0u, 0u}, &result) ||
        result != 0u || kernel.has_guest_work() ||
        !kernel.cpu().read32(0x2a28u, &low) || low != 0u)
    {
        return fail("CancelAlarm did not disarm the scheduled callback");
    }

    // An earlier alarm may be inserted after an existing later alarm,
    // including from a due HLE callback. Preserve the exact deadlines
    // and cancellation semantics while avoiding future alarm scans.
    kernel.cpu().write32(0x2a28u, 0u);
    const uint64_t alarm_start = kernel.cycles();
    bool inserted = false;
    if (!kernel.set_alarm(100u, 0x3000u, 0u, 0u) ||
        !kernel.schedule_hle_event(10u, [&]()
        {
            inserted = kernel.set_alarm(5u, 0x3000u, 1u, 0u);
        }) ||
        kernel.run(15u, 1u).m_executed != 0u ||
        !inserted || kernel.cycles() != alarm_start + 15u ||
        !kernel.cpu().read32(0x2a28u, &low) || low != 0u)
    {
        return fail("future alarm hint changed event timing");
    }
    // The later alarm is still pending, so advancing to the end of
    // this slice exhausts its budget rather than reporting idle.
    if (kernel.run(20u, 1u).m_stop != iop::e_kernel_stop::budget_exhausted ||
        kernel.cycles() != alarm_start + 35u ||
        !kernel.cpu().read32(0x2a28u, &low) || low != 1u ||
        !kernel.has_guest_work() ||
        !kernel.cancel_alarm(0x3000u, 0u) ||
        kernel.has_guest_work())
    {
        return fail("earlier alarm insertion or cancellation changed idle timing");
    }
    const iop::s_kernel_run_result cancelled_run = kernel.run(100u, 1u);
    if (cancelled_run.m_stop != iop::e_kernel_stop::idle ||
        cancelled_run.m_executed != 0u ||
        kernel.cycles() != alarm_start + 135u || kernel.has_guest_work())
    {
        return fail("earlier alarm insertion or cancellation changed idle timing");
    }

    // Bounded guest-memory strncmp, lookup_ctype and strtol operate
    // through actual R3000 import stubs, not direct host-only calls.
    const std::string lhs = "ABC";
    const std::string rhs = "ABD";
    for (size_t i = 0u; i <= lhs.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2200u + static_cast<uint32_t>(i),
                i == lhs.size() ? 0u :
                    static_cast<uint8_t>(lhs[i])) ||
            !kernel.cpu().write8(
                0x2300u + static_cast<uint32_t>(i),
                i == rhs.size() ? 0u :
                    static_cast<uint8_t>(rhs[i])))
        {
            return fail("cannot prepare guest strings");
        }
    }
    if (!call_guest(
            kernel, "sysclib", 29u,
            {0x2200u, 0x2300u, 3u, 0u}, &result) ||
        static_cast<int32_t>(result) != -1 ||
        !call_guest(
            kernel, "sysclib", 8u,
            {static_cast<uint32_t>('A'), 0u, 0u, 0u}, &result) ||
        (result & 1u) == 0u)
    {
        return fail("guest strncmp or character classification failed");
    }

    const std::string number = " -42foo";
    for (size_t i = 0u; i <= number.size(); ++i)
    {
        kernel.cpu().write8(
            0x2400u + static_cast<uint32_t>(i),
            i == number.size() ? 0u :
                static_cast<uint8_t>(number[i]));
    }
    uint32_t consumed = 0u;
    if (!call_guest(
            kernel, "sysclib", 36u,
            {0x2400u, 0x2500u, 10u, 0u}, &result) ||
        static_cast<int32_t>(result) != -42 ||
        !kernel.cpu().read32(0x2500u, &consumed) ||
        consumed != 0x2404u)
    {
        return fail("guest strtol or end pointer was incorrect");
    }

    // EventFlag and semaphore are distinct IOP-owned objects; they
    // cannot be conjured merely by the EE reading SIF flags.
    kernel.cpu().write32(0x2600u, 0u);
    kernel.cpu().write32(0x2604u, 0u);
    kernel.cpu().write32(0x2608u, 0u);
    if (!call_guest(
            kernel, "thevent", 4u,
            {0x2600u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "thevent", 6u,
            {1u, 0x20u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 10u,
            {1u, 0x20u, 0u, 0x2620u}, &result) ||
        result != 0u)
    {
        return fail("guest event creation, signal or wait failed");
    }
    uint32_t event_bits = 0u;
    if (!kernel.cpu().read32(0x2620u, &event_bits) ||
        event_bits != 0x20u)
    {
        return fail("event flag did not report actual guest bits");
    }

    kernel.cpu().write32(0x2700u, 0u);
    kernel.cpu().write32(0x2704u, 0u);
    kernel.cpu().write32(0x2708u, 1u);
    kernel.cpu().write32(0x270cu, 2u);
    if (!call_guest(
            kernel, "thsemap", 4u,
            {0x2700u, 0u, 0u, 0u}, &result) ||
        result != 2u ||
        !call_guest(
            kernel, "thsemap", 8u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 6u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 11u,
            {2u, 0x2740u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("real guest semaphore calls did not complete");
    }
    uint32_t remaining = 0u;
    if (!kernel.cpu().read32(0x2750u, &remaining) ||
        remaining != 1u)
    {
        return fail("refer semaphore status reported false count");
    }

    // Polling does not block, status describes actual objects, and a
    // successfully destroyed object cannot be polled again.
    if (!call_guest(
            kernel, "thevent", 11u,
            {1u, 0x20u, 0u, 0x2660u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 12u,
            {1u, 0x40u, 0u, 0x2660u}, &result) ||
        result != static_cast<uint32_t>(-421) ||
        !call_guest(
            kernel, "thevent", 13u,
            {1u, 0x2680u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x268cu, &event_bits) ||
        event_bits != 0x20u ||
        !call_guest(
            kernel, "thevent", 9u,
            {1u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thevent", 14u,
            {1u, 0x2680u, 0u, 0u}, &result) ||
        !kernel.cpu().read32(0x268cu, &event_bits) ||
        event_bits != 0u ||
        !call_guest(
            kernel, "thevent", 5u,
            {1u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("event polling, status or delete failed");
    }
    if (!call_guest(
            kernel, "thsemap", 9u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 9u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-419) ||
        !call_guest(
            kernel, "thsemap", 7u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thsemap", 12u,
            {2u, 0x2780u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2790u, &remaining) ||
        remaining != 1u ||
        !call_guest(
            kernel, "thsemap", 5u,
            {2u, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("semaphore polling, status or delete failed");
    }

    // The primary ps2-web-recomp/SDBZ IOP kernel returns precise
    // unknown-object errors for real imported services, including the
    // thsemap #8 WaitSema reached by the ARKD_DVD guest RPC worker.
    // These are guest API failures, not unsupported host syscalls.
    // A missing object must not be created, signalled or waited on.
    struct s_missing_object_case
    {
        const char* m_library;
        uint16_t m_ordinal;
        std::array<uint32_t, 4> m_args;
        uint32_t m_error;
    };
    const s_missing_object_case missing_objects[] =
    {
        {"thsemap", 5u, {2u, 0u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 6u, {2u, 0u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 7u, {2u, 0u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 8u, {2u, 0u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 9u, {2u, 0u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 11u, {2u, 0x2740u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thsemap", 12u, {2u, 0x2740u, 0u, 0u},
            static_cast<uint32_t>(-408)},
        {"thevent", 6u, {1u, 0x20u, 0u, 0u},
            static_cast<uint32_t>(-409)},
        {"thevent", 10u, {1u, 0x20u, 0u, 0x2620u},
            static_cast<uint32_t>(-409)},
        {"thevent", 13u, {1u, 0x2680u, 0u, 0u},
            static_cast<uint32_t>(-409)},
        {"thmsgbx", 5u, {0xfffffffeu, 0u, 0u, 0u},
            static_cast<uint32_t>(-410)},
        {"thmsgbx", 8u, {0x2400u, 0xfffffffeu, 0u, 0u},
            static_cast<uint32_t>(-410)},
        {"thvpool", 6u, {0xfffffffeu, 16u, 0u, 0u},
            static_cast<uint32_t>(-411)},
        {"thvpool", 11u, {0xfffffffeu, 0x2680u, 0u, 0u},
            static_cast<uint32_t>(-411)},
        {"thfpool", 6u, {0xfffffffeu, 0u, 0u, 0u},
            static_cast<uint32_t>(-412)},
        {"thfpool", 9u, {0xfffffffeu, 0x2500u, 0u, 0u},
            static_cast<uint32_t>(-412)}
    };
    for (const s_missing_object_case& item : missing_objects)
    {
        if (!call_guest(
                kernel, item.m_library, item.m_ordinal,
                item.m_args, &result) ||
            result != item.m_error ||
            kernel.hardware().pending_interrupts() != 0u)
        {
            return fail("unknown IOP object did not return guest error");
        }
    }

    // Cross-check the additional reference-backed families against
    // the actual guest-visible IOP allocation and thread state.
    uint32_t total_free = 0u;
    if (!call_guest(
            kernel, "sysmem", 6u, {0u, 0u, 0u, 0u}, &result) ||
        result != iop::c_cpu::k_ram_size ||
        !call_guest(
            kernel, "sysmem", 8u, {0u, 0u, 0u, 0u}, &result))
    {
        return fail("SYS_MEM capacity import failed");
    }
    total_free = result;
    if (!call_guest(
            kernel, "sysmem", 4u, {0u, 64u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("SYS_MEM guest allocation failed");
    }
    const uint32_t allocated = result;
    if (!call_guest(
            kernel, "sysmem", 5u,
            {allocated, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "sysmem", 7u, {0u, 0u, 0u, 0u}, &result) ||
        result == 0u ||
        !call_guest(
            kernel, "loadcore", 4u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u ||
        !call_guest(
            kernel, "loadcore", 5u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u ||
        !call_guest(
            kernel, "intrman", 23u, {0u, 0u, 0u, 0u},
            &result) ||
        result != 0u)
    {
        return fail("SYS_MEM free, coherent cache or context query failed");
    }
    // Each call_guest creates its own program thread stack, so total
    // free memory decreases across calls independently of FreeSysMemory.
    if (total_free == 0u)
    {
        return fail("SYS_MEM queried an impossible free-memory total");
    }

    kernel.cpu().write32(0x2b00u, 0x02000000u);
    kernel.cpu().write32(0x2b04u, 0x13572468u);
    kernel.cpu().write32(0x2b08u, 0x3000u);
    kernel.cpu().write32(0x2b0cu, 0x400u);
    kernel.cpu().write32(0x2b10u, 32u);
    if (!call_guest(
            kernel, "thbase", 4u,
            {0x2b00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("guest thread creation import failed");
    }
    const uint32_t managed_thread = result;
    uint32_t thread_status = 0u;
    uint32_t priority = 0u;
    uint32_t attributes = 0u;
    uint32_t options = 0u;
    if (!call_guest(
            kernel, "thbase", 14u,
            {managed_thread, 16u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 22u,
            {managed_thread, 0x2b40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2b40u, &attributes) ||
        attributes != 0x02000000u ||
        !kernel.cpu().read32(0x2b44u, &options) ||
        options != 0x13572468u ||
        !kernel.cpu().read32(0x2b48u, &thread_status) ||
        thread_status != 16u ||
        !kernel.cpu().read32(0x2b60u, &priority) ||
        priority != 16u ||
        !call_guest(
            kernel, "thbase", 26u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 27u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !call_guest(
            kernel, "thbase", 10u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thbase", 5u,
            {managed_thread, 0u, 0u, 0u}, &result) ||
        result != 0u || kernel.thread(managed_thread) != nullptr)
    {
        return fail("THBASE guest thread management or status failed");
    }

    // Reference IOP mailbox and heaplib services are backed by real
    // queues and owned IOP guest allocations, not success-only stubs.
    kernel.cpu().write32(0x2c00u, 0u);
    kernel.cpu().write32(0x2c04u, 0u);
    kernel.cpu().write32(0x2c10u, 0u);
    if (!call_guest(
            kernel, "thmsgbx", 4u, {0x2c00u, 0u, 0u, 0u},
            &result) || result == 0u)
    {
        return fail("mailbox creation failed");
    }
    const uint32_t mailbox = result;
    if (!call_guest(
            kernel, "thmsgbx", 9u,
            {0x2c20u, mailbox, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-424) ||
        !call_guest(
            kernel, "thmsgbx", 6u,
            {mailbox, 0x2c10u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thmsgbx", 9u,
            {0x2c20u, mailbox, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2c20u, &remaining) ||
        remaining != 0x2c10u ||
        !call_guest(
            kernel, "thmsgbx", 5u,
            {mailbox, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("guest mailbox polling, delivery or deletion failed");
    }
    if (!call_guest(
            kernel, "heaplib", 4u, {0u, 0u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("heaplib create did not allocate a real object");
    }
    const uint32_t heap_id = result;
    if (!call_guest(
            kernel, "heaplib", 6u,
            {heap_id, 128u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base ||
        !kernel.owns_heap_span(result, 128u))
    {
        return fail("heaplib allocation did not reserve IOP memory");
    }
    const uint32_t block = result;
    if (!call_guest(
            kernel, "heaplib", 7u,
            {heap_id, block, 0u, 0u}, &result) ||
        result != 0u ||
        kernel.owns_heap_span(block, 1u) ||
        !call_guest(
            kernel, "heaplib", 5u,
            {heap_id, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        kernel.owns_heap_span(heap_id, 1u))
    {
        return fail("heaplib free or delete failed");
    }

    // A fixed pool only returns blocks it owns and rejects an
    // additional allocation after its last free slot is consumed.
    kernel.cpu().write32(0x2d00u, 0u);
    kernel.cpu().write32(0x2d04u, 0u);
    kernel.cpu().write32(0x2d08u, 16u);
    kernel.cpu().write32(0x2d0cu, 2u);
    if (!call_guest(
            kernel, "thfpool", 4u,
            {0x2d00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("THFPOOL guest creation failed");
    }
    const uint32_t fixed_pool = result;
    if (!call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("THFPOOL first allocation failed");
    }
    const uint32_t fixed_block_a = result;
    if (!call_guest(
            kernel, "thfpool", 8u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != fixed_block_a + 16u ||
        !call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thfpool", 9u,
            {fixed_pool, fixed_block_a, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thfpool", 7u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != fixed_block_a ||
        !call_guest(
            kernel, "thfpool", 5u,
            {fixed_pool, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THFPOOL exhaustion, release or deletion failed");
    }
    kernel.cpu().write32(0x2e00u, 0u);
    kernel.cpu().write32(0x2e04u, 0u);
    kernel.cpu().write32(0x2e08u, 64u);
    if (!call_guest(
            kernel, "thvpool", 4u,
            {0x2e00u, 0u, 0u, 0u}, &result) ||
        result == 0u)
    {
        return fail("THVPOOL guest creation failed");
    }
    const uint32_t variable_pool = result;
    if (!call_guest(
            kernel, "thvpool", 7u,
            {variable_pool, 16u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("THVPOOL first allocation failed");
    }
    const uint32_t variable_block = result;
    if (!call_guest(
            kernel, "thvpool", 11u,
            {variable_pool, 0x2e40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2e4cu, &remaining) ||
        remaining != 48u ||
        !call_guest(
            kernel, "thvpool", 9u,
            {variable_pool, variable_block, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "thvpool", 12u,
            {variable_pool, 0x2e40u, 0u, 0u}, &result) ||
        result != 0u ||
        !kernel.cpu().read32(0x2e4cu, &remaining) ||
        remaining != 64u ||
        !call_guest(
            kernel, "thvpool", 5u,
            {variable_pool, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("THVPOOL status, release or deletion failed");
    }

    // SYS_MEM block metadata comes from actual allocator ownership,
    // including interior pointers; non-owned addresses have no block.
    if (!call_guest(
            kernel, "sysmem", 4u,
            {0u, 64u, 0u, 0u}, &result) ||
        result < iop::c_kernel::k_user_base)
    {
        return fail("second SYS_MEM allocation failed");
    }
    const uint32_t owned_block = result;
    if (!call_guest(
            kernel, "sysmem", 9u,
            {owned_block + 4u, 0u, 0u, 0u}, &result) ||
        result != owned_block ||
        !call_guest(
            kernel, "sysmem", 10u,
            {owned_block + 4u, 0u, 0u, 0u}, &result) ||
        result != 64u ||
        !call_guest(
            kernel, "sysmem", 5u,
            {owned_block, 0u, 0u, 0u}, &result) ||
        result != 0u ||
        !call_guest(
            kernel, "sysmem", 9u,
            {owned_block, 0u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("SYS_MEM ownership/block metadata did not match");
    }

    const std::string strtok_input = "aaa,bb";
    for (size_t i = 0u; i <= strtok_input.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2f00u + static_cast<uint32_t>(i),
                i == strtok_input.size() ? 0u :
                static_cast<uint8_t>(strtok_input[i])))
        {
            return fail("cannot initialize SYSCLIB token input");
        }
    }
    kernel.cpu().write8(0x2f40u, ',');
    kernel.cpu().write8(0x2f41u, 0u);
    if (!call_guest(
            kernel, "sysclib", 35u,
            {0x2f00u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0x2f00u ||
        !call_guest(
            kernel, "sysclib", 35u,
            {0u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0x2f04u ||
        !call_guest(
            kernel, "sysclib", 35u,
            {0u, 0x2f40u, 0u, 0u}, &result) ||
        result != 0u)
    {
        return fail("bounded SYSCLIB strtok continuation was incorrect");
    }
    const std::string unsigned_input = "4294967295tail";
    for (size_t i = 0u; i <= unsigned_input.size(); ++i)
    {
        if (!kernel.cpu().write8(
                0x2f60u + static_cast<uint32_t>(i),
                i == unsigned_input.size() ? 0u :
                static_cast<uint8_t>(unsigned_input[i])))
        {
            return fail("cannot initialize SYSCLIB unsigned input");
        }
    }
    uint32_t unsigned_end = 0u;
    if (!call_guest(
            kernel, "sysclib", 38u,
            {0x2f60u, 0x2f90u, 10u, 0u}, &result) ||
        result != 0xffffffffu ||
        !kernel.cpu().read32(0x2f90u, &unsigned_end) ||
        unsigned_end != 0x2f6au ||
        !call_guest(
            kernel, "sysclib", 41u,
            {0x2fa0u, 0x12345678u, 8u, 0u}, &result) ||
        result != 0x2fa0u ||
        !call_guest(
            kernel, "sysclib", 40u,
            {0x2fb0u, 0x2fa0u, 8u, 0u}, &result) ||
        result != 0x2fb0u ||
        !kernel.cpu().read32(0x2fb0u, &remaining) ||
        remaining != 0x12345678u ||
        !kernel.cpu().read32(0x2fb4u, &remaining) ||
        remaining != 0x12345678u)
    {
        return fail("SYSCLIB strtoul or word memory operations failed");
    }


    // The real CRI_ADXI IRX imports SYSCLIB sprintf #19. Its
    // first variable arguments occupy O32 a2/a3, with further
    // arguments in the guest stack; vsprintf #42 receives a guest
    // va_list. Never pass guest pointers to the host formatter.
    const auto write_guest_string =
        [&kernel](uint32_t address, const std::string& value)
    {
        for (uint32_t i = 0u; i <= value.size(); ++i)
        {
            if (!kernel.cpu().write8(
                    address + i,
                    i == value.size() ? 0u :
                        static_cast<uint8_t>(value[i])))
            {
                return false;
            }
        }
        return true;
    };
    const auto check_guest_string =
        [&kernel](uint32_t address, const std::string& value)
    {
        for (uint32_t i = 0u; i <= value.size(); ++i)
        {
            uint8_t actual = 0xffu;
            if (!kernel.cpu().read8(address + i, &actual) ||
                actual != (i == value.size() ? 0u :
                           static_cast<uint8_t>(value[i])))
            {
                return false;
            }
        }
        return true;
    };

    constexpr uint32_t k_sprintf_destination = 0x3400u;
    constexpr uint32_t k_sprintf_format = 0x3480u;
    constexpr uint32_t k_sprintf_string = 0x34c0u;
    constexpr uint32_t k_vsprintf_format = 0x3500u;
    constexpr uint32_t k_vsprintf_argument_list = 0x3580u;
    constexpr uint32_t k_vsprintf_destination = 0x3600u;

    // STDIO #4 must share SYSCLIB's validated R3000 O32 formatter.
    // The retail ARKD method-1 worker reached this import after
    // millions of real byte operations. A previously narrower printf
    // implementation rejected legitimate width/precision modifiers.
    constexpr uint32_t k_printf_format = 0x3680u;
    constexpr uint32_t k_printf_text = 0x36e0u;
    uint32_t printf_result = 0u;
    const std::string expected_printf = "blk:buf   /0x00002a/-7\n";
    // The test kernel was deliberately unattached during import/link
    // checks. Guest printf writes the real IOP debug-console MMIO,
    // which requires the normal HLE hardware binding.
    if (!kernel.attach_hle_hardware())
    {
        return fail("could not attach real IOP debug console for STDIO");
    }
    if (!write_guest_string(k_printf_format,
            "blk:%-6.3s/%#08x/%+ld\n") ||
        !write_guest_string(k_printf_text, "buffers") ||
        !call_guest(kernel, "stdio", 4u,
            {k_printf_format, k_printf_text, 0x2au, 0xfffffff9u},
            &printf_result) ||
        printf_result != expected_printf.size())
    {
        return fail("STDIO printf guest flags/precision/long formatting failed");
    }
    const std::string& printed = kernel.hardware().debug_console();
    if (printed.size() < expected_printf.size() ||
        printed.compare(printed.size() - expected_printf.size(),
                        expected_printf.size(), expected_printf) != 0)
    {
        return fail("STDIO printf did not emit expected guest debug text");
    }
    // O32 long long varargs consume an aligned 32-bit argument pair.
    if (!write_guest_string(k_printf_format, "%08llX") ||
        !call_guest(kernel, "stdio", 4u,
            {k_printf_format, 0xabcdef01u, 0x89abcdefu, 0x12345678u},
            &printf_result) || printf_result != 16u ||
        kernel.hardware().debug_console().size() <
            expected_printf.size() + 16u ||
        kernel.hardware().debug_console().substr(
            kernel.hardware().debug_console().size() - 16u) !=
                "1234567889ABCDEF")
    {
        return fail("STDIO printf O32 64-bit vararg alignment failed");
    }
    // Unsafe printf formats must fail before printing or changing v0.
    if (!write_guest_string(k_printf_format, "unsafe:%n"))
    {
        return fail("could not prepare unsafe STDIO printf test");
    }
    kernel.cpu().state().m_gpr[4] = k_printf_format;
    kernel.cpu().state().m_gpr[5] = k_printf_text;
    kernel.cpu().state().m_gpr[2] = 0x1234abcdu;
    std::string rejected_printf = "preserve";
    if (iop::render_stdio_format(kernel.cpu(), &rejected_printf) ||
        rejected_printf != "preserve" ||
        kernel.cpu().state().m_gpr[2] != 0x1234abcdu)
    {
        return fail("unsafe STDIO printf format was accepted or mutated state");
    }

    const std::string expected_sprintf = "mod=002A/audio";
    if (!write_guest_string(
            k_sprintf_format, "mod=%04X/%s") ||
        !write_guest_string(k_sprintf_string, "audio") ||
        !call_guest(
            kernel, "sysclib", 19u,
            {k_sprintf_destination, k_sprintf_format,
             0x2au, k_sprintf_string}, &result) ||
        result != expected_sprintf.size() ||
        !check_guest_string(
            k_sprintf_destination, expected_sprintf))
    {
        return fail("guest SYSCLIB sprintf O32 a2/a3 formatting failed");
    }

    // A third sprintf variable argument must be taken from O32
    // sp+16, not a fictitious host-side variadic argument.
    constexpr uint32_t k_stack_format_program = 0x3b40u;
    uint32_t sprintf_stub = 0u;
    const uint32_t format_program[] = {
        0x34060004u, // ori a2, zero, 4
        0x34070005u, // ori a3, zero, 5
        0x27bdffe0u, // addiu sp, sp, -32
        0x34080006u, // ori t0, zero, 6
        0xafa80010u, // sw t0, 16(sp)
        0u,          // jal sprintf HLE guest import (patched below)
        0u,          // delay slot
        0x0000000du // break
    };
    if (!kernel.lookup_library("sysclib", 19u, &sprintf_stub) ||
        !write_guest_string(
            k_sprintf_format, "%u-%u-%u"))
    {
        return fail("could not prepare stacked sprintf guest");
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t instruction = i == 5u
            ? (0x0c000000u |
               ((sprintf_stub >> 2u) & 0x03ffffffu))
            : format_program[i];
        if (!kernel.cpu().write32(
                k_stack_format_program + i * 4u,
                instruction))
        {
            return fail("could not author O32 stacked sprintf call");
        }
    }
    const uint32_t stack_formatter = kernel.create_thread(
        k_stack_format_program, 0u, 1u, 0x400u,
        k_sprintf_destination, k_sprintf_format);
    if (stack_formatter == 0u ||
        !kernel.start_thread(stack_formatter))
    {
        return fail("could not start O32 stacked sprintf guest");
    }
    const iop::s_kernel_run_result formatted_run =
        kernel.run(128u);
    const iop::s_kernel_thread* formatted_thread =
        kernel.thread(stack_formatter);
    if (formatted_run.m_stop != iop::e_kernel_stop::breakpoint ||
        formatted_run.m_guest_pc != k_stack_format_program + 28u ||
        formatted_thread == nullptr ||
        formatted_thread->m_cpu.m_gpr[2] != 5u ||
        !check_guest_string(k_sprintf_destination, "4-5-6"))
    {
        return fail("SYSCLIB sprintf did not read O32 sp+16 argument");
    }

    const std::string expected_vsprintf = "00000012:-7:audio";
    if (!write_guest_string(
            k_vsprintf_format, "%08x:%d:%s") ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list, 0x12u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 4u, 0xfffffff9u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 8u, k_sprintf_string) ||
        !call_guest(
            kernel, "sysclib", 42u,
            {k_vsprintf_destination, k_vsprintf_format,
             k_vsprintf_argument_list, 0u}, &result) ||
        result != expected_vsprintf.size() ||
        !check_guest_string(
            k_vsprintf_destination, expected_vsprintf))
    {
        return fail("guest SYSCLIB vsprintf guest va_list was incorrect");
    }

    // The O32 guest va_list aligns 64-bit integer/double arguments
    // and preserves their little-endian words.
    const std::string expected_wide = "-42/1.5";
    if (!write_guest_string(k_vsprintf_format, "%lld/%.1f") ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list, 0xffffffd6u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 4u, 0xffffffffu) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 8u, 0u) ||
        !kernel.cpu().write32(
            k_vsprintf_argument_list + 12u, 0x3ff80000u) ||
        !call_guest(
            kernel, "sysclib", 42u,
            {k_vsprintf_destination, k_vsprintf_format,
             k_vsprintf_argument_list, 0u}, &result) ||
        result != expected_wide.size() ||
        !check_guest_string(k_vsprintf_destination, expected_wide))
    {
        return fail("guest SYSCLIB 64-bit varargs were misaligned");
    }

    // Malformed or dangerous format strings must reject the whole
    // request before changing any destination byte or guest result.
    for (const std::string& unsafe : {
             std::string("bad%n"), std::string("%5000d"),
             std::string("dangling%")})
    {
        if (!write_guest_string(k_sprintf_format, unsafe) ||
            !kernel.cpu().write32(
                k_sprintf_destination, 0x55555555u))
        {
            return fail("cannot prepare rejected sprintf test");
        }
        kernel.cpu().state().m_gpr[4] = k_sprintf_destination;
        kernel.cpu().state().m_gpr[5] = k_sprintf_format;
        kernel.cpu().state().m_gpr[6] = k_sprintf_string;
        const uint32_t prior = kernel.cpu().state().m_gpr[2];
        uint32_t preserved = 0u;
        if (iop::execute_sysclib_format(kernel.cpu(), 19u) ||
            !kernel.cpu().read32(
                k_sprintf_destination, &preserved) ||
            preserved != 0x55555555u ||
            kernel.cpu().state().m_gpr[2] != prior)
        {
            return fail("rejected SYSCLIB sprintf modified guest RAM");
        }
    }

    // A null destination or guest va_list cannot be treated as a
    // mapped IOP RAM region, even though physical RAM starts at zero.
    uint32_t first_iop_word = 0u;
    uint32_t preserved_first_word = 0u;
    if (!write_guest_string(k_sprintf_format, "%u") ||
        !kernel.cpu().read32(0u, &first_iop_word))
    {
        return fail("cannot prepare null formatting pointer test");
    }
    kernel.cpu().state().m_gpr[4] = 0u;
    kernel.cpu().state().m_gpr[5] = k_sprintf_format;
    kernel.cpu().state().m_gpr[6] = 123u;
    if (iop::execute_sysclib_format(kernel.cpu(), 19u) ||
        !kernel.cpu().read32(0u, &preserved_first_word) ||
        preserved_first_word != first_iop_word)
    {
        return fail("sprintf accepted null guest output pointer");
    }
    kernel.cpu().state().m_gpr[4] = k_sprintf_destination;
    kernel.cpu().state().m_gpr[6] = 0u;
    if (iop::execute_sysclib_format(kernel.cpu(), 42u))
    {
        return fail("vsprintf accepted null guest va_list pointer");
    }

    // StartThreadArgs preserves both guest entry arguments; the new
    // thread executes actual R3000 instructions when scheduled.
    kernel.cpu().write32(0x3e00u, 0x0000000du);
    const uint32_t args_thread =
        kernel.create_thread(0x3e00u, 0u, 32u, 0x400u);
    if (args_thread == 0u ||
        !call_guest(
            kernel, "thbase", 7u,
            {args_thread, 0x80u, 0x2e00u, 0u}, &result) ||
        result != 0u ||
        kernel.thread(args_thread) == nullptr ||
        kernel.thread(args_thread)->m_cpu.m_gpr[4] != 0x80u ||
        kernel.thread(args_thread)->m_cpu.m_gpr[5] != 0x2e00u)
    {
        return fail("StartThreadArgs lost guest argument state");
    }
    const iop::s_kernel_run_result args_run = kernel.run(64u);
    if (args_run.m_stop != iop::e_kernel_stop::breakpoint ||
        args_run.m_guest_pc != 0x3e00u)
    {
        return fail("StartThreadArgs guest was not actually scheduled");
    }

    // Unknown thread IDs remain a guest-level negative error.
    // Zero instead denotes the currently running R3000 thread, as
    // in the ps2-web-recomp/SDBZ kernel's thread(0)=current() lookup.
    for (uint16_t ordinal : {6u, 7u})
    {
        if (!call_guest(kernel, "thbase", ordinal,
                        {0xffffffffu, 0u, 0u, 0u}, &result) ||
            result != static_cast<uint32_t>(-407))
        {
            return fail("unknown StartThread guest error ABI");
        }
    }
    // Exercise the original import via actual R3000 instructions:
    // StartThread(0, 0x37) must restart the calling guest thread at
    // its entry, with a clean register context, not execute the old
    // post-syscall continuation and not spawn a synthetic thread.
    constexpr uint32_t restart_entry = 0x00005000u;
    constexpr uint32_t restart_marker = 0x00005f80u;
    uint32_t restart_import = 0u;
    if (!kernel.lookup_library("thbase", 6u, &restart_import) ||
        !kernel.cpu().write32(restart_marker, 0u))
    {
        return fail("cannot stage real StartThread(0) guest fixture");
    }
    const uint32_t restart_code[] = {
        0x3c080000u, // lui t0, 0
        0x35085f80u, // ori t0,t0,0x5f80
        0x8d090000u, // lw t1,0(t0)
        0x15200009u, // bne t1,zero,finished
        0u,          // delay slot
        0x24090001u, // addiu t1,zero,1
        0xad090000u, // sw t1,0(t0)
        0x24040000u, // addiu a0,zero,0 (current thread)
        0x24050037u, // addiu a1,zero,0x37
        0x0c000000u | ((restart_import >> 2u) & 0x03ffffffu),
        0u,          // JAL delay slot
        0x24090002u, // must not execute after a self restart
        0xad090000u, // must not overwrite the persistent marker
        0x0000000du // finished: BREAK
    };
    for (uint32_t index = 0u;
         index < sizeof(restart_code) / sizeof(restart_code[0]); ++index)
    {
        if (!kernel.cpu().write32(
                restart_entry + index * 4u, restart_code[index]))
        {
            return fail("cannot write self-restart R3000 program");
        }
    }
    const uint32_t restart_id = kernel.create_thread(
        restart_entry, 0u, 1u, 0x400u);
    if (restart_id == 0u || !kernel.start_thread(restart_id))
    {
        return fail("cannot start genuine self-restart guest thread");
    }
    const iop::s_kernel_run_result restart_run = kernel.run(128u, 8u);
    uint32_t marker_value = 0u;
    const iop::s_kernel_thread* restarted = kernel.thread(restart_id);
    if (restart_run.m_stop != iop::e_kernel_stop::breakpoint ||
        restart_run.m_guest_pc != restart_entry + 52u ||
        !kernel.cpu().read32(restart_marker, &marker_value) ||
        marker_value != 1u || restarted == nullptr ||
        // StartThread passes its second API argument as new a0.
        restarted->m_cpu.m_gpr[4] != 0x37u ||
        restarted->m_cpu.m_gpr[5] != 0u ||
        restarted->m_cpu.m_gpr[31] != 0x00013fc0u ||
        restarted->m_guest_instructions == 0u ||
        restarted->m_guest_slices == 0u ||
        restarted->m_handled_syscalls != 1u ||
        restarted->m_self_restarts != 1u ||
        restarted->m_last_self_restart_ra != restart_entry + 44u ||
        restarted->m_last_self_restart_arg != 0x37u ||
        restarted->m_last_syscall_ra != restart_entry + 44u ||
        restarted->m_last_syscall_a0 != 0u)
    {
        std::fprintf(stderr,
            "StartThread(0) diagnostic: stop=%u guest_pc=0x%08x "
            "executed=%llu marker=%u thread=%u status=%u "
            "pc=0x%08x a0=0x%08x a1=0x%08x ra=0x%08x\n",
            static_cast<uint32_t>(restart_run.m_stop),
            restart_run.m_guest_pc,
            static_cast<unsigned long long>(restart_run.m_executed),
            marker_value, restart_id,
            restarted == nullptr ? 0xffffffffu :
                static_cast<uint32_t>(restarted->m_status),
            restarted == nullptr ? 0u : restarted->m_cpu.m_pc,
            restarted == nullptr ? 0u : restarted->m_cpu.m_gpr[4],
            restarted == nullptr ? 0u : restarted->m_cpu.m_gpr[5],
            restarted == nullptr ? 0u : restarted->m_cpu.m_gpr[31]);
        return fail("StartThread(0) failed to restart its real guest CPU");
    }
    // A valid descriptor with impossible stack size must fail with
    // KE_NO_MEMORY rather than creating a fake thread or faulting
    // the caller's syscall.
    constexpr uint32_t k_oversized_thread = 0x00003f80u;
    if (!kernel.cpu().write32(k_oversized_thread + 8u, 0x3e00u) ||
        !kernel.cpu().write32(k_oversized_thread + 12u, 0xffffffffu) ||
        !kernel.cpu().write32(k_oversized_thread + 16u, 32u) ||
        !call_guest(kernel, "thbase", 4u,
                    {k_oversized_thread, 0u, 0u, 0u}, &result) ||
        result != static_cast<uint32_t>(-400))
    {
        return fail("oversized CreateThread did not report no memory");
    }

    // ExitDeleteThread must retire the real guest thread/stack after
    // its syscall returns; an implementation that merely reports zero
    // would instead execute the following BREAK.
    uint32_t exit_import = 0u;
    if (!kernel.lookup_library("thbase", 9u, &exit_import) ||
        !kernel.cpu().write32(
            0x3f40u, 0x0c000000u |
                ((exit_import >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(0x3f44u, 0u) ||
        !kernel.cpu().write32(0x3f48u, 0x0000000du))
    {
        return fail("cannot prepare ExitDeleteThread guest");
    }
    const uint32_t delete_thread =
        kernel.create_thread(0x3f40u, 0u, 32u, 0x400u);
    if (delete_thread == 0u ||
        !kernel.start_thread(delete_thread))
    {
        return fail("cannot schedule ExitDeleteThread guest");
    }
    const iop::s_kernel_run_result delete_run = kernel.run(128u);
    if (delete_run.m_stop != iop::e_kernel_stop::idle ||
        kernel.thread(delete_thread) != nullptr)
    {
        return fail("ExitDeleteThread did not retire the real thread");
    }

    // SIO2 DMA MMIO request retains STR, but never invents a DMA
    // completion or sets IOP interrupt readiness.
    uint32_t request = 0u;
    if (!call_guest(
            kernel, "dmacman", 32u,
            {11u, 0u, 0u, 0u}, &result) ||
        result != 1u ||
        !kernel.hardware().read(
            iop::c_hw::k_sio2_dma_rx_base + 8u,
            4u, &request) ||
        request != 0x01000200u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("SIO2 DMA request was not hardware-backed");
    }

    // Unknown IOP service must remain unhandled; reset invalidates
    // all installed imports and hosted semaphore/event state.
    kernel.reset();
    hle.reset_for_reboot();
    uint32_t stale = 0u;
    if (kernel.lookup_library("loadcore", 6u, &stale) ||
        !hle.install() ||
        !kernel.lookup_library("loadcore", 6u, &stale))
    {
        return fail("reboot did not reinstall legitimate HLE libraries");
    }

    std::printf("IOP kernel HLE service families: PASS\n");
    return 0;
}
