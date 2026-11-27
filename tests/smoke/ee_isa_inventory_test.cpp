#include "ee_decoder.h"
#include "ee_isa_inventory.h"
#include "instruction_lifter.h"
#include <cstdio>
#include <sstream>
#include <string>

int main()
{
    ee::c_ee_isa_inventory inventory;
    const ee::s_ee_instruction addiu = ee::decode(0x1000u, 0x24020001u);
    // Nonzero reserved rs is deliberately unsupported; legal MFSA is valid.
    const ee::s_ee_instruction mfsa = ee::decode(0x1004u, 0x00803028u);
    const ee::s_ee_instruction sc = ee::decode(0x1008u, 0xe0850000u);

    if (!(addiu.valid == ee::e_ee_valid::VALID))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(mfsa.valid == ee::e_ee_valid::UNSUPPORTED))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(sc.valid == ee::e_ee_valid::INVALID))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    const bool addiu_lowers = ee::lift(addiu).kind != ee::LOP_UNSUPPORTED;
    if (!(addiu_lowers))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    inventory.record(addiu, addiu_lowers);
    inventory.record(mfsa, false);
    inventory.record(sc, false);

    // A newly introduced valid decoder opcode that the lifter does not
    // handle must appear as valid_lift_gap, never as supported execution.
    const ee::s_ee_instruction introduced = ee::decode(0x100cu, 0x24030002u);
    if (!(introduced.valid == ee::e_ee_valid::VALID))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    inventory.record(introduced, false);

    if (!(inventory.totals().m_total == 4u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(inventory.totals().m_decoder_valid == 2u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(inventory.totals().m_decoder_unsupported == 1u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(inventory.totals().m_decoder_invalid == 1u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(inventory.totals().m_lifter_accepts == 1u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(inventory.totals().m_valid_lift_gap == 1u))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }

    std::ostringstream output;
    inventory.write_json(output, true);
    const std::string json = output.str();
    if (!(json.find("\"schema\": \"ee-m2-cfg-isa-inventory-v1\"") != std::string::npos))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(json.find("\"scope\": \"selected_function_cfgs\"") != std::string::npos))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(json.find("\"reached\": \"unmeasured_without_guest_trace\"") != std::string::npos))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(json.find("\"classification\": \"valid_lift_gap\"") != std::string::npos))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    if (!(json.find("\"decoder_invalid\": 1") != std::string::npos))
    {
        std::fprintf(stderr, "ISA inventory check failed at line %d\n", __LINE__);
        return 1;
    }
    return 0;
}
