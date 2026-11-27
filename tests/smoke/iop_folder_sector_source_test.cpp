#include "iop_folder_sector_source.h"
#include "iop_cpu.h"
#include "iop_hw.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP folder ISO9660: %s\n", message);
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

bool write_file(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

uint32_t read32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) << 24u);
}

bool dma_read(iop::c_hw& hardware, uint32_t lba, uint32_t count)
{
    const uint8_t command[11]{
        static_cast<uint8_t>(lba),
        static_cast<uint8_t>(lba >> 8u),
        static_cast<uint8_t>(lba >> 16u),
        static_cast<uint8_t>(lba >> 24u),
        static_cast<uint8_t>(count),
        static_cast<uint8_t>(count >> 8u),
        static_cast<uint8_t>(count >> 16u),
        static_cast<uint8_t>(count >> 24u),
        0x10u, 0x85u, 0u
    };
    if (!hardware.write(iop::c_hw::k_cdrom_dma_base, 4u, 0x30000u) ||
        !hardware.write(iop::c_hw::k_cdrom_dma_base + 4u, 4u,
                        (count << 16u) | 0x0200u) ||
        !hardware.write(iop::c_hw::k_cdrom_dma_chcr, 4u, 0x41000200u))
    {
        return false;
    }
    for (uint8_t ch : command)
    {
        if (!hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, ch))
        {
            return false;
        }
    }
    return hardware.write(iop::c_hw::k_cdvd_ncmd_command, 1u, 0x06u);
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().
        time_since_epoch().count();
    s_temp_directory temporary{
        fs::temp_directory_path() /
            ("sdbz-folder-iso-" + std::to_string(stamp))
    };
    fs::create_directories(temporary.m_path / "DATA");
    fs::create_directories(temporary.m_path / "GAME");
    fs::create_directories(temporary.m_path / "NATIVE");
    std::vector<uint8_t> payload(3000u);
    for (size_t i = 0u; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(i & 0xffu);
    }
    if (!write_file(temporary.m_path / "DATA" / "MiXeD.DAT", payload) ||
        !write_file(temporary.m_path / "SYSTEM.CNF", {'O','K'}) ||
        !write_file(temporary.m_path / "GAME" / "ASSET.BIN", {'X'}) ||
        !write_file(temporary.m_path / "NATIVE" / "MOD.BIN", {'Y'}))
    {
        return fail("cannot create fixture");
    }

    iop::c_folder_sector_source source(temporary.m_path);
    iop::c_folder_sector_source second(temporary.m_path);
    if (!source.valid() || !second.valid() ||
        source.sector_count() != second.sector_count())
    {
        std::fprintf(stderr, "mount error: %s\n", source.error().c_str());
        return fail("mount or deterministic layout rejected");
    }

    iop::s_folder_file_info info{};
    iop::s_folder_file_info repeat{};
    if (!source.find_file("cdrom0:\\data\\mixed.dat;1", &info) ||
        !source.find_file("CDROM0:/DATA/MIXED.DAT", &repeat) ||
        info.m_lba != repeat.m_lba || info.m_size != payload.size() ||
        !second.find_file("CDROM0:/DATA/MIXED.DAT;1", &repeat) ||
        info.m_lba != repeat.m_lba ||
        source.find_file("cdrom0:/DATA/MIXED.DAT;2", &repeat) ||
        source.find_file("cdrom0:/DATA/../SYSTEM.CNF", &repeat) ||
        source.find_file("cdrom0:/DATA/MISSING.IRX", &repeat) ||
        source.find_file("cdrom0:/NATIVE/MOD.BIN", &repeat) ||
        source.find_file("cdrom0:/GAME/ASSET.BIN", &repeat) ||
        source.find_file("host:/DATA/MIXED.DAT", &repeat))
    {
        return fail("case/version lookup or path safety failed");
    }

    std::vector<uint8_t> pvd;
    std::vector<uint8_t> terminator;
    std::vector<uint8_t> pvd_repeat;
    if (!source.read_sectors(16u, 1u, pvd) ||
        !source.read_sectors(17u, 1u, terminator) ||
        !second.read_sectors(16u, 1u, pvd_repeat) ||
        pvd != pvd_repeat ||
        pvd.size() != 2048u || pvd[0] != 1u ||
        std::string(reinterpret_cast<const char*>(pvd.data() + 1u), 5u) !=
            "CD001" ||
        terminator[0] != 255u ||
        read32(pvd.data() + 80u) != source.sector_count() ||
        read32(pvd.data() + 158u) < 18u)
    {
        return fail("ISO9660 PVD, volume bounds, or determinism incorrect");
    }

    const uint32_t root_lba = read32(pvd.data() + 158u);
    const uint32_t path_lba = read32(pvd.data() + 140u);
    std::vector<uint8_t> root_data;
    std::vector<uint8_t> path_table;
    if (!source.read_sectors(root_lba, 1u, root_data) ||
        !source.read_sectors(path_lba, 1u, path_table) ||
        root_data[0] != 34u || root_data[34u] != 34u ||
        path_table[0u] != 1u || path_table[6u] != 1u ||
        path_table[7u] != 0u)
    {
        return fail("root directory or path table missing");
    }

    std::vector<uint8_t> file_data;
    if (!source.read_sectors(info.m_lba, 2u, file_data) ||
        file_data.size() != 4096u ||
        !std::equal(payload.begin(), payload.end(), file_data.begin()) ||
        !std::all_of(file_data.begin() + payload.size(), file_data.end(),
                     [](uint8_t ch) { return ch == 0u; }) ||
        source.read_sectors(static_cast<uint32_t>(
            source.sector_count()), 1u, file_data) ||
        source.read_sectors(info.m_lba, 0u, file_data) ||
        source.read_sectors(info.m_lba, 0xffffffffu, file_data))
    {
        return fail("folder sectors, zero padding, or bounds incorrect");
    }

    iop::c_cpu cpu;
    iop::c_hw hardware;
    cpu.bind_hardware(&hardware);
    iop::c_cdvd_dma_transport transport(cpu, hardware, source);
    if (!hardware.write(iop::c_hw::k_dma_icr, 4u, 0x00880000u) ||
        !dma_read(hardware, info.m_lba, 2u))
    {
        return fail("folder-backed CDVD read command failed");
    }
    uint32_t value = 0u;
    if (transport.bytes_transferred() != 4096u ||
        !cpu.read32(0x30000u, &value) ||
        value != read32(payload.data()) ||
        !cpu.read32(0x30000u + 2996u, &value) ||
        value != read32(payload.data() + 2996u))
    {
        return fail("folder-backed CDVD DMA did not copy authentic file bytes");
    }

    {
        const fs::path card = temporary.m_path / "Mcd001.ps2";
        if (!write_file(card, {'M', 'C'}))
        {
            return fail("cannot create a host card fixture");
        }
        const iop::c_folder_sector_source with_card(temporary.m_path, card);
        iop::s_folder_file_info card_info{};
        if (!with_card.valid() || with_card.sector_count() != source.sector_count() ||
            with_card.find_file("cdrom0:/MCD001.PS2", &card_info) ||
            !with_card.find_file("cdrom0:/SYSTEM.CNF", &card_info))
        {
            return fail("mounted card affected guest disc layout");
        }
        const fs::path invalid = temporary.m_path / "other invalid filename.bin";
        if (!write_file(invalid, {'X'}))
        {
            return fail("cannot create an invalid disc entry");
        }
        const iop::c_folder_sector_source with_invalid(temporary.m_path, card);
        if (with_invalid.valid() || with_invalid.error().find(invalid.filename().string()) == std::string::npos)
        {
            return fail("unrelated invalid disc filename was not reported");
        }
        fs::remove(invalid);
        fs::remove(card);
    }

    {
        // Non-ASCII host card names must remain UTF-8 and be excluded
        // before validating the guest ISO9660 filename alphabet.
        const fs::path card = temporary.m_path / fs::path(u8"Mcd-\u00e9.ps2");
        if (!write_file(card, {'M', 'C'}))
        {
            return fail("cannot create UTF-8 host card fixture");
        }
        const iop::c_folder_sector_source with_card(temporary.m_path, card);
        if (!with_card.valid() || with_card.sector_count() != source.sector_count())
        {
            return fail("UTF-8 mounted card affected guest disc layout");
        }
        const iop::c_folder_sector_source without_card(temporary.m_path);
        if (without_card.valid() ||
            without_card.error().find("Mcd-\xc3\xa9.ps2") == std::string::npos)
        {
            return fail("non-ISO9660 UTF-8 filename was not reported byte-exactly");
        }
        fs::remove(card);
    }

#ifndef _WIN32
    {
        const fs::path duplicate_root = temporary.m_path / "COLLISION";
        fs::create_directory(duplicate_root);
        write_file(duplicate_root / "Case.DAT", {'A'});
        write_file(duplicate_root / "case.dat", {'B'});
        iop::c_folder_sector_source collision(duplicate_root);
        if (collision.valid())
        {
            return fail("case-insensitive duplicate accepted");
        }
    }
    {
        const fs::path link_root = temporary.m_path / "SYMLINK";
        fs::create_directory(link_root);
        std::error_code error;
        fs::create_symlink(temporary.m_path / "SYSTEM.CNF",
                           link_root / "ESCAPE.CNF", error);
        if (!error)
        {
            iop::c_folder_sector_source link(link_root);
            if (link.valid())
            {
                return fail("symbolic link accepted");
            }
        }
    }
#endif

    // The synthetic ISO index is fixed when mounted. If its host data
    // changes size, reads must not silently return mismatched contents.
    write_file(temporary.m_path / "DATA" / "MiXeD.DAT", {'Z'});
    if (source.read_sectors(info.m_lba, 1u, file_data))
    {
        return fail("modified source file was silently accepted");
    }

    std::printf("IOP folder ISO9660 and CDVD DMA: PASS\n");
    return 0;
}
