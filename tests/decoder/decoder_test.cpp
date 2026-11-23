// Decoder regression test for the MMI classification and the VALID/UNSUPPORTED
// contract. Words are composed from fields so each row documents the encoding.
#include "ee_decoder.h"
#include "instruction_lifter.h"
#include "function_analyzer.h"
#include "ee_recomp.h"
#include <cstdio>
#include <map>
#include <cstring>
#include <set>
using namespace ee;
static int g_pass = 0, g_fail = 0;
static uint32_t R(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct)
{ return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct; }

static void expect(const char* what, uint32_t word, const char* mn, e_ee_valid v)
{
    s_ee_instruction i = decode(0x100000, word);
    bool ok = std::strcmp(i.mnemonic, mn) == 0 && i.valid == v;
    if (ok) ++g_pass; else { ++g_fail;
        std::printf("FAIL %-10s %08x got=%s/%d want=%s/%d\n", what, word, i.mnemonic, (int)i.valid, mn, (int)v); }
}

int main()
{
    const auto V = e_ee_valid::VALID, U = e_ee_valid::UNSUPPORTED, X = e_ee_valid::INVALID;
    // Signed-overflow arithmetic and both integer trap families now
    // enter the real native EX_Ov / EX_Tr exception path. The independent
    // legacy C++ emitter must not alias them to unchecked arithmetic/ERET.
    for (uint32_t funct : {0x20u, 0x22u, 0x2cu, 0x2eu})
    {
        const auto ins = decode(0x00150000u, R(0x00, 4, 5, 6, 0, funct));
        const bool ok = ins.valid == V &&
            lift(ins).kind == LOP_CHECKED_SIGNED;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL signed checked SPECIAL 0x%x\n", funct);
    }
    expect("DADD bad sa", R(0x00,4,5,6,1,0x2c), "signed_arith", U);
    expect("DSUB bad sa", R(0x00,4,5,6,1,0x2e), "signed_arith", U);
    for (uint32_t opcode : {0x08u, 0x18u})
    {
        const auto ins = decode(0x00150000u,
            (opcode << 26) | (4u << 21) | (6u << 16) | 0xFFFFu);
        const bool ok = ins.valid == V &&
            lift(ins).kind == LOP_CHECKED_SIGNED;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL checked immediate 0x%x\n", opcode);
    }
    for (uint32_t funct : {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x36u})
    {
        // Non-zero ten-bit trap-code fields are architecturally allowed.
        const auto ins = decode(0x00150000u, R(0x00,4,5,6,17,funct));
        const bool ok = ins.valid == V &&
            lift(ins).kind == LOP_CHECKED_TRAP;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL checked SPECIAL trap 0x%x\n", funct);
    }
    for (uint32_t rt : {0x08u,0x09u,0x0au,0x0bu,0x0cu,0x0eu})
    {
        const auto ins = decode(0x00150000u,
            (0x01u << 26) | (4u << 21) | (rt << 16) | 0xFFFFu);
        const bool ok = ins.valid == V &&
            ins.flow == e_ee_flow::FLOW_NONE && !ins.has_delay_slot &&
            lift(ins).kind == LOP_CHECKED_TRAP;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL checked REGIMM trap 0x%x\n", rt);
    }
    // Scalar comparisons are lowered exactly as 64-bit signed/unsigned tests.
    expect("slt", R(0x00,4,5,6,0,0x2a), "slt", V);
    expect("sltu", R(0x00,4,5,6,0,0x2b), "sltu", V);
    expect("slt bad sa", R(0x00,4,5,6,1,0x2a), "slt", U);
    expect("sltu bad sa", R(0x00,4,5,6,1,0x2b), "sltu", U);
    expect("slti", (0x0au << 26) | (4u << 21) | (6u << 16) | 0xffffu,
           "slti", V);
    expect("sltiu", (0x0bu << 26) | (4u << 21) | (6u << 16) | 0xffffu,
           "sltiu", V);
    // The R5900 standard table has LD at primary 0x37 only.
    // In particular 0x38, though SC on some MIPS variants, is not
    // a second LD encoding on PS2. Unknown selectors fail closed.
    expect("ld primary", R(0x37, 4, 5, 0, 0, 0), "ld", V);
    // SC is a defined MIPS opcode but deliberately unavailable on EE.
    expect("0x38 EE-unsupported SC", R(0x38, 4, 5, 0, 0, 0), "sc", X);
    expect("0x30 reserved", R(0x30, 4, 5, 0, 0, 0), "unknown", X);
    expect("0x35 reserved", R(0x35, 4, 5, 0, 0, 0), "unknown", X);
    expect("0x3d reserved", R(0x3d, 4, 5, 0, 0, 0), "unknown", X);
    // SPECIAL 0x28/0x29 are real R5900 SA transfers, not UNKNOWN
    // and not yet implemented; other reserved functs are INVALID.
    expect("mfsa", R(0x00, 0, 0, 6, 0, 0x28), "mfsa", U);
    expect("mtsa", R(0x00, 4, 0, 0, 0, 0x29), "mtsa", U);
    expect("special reserved", R(0x00, 4, 5, 6, 0, 0x01),
           "special_rsvd", X);
    // MMI groups: funct 0x08=MMI0, 0x28=MMI1, 0x09=MMI2, 0x29=MMI3; sub-op in sa.
    expect("paddw",   R(0x1c,4,5,6,0x00,0x08), "paddw",  V);
    expect("psubh",   R(0x1c,4,5,6,0x05,0x08), "psubh",  V);
    expect("pmaxh",   R(0x1c,4,5,6,0x07,0x08), "pmaxh",  V);
    expect("paddb",   R(0x1c,4,5,6,0x08,0x08), "paddb",  V);
    expect("pand",    R(0x1c,4,5,6,0x12,0x09), "pand",   V);
    expect("pxor",    R(0x1c,4,5,6,0x13,0x09), "pxor",   V);
    expect("por",     R(0x1c,4,5,6,0x12,0x29), "por",    V);
    expect("pnor",    R(0x1c,4,5,6,0x13,0x29), "pnor",   V);
    expect("pcpyud",  R(0x1c,4,5,6,0x0e,0x29), "pcpyud", V);
    // Verified from the US SDBZ ELF: PC 0x0018E154.
    expect("pcpyud SDBZ", 0x70491ba9u, "pcpyud", V);
    {
        // This exact SDBZ instruction was decoded VALID but lift() used
        // to return LOP_UNSUPPORTED, producing a false coverage gap.
        const auto ins = decode(0x0018e154u, 0x70491ba9u);
        const bool ok = ins.valid == V &&
                        lift(ins).kind == LOP_MMI_COPY_UPPER_D;
        ok ? ++g_pass : ++g_fail;
        if (!ok)
        {
            std::printf("FAIL SDBZ PCPYUD decode/lift/emitter classification\n");
        }
    }
    // Every MMI encoding declared VALID must have a non-unsupported
    // lifter kind; the LLVM emitter dispatches MMI using raw opcodes.
    // Test both unary (rs=0) and binary (rs=4) forms exhaustively,
    // including selector sub-opcodes that otherwise receive no unit test.
    for (uint32_t funct = 0u; funct < 64u; ++funct)
    {
        for (uint32_t sa = 0u; sa < 32u; ++sa)
        {
            for (uint32_t rs : {0u, 4u})
            {
                const uint32_t word = R(0x1cu, rs, 5u, 6u, sa, funct);
                const auto ins = decode(0x0018e154u, word);
                if (ins.valid != V)
                {
                    continue;
                }
                const bool ok = lift(ins).kind != LOP_UNSUPPORTED;
                ok ? ++g_pass : ++g_fail;
                if (!ok)
                {
                    std::printf("FAIL VALID MMI unsupported by lifter: word=%08x "
                                "funct=%02x sa=%02x rs=%u\n",
                                word, funct, sa, rs);
                }
            }
        }
    }
    expect("mfhi1",   R(0x1c,0,0,6,0,0x10),    "mfhi1",  V);
    expect("mtlo1",   R(0x1c,4,0,0,0,0x13),    "mtlo1",  V);
    // Real ops that are NOT lowered exactly must never be VALID (previously a
    // silent fallback add/and/or/move was emitted for them).
    expect("paddsw",  R(0x1c,4,5,6,0x10,0x08), "paddsw", U);   // saturating: was plain add
    expect("pextlw",  R(0x1c,4,5,6,0x12,0x08), "pextlw", U);   // was lowered as OR
    expect("pcpyld",  R(0x1c,4,5,6,0x0e,0x09), "pcpyld", V);   // exact lower-64 interleave
    expect("pcpyh",   R(0x1c,0,5,6,0x1b,0x29), "pcpyh",  V);   // independent halfword broadcast
    expect("pcpyh-bad-rs", R(0x1c,4,5,6,0x1b,0x29), "pcpyh", U); // unary instruction requires rs=0
    expect("pabsw",  R(0x1c,0,5,6,0x01,0x28), "pabsw",  V);
    expect("pceqw",  R(0x1c,4,5,6,0x02,0x28), "pceqw",  V);
    expect("pminw",  R(0x1c,4,5,6,0x03,0x28), "pminw",  V);
    expect("pabsh",  R(0x1c,0,5,6,0x05,0x28), "pabsh",  V);
    expect("pceqh",  R(0x1c,4,5,6,0x06,0x28), "pceqh",  V);
    expect("pminh",  R(0x1c,4,5,6,0x07,0x28), "pminh",  V);
    expect("pceqb",  R(0x1c,4,5,6,0x0a,0x28), "pceqb",  V);
    expect("padsbh", R(0x1c,4,5,6,0x04,0x28), "padsbh", V);
    expect("pextuw", R(0x1c,4,5,6,0x12,0x28), "pextuw", V);
    expect("pextuh", R(0x1c,4,5,6,0x16,0x28), "pextuh", V);
    expect("pextub", R(0x1c,4,5,6,0x1a,0x28), "pextub", V);
    expect("qfsrv",  R(0x1c,4,5,6,0x1b,0x28), "qfsrv",  V);
    expect("padduw", R(0x1c,4,5,6,0x10,0x28), "padduw", V);
    expect("psubuw", R(0x1c,4,5,6,0x11,0x28), "psubuw", V);
    expect("padduh", R(0x1c,4,5,6,0x14,0x28), "padduh", V);
    expect("psubuh", R(0x1c,4,5,6,0x15,0x28), "psubuh", V);
    expect("paddub", R(0x1c,4,5,6,0x18,0x28), "paddub", V);
    expect("psubub", R(0x1c,4,5,6,0x19,0x28), "psubub", V);
    expect("pabsw rs",R(0x1c,4,5,6,0x01,0x28), "pabsw", U);
    expect("mmi1 reserved", R(0x1c,4,5,6,0x00,0x28), "reserved", X);
    expect("madd",    R(0x1c,4,5,6,0,0x00),    "madd",   V);
    expect("maddu",   R(0x1c,4,5,6,0,0x01),    "maddu",  V);
    expect("madd1",   R(0x1c,4,5,6,0,0x20),    "madd1",  V);
    expect("maddu1",  R(0x1c,4,5,6,0,0x21),    "maddu1", V);
    expect("madd bad sa", R(0x1c,4,5,6,1,0x00), "madd", U);
    {
        const auto madd = decode(0x100000, R(0x1c,4,5,6,0,0x00));
        const auto maddu = decode(0x100000, R(0x1c,4,5,6,0,0x01));
        const auto madd1 = decode(0x100000, R(0x1c,4,5,6,0,0x20));
        const auto maddu1 = decode(0x100000, R(0x1c,4,5,6,0,0x21));
        const auto mult1 = decode(0x100000, R(0x1c,4,5,6,0,0x18));
        const auto multu1 = decode(0x100000, R(0x1c,4,5,6,0,0x19));
        const auto div1 = decode(0x100000, R(0x1c,4,5,6,0,0x1a));
        const auto divu1 = decode(0x100000, R(0x1c,4,5,6,0,0x1b));
        const auto mtsab = decode(0x100000, R(0x01,4,0x18,0,0,0));
        const auto mtsah = decode(0x100000, R(0x01,4,0x19,0,0,0));
        const auto pabsw = decode(0x100000, R(0x1c,0,5,6,1,0x28));
        const auto pceqw = decode(0x100000, R(0x1c,4,5,6,2,0x28));
        const auto pminw = decode(0x100000, R(0x1c,4,5,6,3,0x28));
        const auto pabsh = decode(0x100000, R(0x1c,0,5,6,5,0x28));
        const auto pceqh = decode(0x100000, R(0x1c,4,5,6,6,0x28));
        const auto pminh = decode(0x100000, R(0x1c,4,5,6,7,0x28));
        const auto pceqb = decode(0x100000, R(0x1c,4,5,6,10,0x28));
        const auto padsbh = decode(0x100000, R(0x1c,4,5,6,4,0x28));
        const auto pextuw = decode(0x100000, R(0x1c,4,5,6,18,0x28));
        const auto pextuh = decode(0x100000, R(0x1c,4,5,6,22,0x28));
        const auto pextub = decode(0x100000, R(0x1c,4,5,6,26,0x28));
        const auto qfsrv = decode(0x100000, R(0x1c,4,5,6,27,0x28));
        const auto padduw = decode(0x100000, R(0x1c,4,5,6,16,0x28));
        const auto psubuw = decode(0x100000, R(0x1c,4,5,6,17,0x28));
        const auto padduh = decode(0x100000, R(0x1c,4,5,6,20,0x28));
        const auto psubuh = decode(0x100000, R(0x1c,4,5,6,21,0x28));
        const auto paddub = decode(0x100000, R(0x1c,4,5,6,24,0x28));
        const auto psubub = decode(0x100000, R(0x1c,4,5,6,25,0x28));
        const bool ok = lift(madd).kind == LOP_MADD &&
                        lift(maddu).kind == LOP_MADDU &&
                        lift(madd1).kind == LOP_MADD1 &&
                        lift(maddu1).kind == LOP_MADDU1 &&
                        lift(mult1).kind == LOP_MULT1 &&
                        lift(multu1).kind == LOP_MULTU1 &&
                        lift(div1).kind == LOP_DIV1 &&
                        lift(divu1).kind == LOP_DIVU1 &&
                        lift(mtsab).kind == LOP_MTSAB &&
                        lift(mtsah).kind == LOP_MTSAH &&
                        lift(pabsw).kind == LOP_MMI_ABS_W &&
                        lift(pceqw).kind == LOP_MMI_CEQ_W &&
                        lift(pminw).kind == LOP_MMI_MINS_W &&
                        lift(pabsh).kind == LOP_MMI_ABS_H &&
                        lift(pceqh).kind == LOP_MMI_CEQ_H &&
                        lift(pminh).kind == LOP_MMI_MINS_H &&
                        lift(pceqb).kind == LOP_MMI_CEQ_B &&
                        lift(padsbh).kind == LOP_MMI_ADDSUB_H &&
                        lift(pextuw).kind == LOP_MMI_EXTU_W &&
                        lift(pextuh).kind == LOP_MMI_EXTU_H &&
                        lift(pextub).kind == LOP_MMI_EXTU_B &&
                        lift(qfsrv).kind == LOP_MMI_QFSRV &&
                        lift(padduw).kind == LOP_MMI_ADDU_SAT_W &&
                        lift(psubuw).kind == LOP_MMI_SUBU_SAT_W &&
                        lift(padduh).kind == LOP_MMI_ADDU_SAT_H &&
                        lift(psubuh).kind == LOP_MMI_SUBU_SAT_H &&
                        lift(paddub).kind == LOP_MMI_ADDU_SAT_B &&
                        lift(psubub).kind == LOP_MMI_SUBU_SAT_B;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL MADD/MADDU/MULT1/DIV1/MMI1 pipeline lift mapping\n");
    }
    expect("mult1",   R(0x1c,4,5,6,0,0x18),    "mult1",  V);
    expect("multu1",  R(0x1c,4,5,6,0,0x19),    "multu1", V);
    expect("div1",    R(0x1c,4,5,6,0,0x1a),    "div1",   V);
    expect("divu1",   R(0x1c,4,5,6,0,0x1b),    "divu1",  V);
    expect("div1 bad sa", R(0x1c,4,5,6,1,0x1a), "div1", U);
    expect("divu1 bad sa", R(0x1c,4,5,6,1,0x1b), "divu1", U);
    expect("mult1 bad sa", R(0x1c,4,5,6,1,0x18), "mult1", U);
    expect("pmfhl",   R(0x1c,0,0,6,0,0x30),    "pmfhl",  U);
    expect("psllh",   R(0x1c,0,5,6,3,0x34),    "psllh",  U);
    expect("reserved",R(0x1c,0,0,0,0x1f,0x28), "reserved", X);
    // COP2 field decoding is hierarchical: rs selects transfer, BC2, or VU0
    // macro mode; the low funct/sa fields are only interpreted inside macro mode.
    expect("qmfc2",   R(0x12,1,4,6,0,0), "qmfc2", V);
    expect("cfc2",    R(0x12,2,4,6,0,0), "cfc2", V);
    expect("qmtc2",   R(0x12,5,4,6,0,0), "qmtc2", V);
    expect("ctc2",    R(0x12,6,4,6,0,0), "ctc2", V);

    // Bit 0 is the actual VU0 transfer interlock, NOT a reserved bit.
    for (uint32_t selector : {1u, 2u, 5u, 6u})
    {
        const char* names[4] = {"qmfc2.i", "cfc2.i", "qmtc2.i", "ctc2.i"};
        const uint32_t index = selector == 1u ? 0u :
            selector == 2u ? 1u : selector == 5u ? 2u : 3u;
        const uint32_t word = R(0x12, selector, 4, 6, 0, 1);
        const auto ins = decode(0x00173258, word);
        const auto lifted = lift(ins);
        const auto want = (selector == 1u || selector == 2u)
            ? LOP_MFC2 : LOP_MTC2;
        const bool ok = ins.valid == V &&
            std::strcmp(ins.mnemonic, names[index]) == 0 &&
            lifted.kind == want;
        ok ? ++g_pass : ++g_fail;
        if (!ok)
        {
            std::printf("FAIL COP2 .I transfer selector=%u word=%08x\n",
                        selector, word);
        }
        // Other low bits remain reserved even if the interlock bit is set.
        const char* base_names[4] = {"qmfc2", "cfc2", "qmtc2", "ctc2"};
        expect("cop2 bit1 reserved",
               R(0x12, selector, 4, 6, 0, 3), base_names[index], X);
    }

    // Cover all actual COP2 transfer opcodes reached by the SDBZ startup.
    // The native emitter already runs these helpers; lifter coverage must
    // agree rather than counting successful execution as UNSUPPORTED.
    {
        const uint32_t words[] =
        {
            0x48460800u, 0x48230800u, 0x48c50800u,
            0x48220800u, 0x48c60800u, 0x48a30800u
        };
        const e_lift_op_kind expected[] =
        {
            LOP_MFC2, LOP_MFC2, LOP_MTC2,
            LOP_MFC2, LOP_MTC2, LOP_MTC2
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            const auto inst = decode(0x00173258u + 4u * i, words[i]);
            const bool ok = inst.valid == V &&
                lift(inst).kind == expected[i];
            ok ? ++g_pass : ++g_fail;
            if (!ok)
            {
                std::printf("FAIL SDBZ COP2 transfer %u: %08x kind=%d\n",
                            i, words[i], static_cast<int>(lift(inst).kind));
            }
        }
    }

    // All four transfer forms reserve sa/funct. Do not classify malformed
    // words as merely unsupported valid instructions.
    expect("qmfc2 bad sa",   R(0x12,1,4,6,1,0), "qmfc2", X);
    expect("qmfc2 bad funct",R(0x12,1,4,6,0,2), "qmfc2", X);
    expect("cfc2 bad sa",    R(0x12,2,4,6,1,0), "cfc2", X);
    expect("cfc2 bad funct", R(0x12,2,4,6,0,2), "cfc2", X);
    expect("qmtc2 bad sa",   R(0x12,5,4,6,1,0), "qmtc2", X);
    expect("qmtc2 bad funct",R(0x12,5,4,6,0,2), "qmtc2", X);
    expect("ctc2 bad sa",    R(0x12,6,4,6,1,0), "ctc2", X);
    expect("ctc2 bad funct", R(0x12,6,4,6,0,2), "ctc2", X);
    expect("vadd",    R(0x12,0x10,4,5,0,0x28), "vu0_macro", U);
    expect("vadda",   R(0x12,0x10,4,5,0,0x3c), "vu0_macro", U); // SPECIAL2 index 0

    // Directly from SDBZ (US) 0x00108240..0x0010827C: a 4x4 matrix
    // multiply using the VU0 accumulator. Decode the actual 16 words
    // Decode all original instructions through the guarded FMAC runtime.
    {
        const uint32_t matrix_words[] =
        {
            0x4be821bcu, 0x4be828bdu, 0x4be830beu, 0x4be83a0bu,
            0x4be921bcu, 0x4be928bdu, 0x4be930beu, 0x4be93a4bu,
            0x4bea21bcu, 0x4bea28bdu, 0x4bea30beu, 0x4bea3a8bu,
            0x4beb21bcu, 0x4beb28bdu, 0x4beb30beu, 0x4beb3acbu,
        };
        const char* const names[] = {"vmulax", "vmadday", "vmaddaz", "vmaddw"};
        for (uint32_t index = 0; index < 16; ++index)
        {
            const auto inst = decode(0x00108240u + 4u * index, matrix_words[index]);
            const uint32_t column = 8u + (index / 4u);
            const uint32_t row = index % 4u;
            const bool ok = inst.valid == V &&
                            inst.opcode == e_ee_opcode::OP_COP2 &&
                            inst.rs == 31u && inst.rt == column &&
                            inst.rd == row + 4u &&
                            std::strcmp(inst.mnemonic, names[row]) == 0 &&
                            lift(inst).kind == LOP_VU0_MACRO;
            ok ? ++g_pass : ++g_fail;
            if (!ok)
            {
                std::printf("FAIL SDBZ VU0 matrix macro %u: %08x -> %s/%d\\n",
                            index, matrix_words[index], inst.mnemonic, (int)inst.valid);
            }
        }
    }

    // The retail SDBZ COP2 opcode which blocked boot, followed by its
    // three adjacent legal indexed VU0 memory variants.
    {
        const uint32_t words[] = {
            0x4be10b7cu, 0x4be10b7du, 0x4be10b7eu, 0x4be10b7fu
        };
        const char* names[] = {"vlqi", "vsqi", "vlqd", "vsqd"};
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            const auto inst = decode(0x00173264u + i * 4u, words[i]);
            const bool ok = inst.valid == V &&
                std::strcmp(inst.mnemonic, names[i]) == 0 &&
                lift(inst).kind == LOP_VU0_MACRO;
            ok ? ++g_pass : ++g_fail;
            if (!ok)
            {
                std::printf("FAIL SDBZ VU0 indexed macro %u: %08x -> %s/%d\n",
                            i, words[i], inst.mnemonic, (int)inst.valid);
            }
        }
    }

    {
        // These six original US game words form the reached VF4
        // squared-length -> SQRT(Q) -> length sequence, and must be
        // lifted to real native VU0 macro helpers (not HLE/no-ops).
        constexpr uint32_t k_words[] = {
            0x4bc4212au, 0x4b042101u, 0x4b042102u,
            0x4a0403bdu, 0x4a0003bfu, 0x4b000120u
        };
        constexpr const char* k_names[] = {
            "vmul", "vaddy", "vaddz", "vsqrt", "vwaitq", "vaddq"
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            const auto ins = decode(0x001069ecu + i * 4u, k_words[i]);
            const bool ok = ins.valid == V &&
                std::strcmp(ins.mnemonic, k_names[i]) == 0 &&
                lift(ins).kind == LOP_VU0_MACRO;
            ok ? ++g_pass : ++g_fail;
            if (!ok)
            {
                std::printf(
                    "FAIL retail SDBZ vector-length macro %u: %08x -> %s/%d\\n",
                    i, k_words[i], ins.mnemonic, (int)ins.valid);
            }
        }
    }

    {
        // Six original EE instructions at 0x0010697c..0x00106990.
        // RSQRT Q and its VMULq consumer must lower to native VU0.
        constexpr uint32_t k_words[] = {
            0x4bc4216au, 0x4b052941u, 0x4b052942u,
            0x4a6503beu, 0x4a0003bfu, 0x4bc0211cu
        };
        constexpr const char* k_names[] = {
            "vmul", "vaddy", "vaddz", "vrsqrt", "vwaitq", "vmulq"
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            const auto ins = decode(0x0010697cu + i * 4u, k_words[i]);
            const bool ok = ins.valid == V &&
                std::strcmp(ins.mnemonic, k_names[i]) == 0 &&
                lift(ins).kind == LOP_VU0_MACRO;
            ok ? ++g_pass : ++g_fail;
            if (!ok)
            {
                std::printf(
                    "FAIL retail RSQRT/VMULq macro %u: %08x -> %s/%d\n",
                    i, k_words[i], ins.mnemonic, (int)ins.valid);
            }
        }
    }

    expect("reserved macro", R(0x12,0x10,4,5,0,0x33), "cop2_reserved", X);
    expect("reserved special2", R(0x12,0x10,4,5,20,0x3c), "cop2_reserved", X); // index 80
    expect("reserved rs", R(0x12,0,4,5,0,0), "cop2_reserved", X);
    expect("reserved class", R(0x12,9,4,5,0,0), "cop2_reserved", X);
    {
        const auto b = decode(0x100000, R(0x12,8,0,0,0,0x02));
        const bool ok = b.valid == V && b.flow == e_ee_flow::FLOW_BRANCH &&
                        b.has_delay_slot && !b.branch_likely &&
                        b.branch_target == 0x0010000c &&
                        std::strcmp(b.mnemonic, "bc2f") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2f flow/target decode\n");
    }
    {
        const auto b = decode(0x100000, R(0x12,8,2,0,0,0x02));
        const bool ok = b.valid == V && b.flow == e_ee_flow::FLOW_BRANCH_LIKELY &&
                        b.has_delay_slot && b.branch_likely &&
                        std::strcmp(b.mnemonic, "bc2fl") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2fl likely decode\n");
    }
    {
        const auto b = decode(0x100000, R(0x12,8,1,0,0,0x02));
        const bool ok = b.valid == V && b.flow == e_ee_flow::FLOW_BRANCH &&
                        b.has_delay_slot && !b.branch_likely &&
                        std::strcmp(b.mnemonic, "bc2t") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2t decode\\n");
    }
    {
        const auto b = decode(0x100000, R(0x12,8,3,0,0,0x02));
        const bool ok = b.valid == V && b.flow == e_ee_flow::FLOW_BRANCH_LIKELY &&
                        b.has_delay_slot && b.branch_likely &&
                        std::strcmp(b.mnemonic, "bc2tl") == 0;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL bc2tl decode\\n");
    }
    // The BC2 table has 32 entries, but only rt=0..3 are defined. Do not
    // silently alias higher rt values through rt & 3.
    expect("bc2 rt=4",  R(0x12,8,4,0,0,0x02), "cop2_reserved", X);
    expect("bc2 rt=31", R(0x12,8,31,0,0,0x02), "cop2_reserved", X);

    // Exhaustively check the published SPECIAL1/SPECIAL2 dispatch tables.
    // Reserved encodings remain INVALID. Only actually implemented
    // vector-length, matrix-FMAC and indexed-memory macros are VALID;
    // other recognized COP2 encodings must remain UNSUPPORTED.
    for (uint32_t funct = 0; funct < 64; ++funct)
    {
        const bool defined = funct <= 50 || funct == 52 || funct == 53 ||
                             funct == 56 || funct == 57 || funct >= 60;
        const auto ins = decode(0x100000, R(0x12,0x10,0,0,0,funct));
        const bool native = funct == 0x0bu || funct == 0x01u ||
            funct == 0x02u || funct == 0x20u || funct == 0x1cu ||
            funct == 0x2au;
        const auto want = !defined ? X : (native ? V : U);
        const bool ok = ins.valid == want;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL COP2 SPECIAL1 funct=%02x valid=%u want=%u\n",
                             funct, (unsigned)ins.valid, (unsigned)want);
    }
    for (uint32_t index = 0; index < 128; ++index)
    {
        const bool defined = index <= 42 || (index >= 44 && index <= 49) ||
                             (index >= 52 && index <= 67);
        const uint32_t funct = 0x3c + (index & 3);
        const uint32_t sa = index >> 2;
        const auto ins = decode(0x100000, R(0x12,0x10,0,0,sa,funct));
        const auto want = !defined ? X :
            (index == 0x09u || index == 0x0au || index == 0x18u ||
             index == 0x39u || index == 0x3au ||
              index == 0x3bu ||
             (index >= 0x34u && index <= 0x37u) ? V : U);
        const bool ok = ins.valid == want;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL COP2 SPECIAL2 index=%02x valid=%u want=%u\n",
                             index, (unsigned)ins.valid, (unsigned)want);
    }
    {
        const auto l = decode(0x100000, R(0x36,4,5,0,0,0));
        const auto st = decode(0x100000, R(0x3e,4,5,0,0,0));
        const bool ok = l.opcode == e_ee_opcode::OP_LQC2 &&
                        st.opcode == e_ee_opcode::OP_SQC2 &&
                        lift(l).kind == LOP_LQC2 && lift(st).kind == LOP_SQC2;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL lqc2/sqc2 primary opcode decode/lift\n");
    }
    {
        const auto m = decode(0x100000, R(0x12,1,4,6,0,0));
        const bool ok = lift(m).kind == LOP_MFC2;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL qmfc2 must lift to existing transfer helper\n");
    }

    // COP1/COP0 validity is an emitter contract, not a claim that every
    // reserved or merely decoded coprocessor operation has been lowered.
    expect("mfc1", R(0x11,0x00,4,5,0,0), "cop1_transfer", V);
    expect("mtc1", R(0x11,0x04,4,5,0,0), "cop1_transfer", V);
    expect("cfc1 fcsr", R(0x11,0x02,4,31,0,0), "cop1_fcsr", V);
    expect("ctc1 fcsr", R(0x11,0x06,4,31,0,0), "cop1_fcsr", V);
    expect("cfc1 other", R(0x11,0x02,4,0,0,0), "cop1_control", U);
    expect("add.s", R(0x11,0x10,5,6,7,0x00), "cop1_s", V);
    expect("rsqrt.s", R(0x11,0x10,5,6,7,0x16), "cop1_s", V);
    expect("madda.s", R(0x11,0x10,5,6,7,0x1e), "cop1_s", V);
    expect("max.s", R(0x11,0x10,5,6,7,0x28), "cop1_s", V);
    expect("min.s", R(0x11,0x10,5,6,7,0x29), "cop1_s", V);
    expect("c.f.s", R(0x11,0x10,5,6,0,0x30), "cop1_s", V);
    expect("c.eq.s", R(0x11,0x10,5,6,0,0x32), "cop1_s", V);
    expect("c.lt.s", R(0x11,0x10,5,6,0,0x34), "cop1_s", V);
    expect("c.le.s", R(0x11,0x10,5,6,0,0x36), "cop1_s", V);
    expect("c.eq.fcc1", R(0x11,0x10,5,6,1,0x32), "cop1_s", U);
    expect("c.lt.fcc1", R(0x11,0x10,5,6,1,0x34), "cop1_s", U);
    expect("cvt.w.s", R(0x11,0x10,5,6,7,0x24), "cop1_s", V);
    expect("unknown.s", R(0x11,0x10,5,6,7,0x08), "cop1_reserved", X);
    expect("unknown.s 0x1b", R(0x11,0x10,5,6,7,0x1b), "cop1_reserved", X);
    expect("cvt.s.w", R(0x11,0x14,0,6,7,0x20), "cvt_s_w", V);
    expect("bad.w", R(0x11,0x14,0,6,7,0x21), "cop1_reserved", X);
    expect("fmt.d", R(0x11,0x11,5,6,7,0x00), "cop1_reserved", X);
    expect("fmt.l", R(0x11,0x15,5,6,7,0x00), "cop1_reserved", X);
    expect("bc1 rt=4", R(0x11,0x08,4,0,0,0x00), "cop1_reserved", X);

    // Signed word register operations raise Ov. Immediate and doubleword
    // overflow encodings remain gated until their exception paths are modelled.
    expect("addi overflow", R(0x08,4,5,0,0,0), "addi", U);
    expect("daddi overflow", R(0x18,4,5,0,0,0), "daddi", U);
    expect("slti", R(0x0a,4,5,0,0,0), "slti", V);
    expect("sltiu", R(0x0b,4,5,0,0,0), "sltiu", V);
    expect("add", R(0x00,4,5,6,0,0x20), "signed_arith", V);
    expect("sub", R(0x00,4,5,6,0,0x22), "signed_arith", V);
    expect("sub reserved shift", R(0x00,4,5,6,1,0x22), "signed_arith", U);
    expect("dadd overflow", R(0x00,4,5,6,0,0x2c), "signed_arith", U);
    expect("slt", R(0x00,4,5,6,0,0x2a), "slt", V);
    expect("sltu", R(0x00,4,5,6,0,0x2b), "sltu", V);
    expect("tge", R(0x00,4,5,0,0,0x30), "trap", U);
    expect("bltzal", R(0x01,4,0x10,0,0,0), "bltzal", V);
    expect("bgezal", R(0x01,4,0x11,0,0,0), "bgezal", V);
    expect("bltzall", R(0x01,4,0x12,0,0,0), "bltzall", V);
    expect("bgezall", R(0x01,4,0x13,0,0,0), "bgezall", V);
    expect("bltzal rs=ra", R(0x01,31,0x10,0,0,0), "bltzal", U);
    expect("bltzall rs=ra", R(0x01,31,0x12,0,0,0), "bltzall", U);
    expect("tgei", R(0x01,4,0x08,0,0,0), "regimm_trap", U);
    expect("mtsab", R(0x01,4,0x18,0,0,0), "mtsab", V);
    expect("mtsah", R(0x01,4,0x19,0,0,0), "mtsah", V);
    expect("reserved regimm", R(0x01,4,0x1f,0,0,0), "regimm_reserved", X);
    {
        const auto jr_ra = decode(0x100000, R(0x00,31,0,0,0,0x08));
        const auto jr_v0 = decode(0x100000, R(0x00,2,0,0,0,0x08));
        const auto jalr = decode(0x100000, R(0x00,2,0,16,0,0x09));
        const auto jalr_bad_reserved = decode(0x100000, R(0x00,2,1,16,0,0x09));
        const bool ok = jr_ra.valid == V && jr_ra.flow == e_ee_flow::FLOW_RETURN &&
                        jr_v0.valid == V && jr_v0.flow == e_ee_flow::FLOW_JUMP &&
                        jalr.valid == V && jalr.flow == e_ee_flow::FLOW_CALL &&
                        jalr.rd == 16 && jalr_bad_reserved.valid == U;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL JR/JALR dynamic-flow decode or reserved fields\n");
    }

    expect("mfc0", R(0x10,0x00,4,12,0,0), "mfc0", V);
    expect("mtc0", R(0x10,0x04,4,12,0,0), "mtc0", V);
    expect("tlbwi", R(0x10,0x10,0,0,0,0x02), "tlb", U);
    expect("bad cop0 co", R(0x10,0x10,0,0,0,0x3f), "cop0_reserved", X);
    expect("bc0", R(0x10,0x08,0,0,0,0x00), "bc0", U);
    {
        const auto eret = decode(0x100000, R(0x10,0x10,0,0,0,0x18));
        const bool ok = eret.valid == V && eret.flow == e_ee_flow::FLOW_RETURN &&
                        !eret.has_delay_slot;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL eret no-delay flow\n");
    }
    {
        const auto syscall = decode(0x100000, R(0x00,0,0,0,0,0x0c));
        const auto brk = decode(0x100000, R(0x00,0,0,0,0,0x0d));
        const bool ok = syscall.valid == V && brk.valid == V &&
                        syscall.flow == e_ee_flow::FLOW_EXCEPTION &&
                        brk.flow == e_ee_flow::FLOW_EXCEPTION;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL syscall/break exception flow\n");
    }

    {   // the game's entry word is an exact PADDUW encoding (zero + zero -> zero)
        s_ee_instruction i = decode(0x100008, 0x70000c28);
        bool ok = std::strcmp(i.mnemonic, "padduw") == 0 && i.valid == V;
        ok ? ++g_pass : ++g_fail; if (!ok) std::printf("FAIL entry word\n");
    }

    {
        s_ee_instruction ins{};
        ins.valid = e_ee_valid::VALID; ins.pc = 0x1000;
        ins.opcode = e_ee_opcode::OP_J;
        ins.flow = e_ee_flow::FLOW_JUMP; ins.has_delay_slot = true;
        ins.jump_target = 0x2000;
        const std::set<uint32_t> expected{0x1004, 0x2000};
        const bool ok = collect_successors(ins) == expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL J delay-slot successor discovery\n");
    }
    {
        s_ee_instruction ins{};
        ins.valid = e_ee_valid::VALID; ins.pc = 0x1000;
        ins.opcode = e_ee_opcode::OP_JAL;
        ins.flow = e_ee_flow::FLOW_CALL; ins.has_delay_slot = true;
        ins.jump_target = 0x3000;
        const std::set<uint32_t> expected{0x1004, 0x1008, 0x3000};
        const bool ok = collect_successors(ins) == expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL JAL delay-slot/return-path discovery\n");
    }
    {
        s_ee_instruction ins{};
        ins.valid = e_ee_valid::VALID; ins.pc = 0x1000;
        ins.flow = e_ee_flow::FLOW_BRANCH_LIKELY; ins.has_delay_slot = true;
        ins.branch_target = 0x1800;
        const std::set<uint32_t> expected{0x1004, 0x1008, 0x1800};
        const bool ok = collect_successors(ins) == expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL branch-likely delay-slot successor discovery\\n");
    }
    {
        s_ee_instruction ins{};
        ins.valid = e_ee_valid::VALID; ins.pc = 0x1000;
        ins.flow = e_ee_flow::FLOW_RETURN; ins.has_delay_slot = true;
        const std::set<uint32_t> expected{0x1004};
        const bool ok = collect_successors(ins) == expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL JR return delay-slot discovery\\n");
    }
    {
        s_ee_instruction ins{};
        ins.valid = e_ee_valid::VALID; ins.pc = 0x1000;
        ins.flow = e_ee_flow::FLOW_RETURN; ins.has_delay_slot = false;
        const bool ok = collect_successors(ins).empty();
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL ERET must not invent a delay slot\\n");
    }
    {
        const auto syscall = decode(0x1000, R(0, 0, 0, 0, 0, 0x0c));
        const auto brk = decode(0x1008, R(0, 0, 0, 0, 0, 0x0d));
        const std::set<uint32_t> syscall_resume{0x1004};
        const std::set<uint32_t> break_resume{0x100c};
        const bool ok = collect_successors(syscall) == syscall_resume &&
                        collect_successors(brk) == break_resume;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL exception instructions must expose possible EPC+4 resume PCs\n");
    }
    {
        // Exception blocks still exit to the host at runtime, but their
        // post-exception continuation must be present for a handler that
        // returns at EPC+4.
        std::map<uint32_t, s_ee_instruction> insns;
        const auto syscall = decode(0x1000, R(0, 0, 0, 0, 0, 0x0c));
        const auto middle = decode(0x1004, (0x09u << 26) | (2u << 16) | 7u);
        const auto brk = decode(0x1008, R(0, 0, 0, 0, 0, 0x0d));
        const auto resume = decode(0x100c, (0x09u << 26) | (3u << 16) | 9u);
        insns.emplace(syscall.pc, syscall);
        insns.emplace(middle.pc, middle);
        insns.emplace(brk.pc, brk);
        insns.emplace(resume.pc, resume);

        function_analyzer analyzer;
        const auto functions = analyzer.analyze(insns, {0x1000});
        const bool ok = functions.size() == 1 &&
                        functions[0].blocks.size() == 3 &&
                        functions[0].blocks[0].start_pc == 0x1000 &&
                        functions[0].blocks[0].is_terminator &&
                        functions[0].blocks[1].start_pc == 0x1004 &&
                        functions[0].blocks[1].insn_indices.size() == 2 &&
                        functions[0].blocks[1].is_terminator &&
                        functions[0].blocks[2].start_pc == 0x100c;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL CFG did not retain code after resumable exceptions\n");
    }
    {
        const auto jal_cross = decode(0x0ffffffc, (0x03u << 26) | 1u);
        const auto j_cross = decode(0x0ffffffc, (0x02u << 26) | 1u);
        const bool ok = jal_cross.valid == V && j_cross.valid == V &&
                        jal_cross.jump_target == 0x10000004u &&
                        j_cross.jump_target == 0x10000004u;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL J/JAL target region must use PC+4 across 256MB boundary\n");
    }
    {
        const auto j_zero = decode(0x00000000, 0x08000000u);
        const auto jal_zero = decode(0x00000000, 0x0c000000u);
        const std::set<uint32_t> j_expected{0x00000000u, 0x00000004u};
        const std::set<uint32_t> jal_expected{0x00000000u, 0x00000004u, 0x00000008u};
        const bool ok = collect_successors(j_zero) == j_expected &&
                        collect_successors(jal_zero) == jal_expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL direct J/JAL target zero was mistaken for indirect transfer\n");
    }
    {
        const auto jr = decode(0x100000, R(0x00,2,0,0,0,0x08));
        const auto jalr = decode(0x100000, R(0x00,2,0,16,0,0x09));
        const std::set<uint32_t> jr_expected{0x100004};
        const std::set<uint32_t> jalr_expected{0x100004, 0x100008};
        const bool ok = collect_successors(jr) == jr_expected &&
                        collect_successors(jalr) == jalr_expected;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL indirect JR/JALR must not invent a static target\n");
    }
    {
        s_recompiled_elf compiled{};
        s_recompiled_fn caller{};
        caller.name = "caller";
        caller.vaddr = 0x00100000;
        caller.block_pcs = {0x00100000, 0x00100008, 0x00100020};
        s_recompiled_fn callee{};
        callee.name = "callee";
        callee.vaddr = 0x00102000;
        callee.block_pcs = {0x00102000, 0x00102018};
        compiled.functions.push_back(caller);
        compiled.functions.push_back(callee);

        const s_recompiled_fn* entry = find_recompiled_function(compiled, 0x00100000);
        const s_recompiled_fn* continuation =
            find_recompiled_function(compiled, 0x00100020);
        const s_recompiled_fn* callee_body =
            find_recompiled_function(compiled, 0x00102018);
        const s_recompiled_fn* unknown =
            find_recompiled_function(compiled, 0xDEAD0000);
        const bool ok = entry != nullptr && entry->name == "caller" &&
                        continuation == entry && callee_body != nullptr &&
                        callee_body->name == "callee" && unknown == nullptr;
        ok ? ++g_pass : ++g_fail;
        if (!ok) std::printf("FAIL translated block-PC ownership lookup\\n");
    }
    std::printf("decoder_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
