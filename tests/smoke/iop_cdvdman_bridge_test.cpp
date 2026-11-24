#include "iop_cdvdman_bridge.h"
#include "iop_sifman_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_kernel_hle_bridge.h"

#include <chrono>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
void put16(std::vector<uint8_t>& bytes, uint32_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

int fail(const char* message)
{
    std::fprintf(stderr, "IOP CDVDMAN HLE: %s\n", message);
    return 1;
}

struct s_temp_directory
{
    std::filesystem::path m_path;
    ~s_temp_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};

bool write_string(iop::c_cpu& cpu, uint32_t address,
                  const std::string& value)
{
    for (size_t i = 0u; i <= value.size(); i += 4u)
    {
        uint32_t word = 0u;
        for (size_t j = 0u; j < 4u && i + j < value.size(); ++j)
        {
            word |= static_cast<uint32_t>(
                static_cast<uint8_t>(value[i + j])
            ) << (j * 8u);
        }
        if (!cpu.write32(address + static_cast<uint32_t>(i), word))
        {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> make_irx(uint16_t ordinal)
{
    // Synthetic R3000A module, calling the actual linked cdvdman import.
    std::vector<uint8_t> image(0x200u, 0u);
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = image[5] = image[6] = 1u;
    put16(image, 16u, 0xff80u);
    put16(image, 18u, 8u);
    put32(image, 28u, 0x34u);
    put16(image, 40u, 52u);
    put16(image, 42u, 32u);
    put16(image, 44u, 2u);

    put32(image, 0x34u, 1u);           // PT_LOAD
    put32(image, 0x38u, 0x100u);
    put32(image, 0x44u, 0x70u);
    put32(image, 0x48u, 0x80u);

    put32(image, 0x54u, 0x70000080u); // PT_SCE_IOPMOD
    put32(image, 0x58u, 0x180u);
    put32(image, 0x184u, 0u);         // entry
    put32(image, 0x188u, 0x60u);      // gp
    put32(image, 0x18cu, 0x40u);
    put32(image, 0x190u, 0x20u);
    put32(image, 0x194u, 0x20u);

    put32(image, 0x100u, 0x3c040003u); // lui a0,3
    put32(image, 0x104u, 0x34841000u); // ori a0,a0,0x1000
    put32(image, 0x108u, 0x3c050003u); // lui a1,3
    put32(image, 0x10cu, 0x34a51020u); // ori a1,a1,0x1020
    put32(image, 0x110u, 0x0c00800du); // jal 0x20034 (linked import)
    put32(image, 0x114u, 0x00000000u); // delay slot nop
    put32(image, 0x118u, 0x08008006u); // j 0x20018 (idle)
    put32(image, 0x11cu, 0x00000000u); // delay slot nop

    put32(image, 0x120u, 0x41e00000u);
    const char library[] = "cdvdman";
    for (uint32_t i = 0u; i < sizeof(library) - 1u; ++i)
    {
        image[0x12cu + i] = static_cast<uint8_t>(library[i]);
    }
    put32(image, 0x134u, 0x03e00008u); // jr ra
    put32(image, 0x138u, 0x24000000u | ordinal);
    return image;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool read_output(iop::c_cpu& cpu, uint32_t address,
                 std::vector<uint8_t>* output)
{
    output->clear();
    for (uint32_t i = 0u; i < 32u; i += 4u)
    {
        uint32_t word = 0u;
        if (!cpu.read32(address + i, &word))
        {
            return false;
        }
        for (uint32_t j = 0u; j < 4u; ++j)
        {
            output->push_back(static_cast<uint8_t>(word >> (j * 8u)));
        }
    }
    return true;
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().
        time_since_epoch().count();
    s_temp_directory temporary{
        fs::temp_directory_path() /
            ("iop-cdvdman-" + std::to_string(stamp))
    };
    std::error_code error;
    fs::create_directories(temporary.m_path / "DATA", error);
    if (error)
    {
        return fail("could not create test directory");
    }
    const std::string payload = "actual-disc-data";
    {
        std::ofstream output(temporary.m_path / "DATA" / "MiXeD.DAT",
                             std::ios::binary);
        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        if (!output)
        {
            return fail("could not write test fixture");
        }
    }
    iop::c_folder_sector_source media(temporary.m_path);
    iop::s_folder_file_info expected{};
    if (!media.valid() ||
        !media.find_file("cdrom0:/data/mixed.dat;1", &expected))
    {
        return fail("synthetic folder media not indexed");
    }

    iop::c_kernel kernel;
    const std::vector<uint8_t> irx = make_irx(10u);
    if (kernel.load_module(irx).m_status !=
            iop::e_irx_load_result::unresolved_import)
    {
        return fail("missing cdvdman service was accepted");
    }

    iop::c_cdvdman_bridge bridge(kernel, media);
    if (!bridge.install())
    {
        return fail("failed to install cdvdman #10 import trampoline");
    }
    if (bridge.install() || bridge.handle_syscall(kernel, 0xffffffffu) ||
        kernel.load_module(make_irx(7u)).m_status !=
            iop::e_irx_load_result::unresolved_import)
    {
        return fail("duplicate install or unknown cdvdman export accepted");
    }

    const iop::s_module_load_result module = kernel.load_module(irx);
    if (module.m_status != iop::e_irx_load_result::success)
    {
        return fail("real synthetic cdvdman IRX import did not link");
    }
    const uint32_t id = kernel.create_module_thread(module.m_id, 1u);
    if (id == 0u || !kernel.start_thread(id))
    {
        return fail("synthetic cdvdman module was not scheduled");
    }

    // No media search occurs during link/install. The guest executes
    // its import stub, passing sceCdlFILE* in a0 and path in a1.
    for (uint32_t offset = 0u; offset < 32u; offset += 4u)
    {
        if (!kernel.cpu().write32(0x31000u + offset, 0xaaaaaaaau))
        {
            return fail("could not initialize output sentinel");
        }
    }
    if (!write_string(kernel.cpu(), 0x31020u, "\\data\\mixed.dat;1"))
    {
        return fail("could not initialize guest filename");
    }
    std::vector<uint8_t> before;
    if (!read_output(kernel.cpu(), 0x31000u, &before) ||
        before[0] != 0xaau || bridge.handle_syscall(kernel, 0xffffffffu))
    {
        return fail("install fabricated a search result");
    }

    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return bridge.handle_syscall(active, service);
    });
    const iop::s_kernel_run_result result = kernel.run(64u, 64u);
    const iop::s_kernel_thread* thread = kernel.thread(id);
    std::vector<uint8_t> output;
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        thread == nullptr || thread->m_cpu.m_gpr[2] != 1u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        read32(output, 0u) != expected.m_lba ||
        read32(output, 4u) != expected.m_size ||
        std::string(reinterpret_cast<const char*>(output.data() + 8u)) !=
            "MIXED.DAT;1" ||
        output[24] != 0u || output[25] != 0u ||
        output[26] != 0u || output[27] != 0u ||
        output[28] != 1u || output[29] != 1u ||
        output[30] != 0xeau || output[31] != 0x07u)
    {
        return fail("guest IRX import did not return accurate sceCdlFILE");
    }

    // The returned LBA must resolve to actual file data through the
    // same sector source used by CDVD DMA3.
    std::vector<uint8_t> bytes;
    if (!media.read_sectors(expected.m_lba, 1u, bytes) ||
        std::string(reinterpret_cast<const char*>(bytes.data()),
                    payload.size()) != payload)
    {
        return fail("returned LBA does not read actual file bytes");
    }

    // Lookup misses are successful HLE dispatches with v0=0 and must
    // leave the full output buffer intact. Invalid pointers fail closed.
    const std::vector<uint8_t> saved = output;
    if (!write_string(kernel.cpu(), 0x31020u, "\\DATA\\ABSENT.DAT;1"))
    {
        return fail("cannot write missing guest filename");
    }
    kernel.cpu().state().m_gpr[4] = 0x31000u;
    kernel.cpu().state().m_gpr[5] = 0x31020u;
    if (!bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        output != saved)
    {
        return fail("missing file did not return zero and preserve output");
    }

    if (!write_string(kernel.cpu(), 0x31020u, "\\DATA\\MIXED.DAT;2") ||
        !bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !read_output(kernel.cpu(), 0x31000u, &output) ||
        output != saved)
    {
        return fail("unsupported ISO version returned a false match");
    }

    kernel.cpu().state().m_gpr[4] = 0x001ffff0u;
    if (bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        !read_output(kernel.cpu(), 0x31000u, &output) || output != saved)
    {
        return fail("out-of-range guest output pointer was accepted");
    }
    kernel.cpu().state().m_gpr[4] = 0x31000u;
    kernel.cpu().state().m_gpr[5] = 0u;
    if (bridge.handle_syscall(kernel,
            iop::c_cdvdman_bridge::k_search_file_syscall) ||
        !read_output(kernel.cpu(), 0x31000u, &output) || output != saved)
    {
        return fail("null guest pathname pointer was accepted");
    }


    // Each supported CDVDMAN ordinal has an executable guest stub;
    // unknown hardware commands still fail to link.
    for (uint16_t ordinal : {
        4u, 5u, 6u, 8u, 10u, 11u, 12u, 13u,
        15u, 24u, 28u, 37u, 38u, 75u
    })
    {
        uint32_t entry = 0u;
        uint32_t instruction = 0u;
        if (!kernel.lookup_library("cdvdman", ordinal, &entry) ||
            !kernel.cpu().read32(entry + 8u, &instruction) ||
            instruction != 0x0000000cu)
        {
            return fail("supported CDVDMAN import has no guest stub");
        }
    }
    for (uint16_t ordinal : {7u, 22u, 39u})
    {
        uint32_t entry = 0u;
        if (kernel.lookup_library("cdvdman", ordinal, &entry))
        {
            return fail("unsupported CDVDMAN command falsely registered");
        }
    }

    // Exercise the first new ARKD_DVD dependency as a real linked
    // R3000 import, not just through a manually invoked host handler.
    iop::c_kernel standby_kernel;
    iop::c_cdvdman_bridge standby_bridge(standby_kernel, media);
    if (!standby_bridge.install() ||
        standby_kernel.load_module(make_irx(5u)).m_status !=
            iop::e_irx_load_result::success)
    {
        return fail("ARKD_DVD sceCdStandby import did not link");
    }
    const uint32_t standby_thread =
        standby_kernel.create_module_thread(1u, 1u);
    if (standby_thread == 0u ||
        !standby_kernel.start_thread(standby_thread))
    {
        return fail("sceCdStandby guest thread did not start");
    }
    standby_kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return standby_bridge.handle_syscall(active, service);
    });
    const iop::s_kernel_run_result standby_run =
        standby_kernel.run(64u, 64u);
    const iop::s_kernel_thread* standby_guest =
        standby_kernel.thread(standby_thread);
    if (standby_run.m_stop != iop::e_kernel_stop::budget_exhausted ||
        standby_guest == nullptr || standby_guest->m_cpu.m_gpr[2] != 1u ||
        standby_kernel.hardware().pending_interrupts() != 0u ||
        standby_kernel.hardware().cdvd_ncmd_pending())
    {
        return fail("sceCdStandby fabricated CDVD completion or failed");
    }
    // Let the accepted standby N-command complete before idle checks.
    (void)standby_kernel.run(256u, 64u);
    for (uint32_t mode : {0u, 1u, 17u})
    {
        standby_kernel.cpu().state().m_gpr[4] = mode;
        if (!standby_bridge.handle_syscall(
                standby_kernel,
                iop::c_cdvdman_bridge::k_service_base + 11u) ||
            standby_kernel.cpu().state().m_gpr[2] != 0u ||
            standby_kernel.hardware().cdvd_ncmd_pending())
        {
            return fail("idle sceCdSync incorrectly completed a command");
        }
    }
    // The synthetic IRX's JAL targets the initial relocated import
    // base, so use a fresh kernel instead of accidentally calling the
    // first module's already-linked sceCdStandby stub.
    iop::c_kernel sync_kernel;
    iop::c_cdvdman_bridge sync_bridge(sync_kernel, media);
    if (!sync_bridge.install())
    {
        return fail("sceCdSync guest bridge installation failed");
    }
    const iop::s_module_load_result sync_module =
        sync_kernel.load_module(make_irx(11u));
    if (sync_module.m_status != iop::e_irx_load_result::success)
    {
        return fail("sceCdSync guest import did not link");
    }
    const uint32_t sync_thread =
        sync_kernel.create_module_thread(sync_module.m_id, 1u);
    if (sync_thread == 0u || !sync_kernel.start_thread(sync_thread))
    {
        return fail("sceCdSync guest thread did not start");
    }
    sync_kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return sync_bridge.handle_syscall(active, service);
    });
    const iop::s_kernel_run_result sync_run =
        sync_kernel.run(64u, 64u);
    const iop::s_kernel_thread* sync_guest =
        sync_kernel.thread(sync_thread);
    if (sync_run.m_stop != iop::e_kernel_stop::budget_exhausted ||
        sync_guest == nullptr || sync_guest->m_cpu.m_gpr[2] != 0u ||
        sync_kernel.hardware().pending_interrupts() != 0u ||
        sync_kernel.hardware().cdvd_ncmd_pending())
    {
        return fail("sceCdSync guest import did not observe idle media");
    }

    // Real ARKD_DVD sceCdRead import with one 2048-byte sector.
    // The synthetic code puts the destination in a2 in JAL's delay
    // slot. The actual guest R3000 imports the linked #6 stub.
    iop::c_kernel read_kernel;
    iop::c_cdvdman_bridge read_bridge(read_kernel, media);
    std::vector<uint8_t> read_irx = make_irx(6u);
    put32(read_irx, 0x100u, 0x3c040000u | (expected.m_lba >> 16u));
    put32(read_irx, 0x104u, 0x34840000u | (expected.m_lba & 0xffffu));
    put32(read_irx, 0x108u, 0x34050001u); // ori a1, zero, 1
    put32(read_irx, 0x10cu, 0x3c060003u); // lui a2, 3
    put32(read_irx, 0x114u, 0x34c61000u); // ori a2, a2, 0x1000
    if (!read_bridge.install())
    {
        return fail("sceCdRead guest bridge installation failed");
    }
    const iop::s_module_load_result read_module =
        read_kernel.load_module(read_irx);
    if (read_module.m_status != iop::e_irx_load_result::success)
    {
        return fail("ARKD_DVD sceCdRead import did not link");
    }
    const uint32_t read_thread =
        read_kernel.create_module_thread(read_module.m_id, 1u);
    if (read_thread == 0u || !read_kernel.start_thread(read_thread))
    {
        return fail("sceCdRead guest thread did not start");
    }
    read_kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return read_bridge.handle_syscall(active, service);
    });
    const uint32_t expected_word =
        static_cast<uint32_t>(static_cast<uint8_t>(payload[0])) |
        (static_cast<uint32_t>(static_cast<uint8_t>(payload[1])) << 8u) |
        (static_cast<uint32_t>(static_cast<uint8_t>(payload[2])) << 16u) |
        (static_cast<uint32_t>(static_cast<uint8_t>(payload[3])) << 24u);
    if (!read_kernel.cpu().write32(0x31000u, 0xdeadbeefu))
    {
        return fail("sceCdRead sentinel could not be written");
    }
    const iop::s_kernel_run_result queued = read_kernel.run(12u, 1u);
    uint32_t sentinel = 0u;
    read_kernel.cpu().state().m_gpr[4] = 1u;
    if (queued.m_stop != iop::e_kernel_stop::budget_exhausted ||
        !read_kernel.cpu().read32(0x31000u, &sentinel) ||
        sentinel != 0xdeadbeefu ||
        !read_bridge.handle_syscall(
            read_kernel, iop::c_cdvdman_bridge::k_service_base + 11u) ||
        read_kernel.cpu().state().m_gpr[2] != 1u ||
        !read_bridge.handle_syscall(
            read_kernel, iop::c_cdvdman_bridge::k_service_base + 28u) ||
        read_kernel.cpu().state().m_gpr[2] != 6u)
    {
        return fail("sceCdRead did not queue a pending real sector read");
    }

    (void)read_kernel.run(256u, 64u);
    read_kernel.cpu().state().m_gpr[4] = 1u;
    if (!read_bridge.handle_syscall(
            read_kernel, iop::c_cdvdman_bridge::k_service_base + 11u) ||
        read_kernel.cpu().state().m_gpr[2] != 0u ||
        !read_bridge.handle_syscall(
            read_kernel, iop::c_cdvdman_bridge::k_service_base + 28u) ||
        read_kernel.cpu().state().m_gpr[2] != 10u ||
        !read_kernel.cpu().read32(0x31000u, &sentinel) ||
        sentinel != expected_word ||
        read_kernel.hardware().pending_interrupts() != 0u ||
        read_kernel.hardware().cdvd_ncmd_pending())
    {
        return fail("sceCdRead did not commit authentic mounted sector bytes");
    }
    // An invalid LBA cannot write guest RAM or report success.
    read_kernel.cpu().state().m_gpr[4] =
        static_cast<uint32_t>(media.sector_count());
    read_kernel.cpu().state().m_gpr[5] = 1u;
    read_kernel.cpu().state().m_gpr[6] = 0x31000u;
    read_kernel.cpu().state().m_gpr[7] = 0u;
    if (!read_bridge.handle_syscall(
            read_kernel, iop::c_cdvdman_bridge::k_service_base + 6u) ||
        read_kernel.cpu().state().m_gpr[2] != 0u ||
        !read_kernel.cpu().read32(0x31000u, &sentinel) ||
        sentinel != expected_word)
    {
        return fail("sceCdRead accepted out-of-range mounted-disc LBA");
    }

    // Exercise all four remaining static ARKD_DVD CDVDMAN imports.
    // In particular, #37 must execute actual R3000 guest callback code
    // when a real staged CDVD read finishes, not just store a pointer.
    iop::c_kernel callback_kernel;
    iop::c_cdvdman_bridge callback_bridge(callback_kernel, media);
    std::vector<uint8_t> callback_irx = make_irx(37u);
    put32(callback_irx, 0x104u, 0x34846000u); // a0=0x36000 callback
    if (!callback_bridge.install())
    {
        return fail("CDVD callback bridge installation failed");
    }
    const iop::s_module_load_result callback_module =
        callback_kernel.load_module(callback_irx);
    if (callback_module.m_status != iop::e_irx_load_result::success)
    {
        return fail("ARKD_DVD sceCdCallback import did not link");
    }
    // A real guest handler writes its reason and GP to IOP RAM, then
    // returns through the kernel's explicit one-shot callback target.
    constexpr uint32_t k_guest_callback = 0x36000u;
    constexpr uint32_t k_guest_callback_result = 0x35000u;
    const uint32_t callback_words[] = {
        0x3c080003u, // lui t0, 3
        0x35085000u, // ori t0, t0, 0x5000
        0xad040000u, // sw a0, 0(t0)
        0xad1c0004u, // sw gp, 4(t0)
        0x03e00008u, // jr ra
        0x00000000u
    };
    for (uint32_t i = 0u;
         i < sizeof(callback_words) / sizeof(callback_words[0]); ++i)
    {
        if (!callback_kernel.cpu().write32(
                k_guest_callback + i * 4u, callback_words[i]))
        {
            return fail("could not install guest CDVD callback code");
        }
    }
    if (!callback_kernel.cpu().write32(
            k_guest_callback_result, 0xdeadbeefu) ||
        !callback_kernel.cpu().write32(
            k_guest_callback_result + 4u, 0xdeadbeefu))
    {
        return fail("could not initialize CDVD callback sentinels");
    }
    const uint32_t callback_thread =
        callback_kernel.create_module_thread(callback_module.m_id, 1u);
    const iop::s_kernel_module* callback_loaded =
        callback_kernel.module(callback_module.m_id);
    if (callback_thread == 0u || callback_loaded == nullptr ||
        !callback_kernel.start_thread(callback_thread))
    {
        return fail("CDVD callback registration thread could not start");
    }
    const uint32_t expected_gp = callback_loaded->m_irx.m_gp;
    callback_kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return callback_bridge.handle_syscall(active, service);
        }
    );
    const iop::s_kernel_run_result registered =
        callback_kernel.run(64u, 64u);
    const iop::s_kernel_thread* registered_thread =
        callback_kernel.thread(callback_thread);
    if (registered.m_stop != iop::e_kernel_stop::budget_exhausted ||
        registered_thread == nullptr ||
        registered_thread->m_cpu.m_gpr[2] != 0u)
    {
        return fail("sceCdCallback guest registration failed");
    }
    callback_kernel.cpu().state().m_gpr[4] = expected.m_lba;
    callback_kernel.cpu().state().m_gpr[5] = 1u;
    callback_kernel.cpu().state().m_gpr[6] = 0x31000u;
    callback_kernel.cpu().state().m_gpr[7] = 0u;
    if (!callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 6u) ||
        callback_kernel.cpu().state().m_gpr[2] != 1u)
    {
        return fail("CDVD read with registered callback was not queued");
    }
    for (uint32_t ordinal : {15u, 38u})
    {
        if (!callback_bridge.handle_syscall(
                callback_kernel,
                iop::c_cdvdman_bridge::k_service_base + ordinal) ||
            callback_kernel.cpu().state().m_gpr[2] != 0u ||
            !callback_bridge.handle_syscall(
                callback_kernel,
                iop::c_cdvdman_bridge::k_service_base + 28u) ||
            callback_kernel.cpu().state().m_gpr[2] != 6u)
        {
            // Stop/Pause are N-commands: refused while a read is busy.
            return fail("sceCdStop/Pause fabricated pending read completion");
        }
    }
    // DVD mode and automatic selection are valid; CD-only does not
    // transform our mounted virtual DVD into a different medium.
    for (uint32_t mode : {0u, 2u})
    {
        callback_kernel.cpu().state().m_gpr[4] = mode;
        if (!callback_bridge.handle_syscall(
                callback_kernel,
                iop::c_cdvdman_bridge::k_service_base + 75u) ||
            callback_kernel.cpu().state().m_gpr[2] != 1u)
        {
            return fail("sceCdMmode rejected mounted DVD mode");
        }
    }
    callback_kernel.cpu().state().m_gpr[4] = 1u;
    if (!callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 75u) ||
        callback_kernel.cpu().state().m_gpr[2] != 0u)
    {
        return fail("sceCdMmode accepted absent CD media");
    }
    uint32_t callback_reason = 0u;
    uint32_t callback_gp = 0u;
    if (!callback_kernel.cpu().read32(
            k_guest_callback_result, &callback_reason) ||
        callback_reason != 0xdeadbeefu ||
        callback_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("CDVD callback fired before an actual read completion");
    }
    (void)callback_kernel.run(256u, 64u);
    if (!callback_kernel.cpu().read32(
            k_guest_callback_result, &callback_reason) ||
        !callback_kernel.cpu().read32(
            k_guest_callback_result + 4u, &callback_gp) ||
        callback_reason != 1u || callback_gp != expected_gp ||
        callback_kernel.hardware().pending_interrupts() != 0u ||
        callback_kernel.hardware().cdvd_ncmd_pending())
    {
        return fail("completed CDVD read did not run real guest callback");
    }
    // sceCdStandby is an N-command as well: accepted now (drive busy),
    // then completion runs the guest callback with SCECdFuncStandby (5).
    // ARKD_DVD.IRX releases its drive mutex only from that callback.
    if (!callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 5u) ||
        callback_kernel.cpu().state().m_gpr[2] != 1u ||
        !callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 13u) ||
        callback_kernel.cpu().state().m_gpr[2] != 6u)
    {
        return fail("sceCdStandby was not accepted as a pending N-command");
    }
    (void)callback_kernel.run(256u, 64u);
    if (!callback_kernel.cpu().read32(
            k_guest_callback_result, &callback_reason) ||
        callback_reason != 5u ||
        !callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 13u) ||
        callback_kernel.cpu().state().m_gpr[2] != 2u)
    {
        return fail("sceCdStandby completion did not run guest callback 5");
    }
    // The guest callback must be replaceable and unregisterable,
    // returning the previous function address as the PS2 ABI requires.
    callback_kernel.cpu().state().m_gpr[4] = 0u;
    if (!callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 37u) ||
        callback_kernel.cpu().state().m_gpr[2] != k_guest_callback)
    {
        return fail("sceCdCallback did not return previous guest handler");
    }
    callback_kernel.cpu().state().m_gpr[4] = 0x001fffffu;
    if (callback_bridge.handle_syscall(
            callback_kernel,
            iop::c_cdvdman_bridge::k_service_base + 37u))
    {
        return fail("sceCdCallback accepted invalid guest code pointer");
    }

    // Isolate a second authentic guest IRX import for sceCdReadClock.
    // Its R3000 module executes JAL into the new HLE import trampoline.
    iop::c_kernel clock_kernel;
    iop::c_cdvdman_bridge clock_bridge(clock_kernel, media);
    if (!clock_bridge.install())
    {
        return fail("CDVDMAN clock bridge install failed");
    }
    const iop::s_module_load_result clock_module =
        clock_kernel.load_module(make_irx(24u));
    if (clock_module.m_status != iop::e_irx_load_result::success)
    {
        return fail("CDVDMAN clock IRX import did not link");
    }
    const uint32_t clock_thread =
        clock_kernel.create_module_thread(clock_module.m_id, 1u);
    if (clock_thread == 0u || !clock_kernel.start_thread(clock_thread))
    {
        return fail("CDVDMAN clock module thread did not start");
    }
    clock_kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return clock_bridge.handle_syscall(active, service);
    });
    const iop::s_kernel_run_result clock_run =
        clock_kernel.run(64u, 64u);
    const iop::s_kernel_thread* clock_guest =
        clock_kernel.thread(clock_thread);
    if (clock_run.m_stop != iop::e_kernel_stop::budget_exhausted ||
        clock_guest == nullptr || clock_guest->m_cpu.m_gpr[2] != 1u)
    {
        return fail("guest R3000 sceCdReadClock failed");
    }
    uint8_t rtc[8]{};
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (!clock_kernel.cpu().read8(0x31000u + i, &rtc[i]))
        {
            return fail("CDVD RTC output not readable");
        }
    }
    const auto bcd = [](uint8_t value)
    {
        return ((value >> 4u) * 10u) + (value & 15u);
    };
    if (rtc[0] != 0u || rtc[4] != 0u ||
        bcd(rtc[1]) > 59u || bcd(rtc[2]) > 59u ||
        bcd(rtc[3]) > 23u || bcd(rtc[5]) < 1u ||
        bcd(rtc[5]) > 31u || bcd(rtc[6]) < 1u ||
        bcd(rtc[6]) > 12u)
    {
        return fail("CDVD clock format is not bounded BCD/JST");
    }
    // Observe immutable mounted media and absence of pending CDVDMAN
    // commands; these queries cannot fabricate a DMA completion.
    for (const auto& query : std::array<std::array<uint32_t, 2u>, 7u>{{
        {{4u, 1u}}, {{8u, 0u}},
        {{11u, 0u}}, {{12u, 0x14u}}, {{13u, 2u}},
        {{28u, 0x0au}}, {{5u, 1u}}
    }})
    {
        if (!clock_bridge.handle_syscall(
                clock_kernel,
                iop::c_cdvdman_bridge::k_service_base + query[0]) ||
            clock_kernel.cpu().state().m_gpr[2] != query[1])
        {
            return fail("CDVDMAN media/clock state query failed");
        }
    }
    // A clock read with an invalid guest pointer fails closed and
    // must not claim that valid clock data was delivered.
    clock_kernel.cpu().state().m_gpr[4] = 0x001ffffcu;
    if (clock_bridge.handle_syscall(
            clock_kernel,
            iop::c_cdvdman_bridge::k_service_base + 24u))
    {
        return fail("CDVDMAN clock read crossed IOP RAM");
    }

    // The retail PADMAN failure was an HLE stub collision:
    // SIFMAN #5 = 0x18700 + 5*0x20 and old CDVDMAN #37 =
    // 0x18300 + 37*0x20 both occupied 0x000187a0. A guest
    // sceSifInit was therefore misexecuted as sceCdCallback and
    // registered a PADMAN diagnostic string as executable code.
    // Check *all* composed service stubs, regardless of install order.
    for (const bool cdvd_first : {false, true})
    {
        iop::c_kernel composed{};
        ee::c_ee_sif mailbox{};
        mailbox.reset();
        iop::c_kernel_hle_bridge hle(composed);
        iop::c_sifman_bridge sifman(composed, mailbox);
        iop::c_sifcmd_bridge sifcmd(composed, mailbox);
        iop::c_cdvdman_bridge cdvd(composed, media);
        if (!hle.install() ||
            (cdvd_first &&
             (!cdvd.install() || !sifcmd.install() ||
              !sifman.install())) ||
            (!cdvd_first &&
             (!sifman.install() || !sifcmd.install() ||
              !cdvd.install())))
        {
            return fail("composed IOP HLE provider installation failed");
        }
        std::set<uint32_t> occupied{};
        const auto verify = [&](const char* library,
            uint16_t ordinal, uint32_t expected)
        {
            uint32_t entry = 0u;
            if (!composed.lookup_library(library, ordinal, &entry) ||
                !occupied.insert(entry).second ||
                entry < 0x00014000u || entry >= 0x0001a000u)
            {
                return false;
            }
            const uint32_t instructions[5] =
            {
                0x3c020000u | (expected >> 16u),
                0x34420000u | (expected & 0xffffu),
                0x0000000cu, 0x03e00008u, 0u
            };
            for (uint32_t index = 0u; index < 5u; ++index)
            {
                uint32_t actual = 0u;
                if (!composed.cpu().read32(entry + index * 4u, &actual) ||
                    actual != instructions[index])
                {
                    return false;
                }
            }
            return true;
        };
        for (const uint16_t ordinal : {
                 4u, 5u, 6u, 7u, 8u, 21u, 22u, 23u,
                 24u, 25u, 26u, 27u, 29u, 32u
             })
        {
            if (!verify("sifman", ordinal,
                iop::c_sifman_bridge::k_service_base + ordinal))
            {
                return fail("SIFMAN stub was overwritten by HLE provider");
            }
        }
        for (const uint16_t ordinal : {
                 4u, 6u, 7u, 12u, 14u, 15u, 16u, 17u,
                 18u, 19u, 22u, 23u, 24u, 25u, 28u
             })
        {
            if (!verify("sifcmd", ordinal,
                iop::c_sifcmd_bridge::k_service_base + ordinal))
            {
                return fail("SIFCMD stub was overwritten by HLE provider");
            }
        }
        for (const uint16_t ordinal : {
                 4u, 5u, 6u, 8u, 10u, 11u, 12u, 13u,
                 15u, 24u, 28u, 37u, 38u, 75u
             })
        {
            if (!verify("cdvdman", ordinal,
                iop::c_cdvdman_bridge::k_service_base + ordinal))
            {
                return fail("CDVDMAN stub overlaps another HLE provider");
            }
        }
        if (!verify("stdio", 4u,
            iop::c_kernel_hle_bridge::k_service_base + 0x304u))
        {
            return fail("STDIO HLE stub was overwritten");
        }
        uint32_t sif_init = 0u;
        uint32_t cd_callback = 0u;
        if (!composed.lookup_library("sifman", 5u, &sif_init) ||
            !composed.lookup_library("cdvdman", 37u, &cd_callback) ||
            sif_init != 0x000187a0u ||
            cd_callback == sif_init ||
            cd_callback != iop::c_cdvdman_bridge::k_extra_stub_base +
                37u * 0x20u ||
            iop::c_cdvdman_bridge::k_search_file_stub != 0x00018200u)
        {
            return fail("retail PADMAN SIFMAN/CDVDMAN collision persists");
        }

        // Exercise the *linked* SIFMAN #5 import through original R3000
        // guest code after all three providers have installed. This
        // must run sceSifInit, not set the CDVDMAN callback pointer.
        std::vector<uint8_t> sif_init_irx = make_irx(5u);
        std::memset(sif_init_irx.data() + 0x12cu, 0u, 8u);
        std::memcpy(sif_init_irx.data() + 0x12cu, "sifman", 6u);
        const iop::s_module_load_result sif_module =
            composed.load_module(sif_init_irx);
        if (sif_module.m_status != iop::e_irx_load_result::success)
        {
            return fail("linked SIFMAN #5 synthetic module not accepted");
        }
        const uint32_t thread_id =
            composed.create_module_thread(sif_module.m_id, 1u);
        if (thread_id == 0u || !composed.start_thread(thread_id))
        {
            return fail("linked SIFMAN #5 thread could not start");
        }
        composed.bind_syscall(
            [&](iop::c_kernel& active, uint32_t service)
            {
                return sifman.handle_syscall(active, service);
            }
        );
        const iop::s_kernel_run_result sif_result =
            composed.run(64u, 64u);
        uint32_t dma_pcr2 = 0u;
        if (sif_result.m_stop != iop::e_kernel_stop::budget_exhausted ||
            !composed.hardware().read(
                iop::c_hw::k_dma_pcr2, 4u, &dma_pcr2) ||
            (dma_pcr2 & 0x8800u) != 0x8800u)
        {
            return fail("linked SIFMAN #5 did not execute sceSifInit");
        }
    }

    std::printf("IOP CDVDMAN search/clock/media guest imports: PASS\n");
    return 0;
}
