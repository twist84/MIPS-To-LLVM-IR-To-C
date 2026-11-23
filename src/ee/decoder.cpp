#include "ee_instruction.h"
#include "ee_opcode.h"
#include <cstdio>
#include <string_view>

namespace ee {

namespace
{
uint32_t field(uint32_t word, int hi, int lo)
{
    return (word >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

void set_mnemonic(s_ee_instruction& insn, const char* name)
{
    std::snprintf(insn.mnemonic, sizeof(insn.mnemonic), "%s", name);
}

// ---- MMI (opcode 0x1c) classification ------------------------------------
// Real R5900 layout: the function field selects either a top-level op
// (MADD, MFHI1, ...) or one of four sub-op groups, with the sub-op in `sa`:
//   funct 0x08 = MMI0, 0x28 = MMI1, 0x09 = MMI2, 0x29 = MMI3.
// Names come from the R5900 opcode map (cross-checked against PCSX2).
const char* const kMmi[64] = {
    "madd", "maddu", "reserved", "reserved", "plzcw", "reserved", "reserved", "reserved",
    "mmi0", "mmi2", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "mfhi1", "mthi1", "mflo1", "mtlo1", "reserved", "reserved", "reserved", "reserved",
    "mult1", "multu1", "div1", "divu1", "reserved", "reserved", "reserved", "reserved",
    "madd1", "maddu1", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "mmi1", "mmi3", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "pmfhl", "pmthl", "reserved", "reserved", "psllh", "reserved", "psrlh", "psrah",
    "reserved", "reserved", "reserved", "reserved", "psllw", "reserved", "psrlw", "psraw"
};

const char* const kMmi0[32] = {
    "paddw", "psubw", "pcgtw", "pmaxw", "paddh", "psubh", "pcgth", "pmaxh",
    "paddb", "psubb", "pcgtb", "reserved", "reserved", "reserved", "reserved", "reserved",
    "paddsw", "psubsw", "pextlw", "ppacw", "paddsh", "psubsh", "pextlh", "ppach",
    "paddsb", "psubsb", "pextlb", "ppacb", "reserved", "reserved", "pext5", "ppac5"
};

const char* const kMmi1[32] = {
    "reserved", "pabsw", "pceqw", "pminw", "padsbh", "pabsh", "pceqh", "pminh",
    "reserved", "reserved", "pceqb", "reserved", "reserved", "reserved", "reserved", "reserved",
    "padduw", "psubuw", "pextuw", "reserved", "padduh", "psubuh", "pextuh", "reserved",
    "paddub", "psubub", "pextub", "qfsrv", "reserved", "reserved", "reserved", "reserved"
};

const char* const kMmi2[32] = {
    "pmaddw", "reserved", "psllvw", "psrlvw", "pmsubw", "reserved", "reserved", "reserved",
    "pmfhi", "pmflo", "pinth", "reserved", "pmultw", "pdivw", "pcpyld", "reserved",
    "pmaddh", "phmadh", "pand", "pxor", "pmsubh", "phmsbh", "reserved", "reserved",
    "reserved", "reserved", "pexeh", "prevh", "pmulth", "pdivbw", "pexew", "prot3w"
};

const char* const kMmi3[32] = {
    "pmadduw", "reserved", "reserved", "psravw", "reserved", "reserved", "reserved", "reserved",
    "pmthi", "pmtlo", "pinteh", "reserved", "pmultuw", "pdivuw", "pcpyud", "reserved",
    "reserved", "reserved", "por", "pnor", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "pexch", "pcpyh", "reserved", "reserved", "pexcw", "reserved"
};


// VALID only for operations the IR builder lowers *exactly*. Everything else is
// reported UNSUPPORTED under its real name, so it can never be mis-executed.

// COP2's rs field first chooses transfer / branch / macro mode. Macro mode
// then dispatches through the low six-bit SPECIAL1 table; entries 60..63
// redirect to SPECIAL2, whose index is {sa, funct[1:0]}. These masks mirror
// the R5900 opcode tables. We classify recognised VU0 operations as
// UNSUPPORTED until their full pipeline/flag semantics are implemented.
bool cop2_special1_defined(uint32_t funct)
{
    return funct <= 50 || funct == 52 || funct == 53 ||
           funct == 56 || funct == 57 || funct >= 60;
}

bool cop2_special2_defined(uint32_t index)
{
    return index <= 42 || (index >= 44 && index <= 49) ||
           (index >= 52 && index <= 67);
}

bool cop1_s_defined(uint32_t funct)
{
    return funct <= 7 || funct == 0x16 ||
           (funct >= 0x18 && funct <= 0x1a) ||
           (funct >= 0x1c && funct <= 0x1f) ||
           funct == 0x24 || funct == 0x28 || funct == 0x29 ||
           funct == 0x30 || funct == 0x32 || funct == 0x34 || funct == 0x36;
}

bool cop1_s_lowered_exactly(uint32_t funct)
{
    return funct <= 7 || funct == 0x16 ||
           funct == 0x18 || funct == 0x19 || funct == 0x1a ||
           (funct >= 0x1c && funct <= 0x1f) ||
           funct == 0x24 || // CVT.W.S (truncating, saturating EE conversion)
           funct == 0x28 || funct == 0x29 || // MAX.S / MIN.S (raw signed-bit ordering)
           funct == 0x30 || funct == 0x32 ||
           funct == 0x34 || funct == 0x36;
}

bool mmi_lowered_exactly(uint32_t funct, uint32_t sa, uint32_t rs,
                         uint32_t rt, uint32_t rd)
{
    switch (funct)
    {
    case 0x04:                                          // PLZCW uses rs only
        return rt == 0 && sa == 0;
    case 0x34: case 0x36: case 0x37:                    // packed halfword shifts
    case 0x3c: case 0x3e: case 0x3f:                    // packed word shifts
        return rs == 0;
    case 0x00: case 0x01: case 0x20: case 0x21:        // madd/maddu, pipeline 0/1
    case 0x18: case 0x19:                                // mult/multu, pipeline 1
    case 0x1a: case 0x1b:                                // div/divu, pipeline 1
        return sa == 0;
    case 0x10: case 0x11: case 0x12: case 0x13:        // mfhi1 mthi1 mflo1 mtlo1
        return true;
    case 0x30: // PMFHL: rd <- HI/LO words, sa selects five layouts
        return rs == 0 && rt == 0 && sa <= 4;
    case 0x31: // PMTHL: HI/LO selected words <- rs; sa must be zero
        return rt == 0 && rd == 0 && sa == 0;
    case 0x08:                                          // MMI0
        // All named MMI0 packed arithmetic, comparisons, interleaves,
        // packs and color conversions have exact native LLVM lowering.
        // Reserved sub-opcodes remain INVALID via kMmi0.
        switch (sa)
        {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0a:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b:
            return true;
        case 0x1e: case 0x1f: // PEXT5/PPAC5 use rt only
            return rs == 0;
        default:
            return false;
        }
    case 0x28:                                          // MMI1
        switch (sa)
        {
        case 0x01: case 0x05:                            // pabsw/pabsh use rt only
            return rs == 0;
        case 0x02: case 0x03: case 0x04: case 0x06: case 0x07: // pceq*/pmin*/padsbh
        case 0x0a:                                      // pceqb
        case 0x10: case 0x11: case 0x12:               // padduw/psubuw/pextuw
        case 0x14: case 0x15: case 0x16:               // padduh/psubuh/pextuh
        case 0x18: case 0x19: case 0x1a: case 0x1b:     // paddub/psubub/pextub/qfsrv
            return true;
        default:
            return false;
        }
    case 0x09:                                          // MMI2
        switch (sa)
        {
        case 0x00: case 0x04:                            // PMADDW / PMSUBW
        case 0x02: case 0x03:                            // PSLLVW / PSRLVW
        case 0x0a:                                      // PINTH
            return true;
        case 0x10: case 0x11: case 0x14: case 0x15:      // PMADDH / PHMADH / PMSUBH / PHMSBH
        case 0x1c:                                      // PMULTH
            return true;
        case 0x08: case 0x09:                            // PMFHI / PMFLO
            return rs == 0 && rt == 0;
        case 0x0c:                                      // PMULTW (signed)
            return true;
        case 0x0d:                                      // PDIVW (signed)
        case 0x1d:                                      // PDIVBW (four signed words / rt low halfword)
            return rd == 0;
        case 0x0e: case 0x12: case 0x13:               // PCPYLD / PAND / PXOR
            return true;
        case 0x1a: case 0x1b: case 0x1e: case 0x1f:   // PEXEH/PREVH/PEXEW/PROT3W
            return rs == 0;
        default:
            return false;
        }
    case 0x29:                                          // MMI3
        switch (sa)
        {
        case 0x03:                                      // PSRAVW: signed variable shift
        case 0x0a:                                      // PINTEH
        case 0x00:                                      // PMADDUW
            return true;
        case 0x08: case 0x09:                            // PMTHI / PMTLO
            return rt == 0 && rd == 0;
        case 0x0c:                                      // PMULTUW (unsigned)
            return true;
        case 0x0d:                                      // PDIVUW (unsigned)
            return rd == 0;
        case 0x0e: case 0x12: case 0x13:               // PCPYUD / POR / PNOR
            return true;
        case 0x1b:                                      // PCPYH uses rt only
            return rs == 0;
        default:
            return false;
        }
    default:
        return false;
    }
}

} // namespace

s_ee_instruction decode(uint32_t pc, uint32_t word)
{
    s_ee_instruction insn;
    insn.pc = pc;
    insn.opcode_word = word;

    const uint32_t op = field(word, 31, 26);
    insn.rs = (uint8_t)field(word, 25, 21);
    insn.rt = (uint8_t)field(word, 20, 16);
    insn.rd = (uint8_t)field(word, 15, 11);
    insn.sa = (uint16_t)field(word, 10, 6);
    insn.funct = (uint8_t)field(word, 5, 0);
    insn.immediate = (int16_t)(word & 0xffffu);
    insn.uimmediate = (uint16_t)(word & 0xffffu);


    switch (op)
    {
    case 0x00: // SPECIAL (R-type)
        insn.type = e_ee_op_type::OP_TYPE_RTYPE;
        insn.opcode = e_ee_opcode::OP_SPECIAL;
        insn.flow = e_ee_flow::FLOW_NONE;
        switch (static_cast<e_ee_opcode>(insn.funct))
        {
        case e_ee_opcode::FUNCT_SLL:
        case e_ee_opcode::FUNCT_SRL:
        case e_ee_opcode::FUNCT_SRA:
        case e_ee_opcode::FUNCT_SLLV:
        case e_ee_opcode::FUNCT_SRLV:
        case e_ee_opcode::FUNCT_SRAV:
        case e_ee_opcode::FUNCT_DSLL:
        case e_ee_opcode::FUNCT_DSRL:
        case e_ee_opcode::FUNCT_DSRA:
        case e_ee_opcode::FUNCT_DSLL32:
        case e_ee_opcode::FUNCT_DSRL32:
        case e_ee_opcode::FUNCT_DSRA32:
        case e_ee_opcode::FUNCT_DSLLV:
        case e_ee_opcode::FUNCT_DSRLV:
        case e_ee_opcode::FUNCT_DSRAV:
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "shift");
            break;
        case e_ee_opcode::FUNCT_JR:
            insn.flow = (insn.rs == 31) ? e_ee_flow::FLOW_RETURN : e_ee_flow::FLOW_JUMP;
            insn.has_delay_slot = true;
            // JR targets are resolved by the host dispatcher after the delay
            // slot. Only the architecturally reserved fields must be zero.
            insn.valid = (insn.rt == 0 && insn.rd == 0 && insn.sa == 0)
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "jr");
            break;
        case e_ee_opcode::FUNCT_JALR:
            // The dynamic target is captured before the delay slot; the
            // encoded rd receives PC+8 before that slot executes.
            insn.flow = e_ee_flow::FLOW_CALL;
            insn.has_delay_slot = true;
            insn.valid = (insn.rt == 0 && insn.sa == 0)
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "jalr");
            break;
        case e_ee_opcode::FUNCT_MFHI:
        case e_ee_opcode::FUNCT_MTHI:
        case e_ee_opcode::FUNCT_MFLO:
        case e_ee_opcode::FUNCT_MTLO:
        case e_ee_opcode::FUNCT_MULT:
        case e_ee_opcode::FUNCT_MULTU:
        case e_ee_opcode::FUNCT_DIV:
        case e_ee_opcode::FUNCT_DIVU:
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "hilo");
            break;
        case e_ee_opcode::FUNCT_ADD:
        case e_ee_opcode::FUNCT_SUB:
            insn.valid = insn.sa == 0 ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "signed_arith");
            break;
        case e_ee_opcode::FUNCT_DADD:
        case e_ee_opcode::FUNCT_DSUB:
            // Exact LLVM signed overflow and guest EX_Ov entry are supported.
            insn.valid = insn.sa == 0
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "signed_arith");
            break;
        case e_ee_opcode::FUNCT_ADDU:
        case e_ee_opcode::FUNCT_SUBU:
        case e_ee_opcode::FUNCT_AND:
        case e_ee_opcode::FUNCT_OR:
        case e_ee_opcode::FUNCT_XOR:
        case e_ee_opcode::FUNCT_NOR:
        case e_ee_opcode::FUNCT_DADDU:
        case e_ee_opcode::FUNCT_DSUBU:
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "r_arith");
            break;
        case e_ee_opcode::FUNCT_SLT:
        case e_ee_opcode::FUNCT_SLTU:
            // The shift-amount field is reserved for register comparisons.
            // Their exact 64-bit signed/unsigned lowering is implemented.
            insn.valid = insn.sa == 0
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, insn.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_SLT)
                ? "slt" : "sltu");
            break;
        case e_ee_opcode::FUNCT_SYSCALL:
            insn.valid = e_ee_valid::VALID;
            insn.flow = e_ee_flow::FLOW_EXCEPTION;
            set_mnemonic(insn, "syscall");
            break;
        case e_ee_opcode::FUNCT_BREAK:
            insn.valid = e_ee_valid::VALID;
            insn.flow = e_ee_flow::FLOW_EXCEPTION;
            set_mnemonic(insn, "break");
            break;
        case e_ee_opcode::FUNCT_SYNC:
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "sync");
            break;
        case e_ee_opcode::FUNCT_MOVZ:
        case e_ee_opcode::FUNCT_MOVN:
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "movc");
            break;
        case e_ee_opcode::FUNCT_TGE:
        case e_ee_opcode::FUNCT_TGEU:
        case e_ee_opcode::FUNCT_TLT:
        case e_ee_opcode::FUNCT_TLTU:
        case e_ee_opcode::FUNCT_TEQ:
        case e_ee_opcode::FUNCT_TNE:
            // Code bits in the reserved-looking middle field are permitted
            // trap-code operands; only the signed/unsigned predicate matters.
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "trap");
            break;
        case e_ee_opcode::FUNCT_MFSA:
        case e_ee_opcode::FUNCT_MTSA:
            // R5900 scalar shift-amount register transfers are genuine
            // instructions, but have no complete native lowering yet.
            insn.valid = e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, insn.funct == static_cast<uint8_t>(e_ee_opcode::FUNCT_MFSA)
                ? "mfsa" : "mtsa");
            break;
        default:
            insn.valid = e_ee_valid::INVALID;
            set_mnemonic(insn, "special_rsvd");
            break;
        }
        break;

    case 0x02: // J
        insn.type = e_ee_op_type::OP_TYPE_JTYPE;
        insn.opcode = e_ee_opcode::OP_J;
        insn.flow = e_ee_flow::FLOW_JUMP;
        insn.jump_target = ((pc + 4u) & 0xf0000000u) | ((word & 0x03ffffffu) << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "j");
        break;

    case 0x03: // JAL
        insn.type = e_ee_op_type::OP_TYPE_JTYPE;
        insn.opcode = e_ee_opcode::OP_JAL;
        insn.flow = e_ee_flow::FLOW_CALL;
        insn.jump_target = ((pc + 4u) & 0xf0000000u) | ((word & 0x03ffffffu) << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "jal");
        break;

    case 0x04: // BEQ
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BEQ;
        insn.flow = e_ee_flow::FLOW_BRANCH;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "beq");
        break;

    case 0x05: // BNE
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BNE;
        insn.flow = e_ee_flow::FLOW_BRANCH;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "bne");
        break;

    case 0x06: // BLEZ
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BLEZ;
        insn.flow = e_ee_flow::FLOW_BRANCH;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "blez");
        break;

    case 0x07: // BGTZ
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BGTZ;
        insn.flow = e_ee_flow::FLOW_BRANCH;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "bgtz");
        break;

    case 0x01: // REGIMM (BLTZ, BGEZ, BLTZL, BGEZL, traps, *AL, …)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_REGIMM;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        switch (static_cast<e_ee_opcode>(insn.rt))
        {
        case e_ee_opcode::REGIMM_BLTZ:
            insn.flow = e_ee_flow::FLOW_BRANCH;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "bltz");
            break;
        case e_ee_opcode::REGIMM_BGEZ:
            insn.flow = e_ee_flow::FLOW_BRANCH;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "bgez");
            break;
        case e_ee_opcode::REGIMM_BLTZL:
            insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
            insn.branch_likely = true;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "bltzl");
            break;
        case e_ee_opcode::REGIMM_BGEZL:
            insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
            insn.branch_likely = true;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "bgezl");
            break;
        case e_ee_opcode::REGIMM_BLTZAL:
        case e_ee_opcode::REGIMM_BGEZAL:
            // Branch-and-link writes GPR31=PC+8 on either branch outcome.
            // The link write is emitted before the ordinary delay slot.
            insn.flow = e_ee_flow::FLOW_BRANCH;
            insn.valid = (insn.rs != 31)
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, insn.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZAL)
                ? "bltzal" : "bgezal");
            break;
        case e_ee_opcode::REGIMM_BLTZALL:
        case e_ee_opcode::REGIMM_BGEZALL:
            // Likely branch-and-link also writes the link unconditionally,
            // but annuls its delay slot when the branch is not taken.
            insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
            insn.branch_likely = true;
            insn.valid = (insn.rs != 31)
                ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, insn.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZALL)
                ? "bltzall" : "bgezall");
            break;
        case e_ee_opcode::REGIMM_TGEI:
        case e_ee_opcode::REGIMM_TGEIU:
        case e_ee_opcode::REGIMM_TLTI:
        case e_ee_opcode::REGIMM_TLTIU:
        case e_ee_opcode::REGIMM_TEQI:
        case e_ee_opcode::REGIMM_TNEI:
            insn.flow = e_ee_flow::FLOW_NONE;
            insn.has_delay_slot = false;
            // REGIMM initially computes a branch target for every rt form.
            // Immediate traps have no target or delay slot and must not
            // appear as synthetic control-flow references.
            insn.branch_target = 0;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "regimm_trap");
            break;
        case e_ee_opcode::REGIMM_MTSAB:
            insn.flow = e_ee_flow::FLOW_NONE;
            insn.has_delay_slot = false;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "mtsab");
            break;
        case e_ee_opcode::REGIMM_MTSAH:
            insn.flow = e_ee_flow::FLOW_NONE;
            insn.has_delay_slot = false;
            insn.valid = e_ee_valid::VALID;
            set_mnemonic(insn, "mtsah");
            break;
        default:
            insn.flow = e_ee_flow::FLOW_NONE;
            insn.has_delay_slot = false;
            insn.valid = e_ee_valid::INVALID;
            set_mnemonic(insn, "regimm_reserved");
            break;
        }
        break;

    case 0x08: // ADDI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_ADDI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID; // checked 32-bit overflow -> EX_Ov
        set_mnemonic(insn, "addi");
        break;

    case 0x09: // ADDIU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_ADDIU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "addiu");
        break;

    case 0x0a: // SLTI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SLTI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "slti");
        break;

    case 0x0b: // SLTIU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SLTIU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sltiu");
        break;

    case 0x0c: // ANDI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_ANDI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "andi");
        break;

    case 0x0d: // ORI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_ORI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "ori");
        break;

    case 0x0e: // XORI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_XORI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "xori");
        break;

    case 0x0f: // LUI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LUI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lui");
        break;

    case 0x20: // LB
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LB;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lb");
        break;

    case 0x21: // LH
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LH;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lh");
        break;

    case 0x22: // LWL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LWL;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lwl");
        break;

    case 0x23: // LW
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LW;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lw");
        break;

    case 0x24: // LBU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LBU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lbu");
        break;

    case 0x25: // LHU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LHU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lhu");
        break;

    case 0x26: // LWR
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LWR;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lwr");
        break;

    case 0x28: // SB
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SB;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sb");
        break;

    case 0x29: // SH
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SH;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sh");
        break;

    case 0x2a: // SWL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SWL;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "swl");
        break;

    case 0x2b: // SW
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SW;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sw");
        break;

    case 0x2c: // SDL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SDL;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sdl");
        break;

    case 0x2d: // SDR
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SDR;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sdr");
        break;

    case 0x2e: // SWR
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SWR;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "swr");
        break;

    case 0x2f: // CACHE
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_CACHE;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "cache");
        break;

    case 0x31: // LWC1
    {
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LWC1;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lwc1");
        break;
    }
    case 0x27: // LWU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LWU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lwu");
        break;

    case 0x33: // PREF
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_PREF;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "pref");
        break;

    case 0x36: // LQC2 (VU0 vector quadword load)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LQC2;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lqc2");
        break;

    case 0x3e: // SQC2 (VU0 vector quadword store)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SQC2;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sqc2");
        break;

    case 0x37: // LD (R5900); 0x38 is not a valid EE LD alias
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LD;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "ld");
        break;

    case 0x39: // SWC1 (R5900)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SWC1;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "swc1");
        break;

    case 0x3f: // SD (R5900)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SD;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sd");
        break;

    case 0x1c: // MMI (multimedia/SIMD)
    {
        insn.type = e_ee_op_type::OP_TYPE_RTYPE;
        insn.opcode = e_ee_opcode::OP_MMI;
        insn.flow = e_ee_flow::FLOW_NONE;
        const char* name = kMmi[insn.funct];
        switch (insn.funct)
        {
        case 0x08: name = kMmi0[insn.sa & 31]; break;
        case 0x28: name = kMmi1[insn.sa & 31]; break;
        case 0x09: name = kMmi2[insn.sa & 31]; break;
        case 0x29: name = kMmi3[insn.sa & 31]; break;
        default: break;
        }
        set_mnemonic(insn, name);
        if (std::string_view(name) == "reserved")
            insn.valid = e_ee_valid::INVALID;
        else
            insn.valid = mmi_lowered_exactly(insn.funct, insn.sa & 31, insn.rs, insn.rt, insn.rd)
                         ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
        break;
    }

    case 0x10: // COP0 (system coprocessor)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_COP0;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.rs = (uint8_t)((word >> 21) & 0x1f); // COP0 fmt / CO
        insn.funct = (uint8_t)(word & 0x3f);
        switch (static_cast<e_ee_opcode>(insn.rs))
        {
        case e_ee_opcode::COP0_MFC0:
        case e_ee_opcode::COP0_MTC0:
            if (insn.sa == 0 && insn.funct == 0)
            {
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, insn.rs == 0 ? "mfc0" : "mtc0");
            }
            else
            {
                insn.valid = e_ee_valid::UNSUPPORTED;
                set_mnemonic(insn, "cop0_transfer");
            }
            break;
        case e_ee_opcode::COP0_BC0:
            // BC0F/T/FL/TL is recognised but not modelled by the current
            // backend. Preserve the real edge shape and fail closed in IR.
            if (insn.rt > 3)
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop0_reserved");
                break;
            }
            insn.valid = e_ee_valid::UNSUPPORTED;
            insn.flow = (insn.rt & 2) ? e_ee_flow::FLOW_BRANCH_LIKELY
                                      : e_ee_flow::FLOW_BRANCH;
            insn.branch_likely = (insn.rt & 2) != 0;
            insn.has_delay_slot = true;
            insn.branch_target = pc + 4 + ((int32_t)insn.immediate << 2);
            set_mnemonic(insn, "bc0");
            break;
        case e_ee_opcode::COP0_CO:
            // Only the exact ERET/EI/DI encodings used by the emitter are
            // promoted. TLB operations are valid instructions but require an
            // architectural TLB model; other CO encodings are reserved here.
            if (insn.rt != 0 || insn.rd != 0 || insn.sa != 0)
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop0_reserved");
                break;
            }
            switch (static_cast<e_ee_opcode>(insn.funct))
            {
            case e_ee_opcode::COP0_ERET:
                insn.flow = e_ee_flow::FLOW_RETURN;
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "eret");
                break;
            case e_ee_opcode::COP0_TLBR:
            case e_ee_opcode::COP0_TLBWI:
            case e_ee_opcode::COP0_TLBWR:
            case e_ee_opcode::COP0_TLBP:
                insn.valid = e_ee_valid::UNSUPPORTED;
                set_mnemonic(insn, "tlb");
                break;
            case e_ee_opcode::COP0_EI:
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "ei");
                break;
            case e_ee_opcode::COP0_DI:
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "di");
                break;
            default:
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop0_reserved");
                break;
            }
            break;
        default:
            insn.valid = e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "cop0");
            break;
        }
        break;

    case 0x11: // COP1 (floating point coprocessor)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_COP1;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.rs = (uint8_t)((word >> 21) & 0x1f); // COP1 function
        switch (static_cast<e_ee_opcode>(insn.rs))
        {
        case e_ee_opcode::COP1_MFC1:
        case e_ee_opcode::COP1_MTC1:
            if (insn.sa == 0 && insn.funct == 0)
            {
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "cop1_transfer");
            }
            else
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop1_reserved");
            }
            break;
        case e_ee_opcode::COP1_CFC1:
        case e_ee_opcode::COP1_CTC1:
            if (insn.sa != 0 || insn.funct != 0)
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop1_reserved");
            }
            else if (insn.rd == 31)
            {
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "cop1_fcsr");
            }
            else
            {
                insn.valid = e_ee_valid::UNSUPPORTED;
                set_mnemonic(insn, "cop1_control");
            }
            break;
        case static_cast<e_ee_opcode>(0x08): // BC1F/T/FL/TL
        {
            if (insn.rt > 3)
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop1_reserved");
                break;
            }
            insn.valid = e_ee_valid::VALID;
            insn.flow = (insn.rt & 2) ? e_ee_flow::FLOW_BRANCH_LIKELY
                                      : e_ee_flow::FLOW_BRANCH;
            insn.has_delay_slot = true;
            insn.branch_likely = (insn.rt & 2) != 0;
            insn.branch_target = pc + 4 + ((int32_t)insn.immediate << 2);
            set_mnemonic(insn, "bc1");
            break;
        }
        case static_cast<e_ee_opcode>(0x10): // fmt=S
            if (cop1_s_defined(insn.funct))
            {
                const bool compare = insn.funct == 0x30 || insn.funct == 0x32 ||
                                     insn.funct == 0x34 || insn.funct == 0x36;
                // The current CPU state models FCC0 only. Other condition
                // codes remain recognised but unsupported until all FCC bits
                // and BC1 condition-code selection are represented.
                const bool supported = cop1_s_lowered_exactly(insn.funct) &&
                                       (!compare || insn.sa == 0);
                insn.valid = supported
                    ? e_ee_valid::VALID : e_ee_valid::UNSUPPORTED;
                set_mnemonic(insn, "cop1_s");
            }
            else
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop1_reserved");
            }
            break;
        case static_cast<e_ee_opcode>(0x14): // fmt=W: CVT.S.W is the only table entry
            if (insn.funct == 0x20)
            {
                insn.valid = e_ee_valid::VALID;
                set_mnemonic(insn, "cvt_s_w");
            }
            else
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop1_reserved");
            }
            break;
        case static_cast<e_ee_opcode>(0x11): // fmt=D is not implemented by R5900
        case static_cast<e_ee_opcode>(0x15): // fmt=L is not implemented by R5900
            insn.valid = e_ee_valid::INVALID;
            set_mnemonic(insn, "cop1_reserved");
            break;
        default:
            insn.valid = e_ee_valid::UNSUPPORTED;
            set_mnemonic(insn, "cop1");
            break;
        }
        break;

    case 0x12: // COP2 (VU0)
    {
        insn.type = e_ee_op_type::OP_TYPE_RTYPE;
        insn.opcode = e_ee_opcode::OP_COP2;
        insn.flow = e_ee_flow::FLOW_NONE;
        switch (insn.rs)
        {
        // Transfers use rd as the VU register index. Bits 10..1 are
        // reserved, but bit 0 is the architectural I (interlock) bit.
        // .NI (0) and .I (1) share the same register transfer helper.
        case 0x01: // QMFC2: GPR rt <- VU0 VF[rd]
        case 0x02: // CFC2: GPR rt <- VU0 VI[rd]
        case 0x05: // QMTC2: VU0 VF[rd] <- GPR rt
        case 0x06: // CTC2: VU0 VI[rd] <- GPR rt
        {
            static const char* const names[] = {
                "qmfc2", "cfc2", "reserved", "reserved",
                "qmtc2", "ctc2"
            };
            const uint32_t selector = insn.rs;
            insn.valid = (insn.sa == 0 && insn.funct <= 1u)
                ? e_ee_valid::VALID : e_ee_valid::INVALID;
            // Preserve the interlock variant in diagnostic output.
            if (insn.funct == 1u)
            {
                char mnemonic[16]{};
                std::snprintf(mnemonic, sizeof(mnemonic),
                              "%s.i", names[selector - 1]);
                set_mnemonic(insn, mnemonic);
            }
            else
            {
                set_mnemonic(insn, names[selector - 1]);
            }
            break;
        }
        case 0x08: // BC2F/T/FL/TL; the full rt field selects the table entry
        {
            // PCSX2's 32-entry BC2 table defines only rt=0..3. Masking the
            // low two bits would incorrectly accept reserved rt values.
            if (insn.rt > 3)
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop2_reserved");
                break;
            }

            const uint32_t branch_kind = insn.rt;
            insn.flow = (branch_kind & 2)
                ? e_ee_flow::FLOW_BRANCH_LIKELY
                : e_ee_flow::FLOW_BRANCH;
            insn.branch_likely = (branch_kind & 2) != 0;
            insn.has_delay_slot = true;
            insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
            insn.valid = e_ee_valid::VALID;
            static const char* const names[] = {"bc2f", "bc2t", "bc2fl", "bc2tl"};
            set_mnemonic(insn, names[branch_kind]);
            break;
        }
        default:
            if (insn.rs >= 0x10)
            {
                const uint32_t funct = insn.funct;
                bool defined = cop2_special1_defined(funct);
                if (defined && funct >= 0x3c)
                {
                    const uint32_t special2_index =
                        (word & 0x3u) | ((word >> 4) & 0x7cu);
                    defined = cop2_special2_defined(special2_index);
                }
                insn.valid = defined ? e_ee_valid::UNSUPPORTED : e_ee_valid::INVALID;
                // Identify the retail SDBZ matrix-multiply FMAC operations
                // exactly, while keeping them UNSUPPORTED until their VU0
                // ACC/flag pipeline and operand forwarding are implemented.
                // SPECIAL2 uses {sa, funct[1:0]}, not the raw funct field.
                const char* name = defined ? "vu0_macro" : "cop2_reserved";
                if (defined && funct == 0x0bu)
                {
                    name = "vmaddw";
                }
                else if (defined && funct == 0x01u)
                {
                    name = "vaddy";
                }
                else if (defined && funct == 0x02u)
                {
                    name = "vaddz";
                }
                else if (defined && funct == 0x20u)
                {
                    name = "vaddq";
                }
                else if (defined && funct == 0x1cu)
                {
                    name = "vmulq";
                }
                else if (defined && funct == 0x2au)
                {
                    name = "vmul";
                }
                else if (defined && funct >= 0x3cu)
                {
                    const uint32_t special2_index =
                        (word & 0x3u) | ((word >> 4) & 0x7cu);
                    switch (special2_index)
                    {
                    case 0x18: name = "vmulax"; break;
                    case 0x09: name = "vmadday"; break;
                    case 0x0a: name = "vmaddaz"; break;
                    case 0x39: name = "vsqrt"; break;
                    case 0x3a: name = "vrsqrt"; break;
                    case 0x3b: name = "vwaitq"; break;
                    case 0x34: name = "vlqi"; break;
                    case 0x35: name = "vsqi"; break;
                    case 0x36: name = "vlqd"; break;
                    case 0x37: name = "vsqd"; break;
                    default: break;
                    }
                }
                // Matrix FMAC, actual reached vector length and Q
                // square-root instructions, and indexed vector memory
                // are native. All other COP2 macros remain unsupported.
                const uint32_t special2_index =
                    (word & 0x3u) | ((word >> 4) & 0x7cu);
                if (defined && (funct == 0x0bu ||
                    funct == 0x01u || funct == 0x02u ||
                    funct == 0x20u || funct == 0x1cu ||
                    funct == 0x2au ||
                    (funct >= 0x3cu &&
                     (special2_index == 0x39u ||
                      special2_index == 0x3au ||
                       special2_index == 0x3bu ||
                      special2_index == 0x18u ||
                      special2_index == 0x09u ||
                      special2_index == 0x0au ||
                       (special2_index >= 0x34u &&
                        special2_index <= 0x37u)))))
                {
                    insn.valid = e_ee_valid::VALID;
                }
                set_mnemonic(insn, name);
            }
            else
            {
                insn.valid = e_ee_valid::INVALID;
                set_mnemonic(insn, "cop2_reserved");
            }
            break;
        }
        break;
    }

    case 0x14: // BEQL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BEQL;
        insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.branch_likely = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "beql");
        break;

    case 0x15: // BNEL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BNEL;
        insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.branch_likely = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "bnel");
        break;

    case 0x16: // BLEZL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BLEZL;
        insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.branch_likely = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "blezl");
        break;

    case 0x17: // BGTZL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_BGTZL;
        insn.flow = e_ee_flow::FLOW_BRANCH_LIKELY;
        insn.branch_target = (int32_t)(pc + 4) + ((int32_t)insn.immediate << 2);
        insn.has_delay_slot = true;
        insn.branch_likely = true;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "bgtzl");
        break;

    case 0x18: // DADDI
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_DADDI;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID; // checked 64-bit overflow -> EX_Ov
        set_mnemonic(insn, "daddi");
        break;

    case 0x19: // DADDIU
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_DADDIU;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "daddiu");
        break;

    case 0x1a: // LDL
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LDL;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "ldl");
        break;

    case 0x1b: // LDR
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LDR;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "ldr");
        break;

    case 0x1e: // LQ (load quadword)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_LQ;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "lq");
        break;

    case 0x1f: // SQ (store quadword)
        insn.type = e_ee_op_type::OP_TYPE_ITYPE;
        insn.opcode = e_ee_opcode::OP_SQ;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::VALID;
        set_mnemonic(insn, "sq");
        break;

    case 0x38: // SC (MIPS III), explicitly unsupported by EE hardware
        // Sony's instruction manual labels this opcode 'unsupported':
        // executing it generates the undefined-instruction exception.
        // Our UNSUPPORTED enum instead means a legal EE instruction
        // awaiting native lowering, so this is INVALID on the EE.
        insn.opcode = e_ee_opcode::OP_INVALID;
        insn.flow = e_ee_flow::FLOW_NONE;
        insn.valid = e_ee_valid::INVALID;
        set_mnemonic(insn, "sc");
        break;

    default:
        insn.opcode = e_ee_opcode::OP_INVALID;
        insn.flow = e_ee_flow::FLOW_NONE;
        // Unassigned primary R5900 selector, not an implemented ISA
        // instruction awaiting lowering. Preserve the original word.
        insn.valid = e_ee_valid::INVALID;
        set_mnemonic(insn, "unknown");
        break;
    }

    return insn;
}

} // namespace ee


