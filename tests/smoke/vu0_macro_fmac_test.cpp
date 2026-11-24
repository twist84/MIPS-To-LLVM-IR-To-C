#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
int g_failures = 0;

void check(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

uint32_t bits(float value)
{
    uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

ee::s_ee_u128 make_vector(float x, float y, float z, float w)
{
    return ee::s_ee_u128{
        static_cast<uint64_t>(bits(x)) | (static_cast<uint64_t>(bits(y)) << 32),
        static_cast<uint64_t>(bits(z)) | (static_cast<uint64_t>(bits(w)) << 32)
    };
}

uint32_t lane(const ee::s_ee_u128& value, uint32_t index)
{
    const uint64_t half = index < 2 ? value.m_lo : value.m_hi;
    return static_cast<uint32_t>(half >> (32 * (index & 1)));
}

uint32_t sync_halted(
    void*, ee::s_ee_cpu_state*, ee::s_ee_vu0_state* vu0,
    uint32_t mode, uint32_t
)
{
    return mode == static_cast<uint32_t>(ee::e_ee_vu0_sync_mode::FINISH_TO_E) &&
           vu0->m_running == 0 && (vu0->m_vpu_status & 1u) == 0
        ? static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE)
        : static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
}

constexpr uint32_t s_matrix_words[16] =
{
    0x4be821bcu, 0x4be828bdu, 0x4be830beu, 0x4be83a0bu,
    0x4be921bcu, 0x4be928bdu, 0x4be930beu, 0x4be93a4bu,
    0x4bea21bcu, 0x4bea28bdu, 0x4bea30beu, 0x4bea3a8bu,
    0x4beb21bcu, 0x4beb28bdu, 0x4beb30beu, 0x4beb3acbu,
};

bool same_vector(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    return a.m_lo == b.m_lo && a.m_hi == b.m_hi;
}

// COP2 macro encodings (VU User's Manual chapter 5). dest: x=8 y=4 z=2 w=1.
constexpr uint32_t special1(uint32_t dest, uint32_t ft, uint32_t fs,
                            uint32_t fd, uint32_t funct)
{
    return 0x4a000000u | (dest << 21) | (ft << 16) | (fs << 11) |
        (fd << 6) | funct;
}

constexpr uint32_t special2(uint32_t dest, uint32_t ft, uint32_t fs,
                            uint32_t op)
{
    return 0x4a000000u | (dest << 21) | (ft << 16) | (fs << 11) |
        ((op >> 2) << 6) | 0x3cu | (op & 3u);
}

uint32_t run_macro(ee::s_ee_cpu_state& cpu, ee::s_ee_runtime& runtime,
                   uint32_t word)
{
    return ee::ee_vu0_macro_fmac(&cpu, &runtime, word, 0x00107d5cu);
}

void check_general_macros(ee::s_ee_cpu_state& cpu, ee::s_ee_runtime& runtime,
                          ee::s_ee_vu0_state& vu0)
{
    // Retail SDBZ 0x00107d5c: VMULx.x vf7, vf6, vf6x (first word that
    // stopped the 2026-11-23 replay before general macro support).
    static_assert(special1(8u, 6u, 6u, 7u, 0x18u) == 0x4b0631d8u,
                  "VMULx.x encoding");
    cpu.m_vu0[6] = make_vector(3.f, 1.f, 1.f, 1.f);
    cpu.m_vu0[7] = make_vector(0.f, 8.f, 7.f, 6.f);
    check(run_macro(cpu, runtime, 0x4b0631d8u) == 0 &&
          same_vector(cpu.m_vu0[7], make_vector(9.f, 8.f, 7.f, 6.f)),
          "retail VMULx.x squares only the X lane");

    cpu.m_vu0[1] = make_vector(1.f, 2.f, 3.f, 4.f);
    check(run_macro(cpu, runtime, special2(15u, 2u, 1u, 0x31u)) == 0 &&
          same_vector(cpu.m_vu0[2], make_vector(2.f, 3.f, 4.f, 1.f)),
          "VMR32 rotates fs into ft");

    cpu.m_vu0[1] = make_vector(1.5f, -2.25f, 0.f, 1e20f);
    check(run_macro(cpu, runtime, special2(15u, 2u, 1u, 0x15u)) == 0 &&
          lane(cpu.m_vu0[2], 0) == 24u &&
          lane(cpu.m_vu0[2], 1) == static_cast<uint32_t>(-36) &&
          lane(cpu.m_vu0[2], 2) == 0u &&
          lane(cpu.m_vu0[2], 3) == 0x7fffffffu,
          "VFTOI4 scales, truncates and saturates");
    check(run_macro(cpu, runtime, special2(8u, 3u, 2u, 0x11u)) == 0 &&
          lane(cpu.m_vu0[3], 0) == bits(1.5f),
          "VITOF4 restores the fixed-point value");

    cpu.m_vu0[1] = make_vector(1.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[2] = make_vector(0.f, 1.f, 0.f, 0.f);
    check(run_macro(cpu, runtime, special2(14u, 2u, 1u, 0x2eu)) == 0 &&
          run_macro(cpu, runtime, special1(14u, 1u, 2u, 3u, 0x2eu)) == 0 &&
          lane(cpu.m_vu0[3], 0) == bits(0.f) &&
          lane(cpu.m_vu0[3], 1) == bits(0.f) &&
          lane(cpu.m_vu0[3], 2) == bits(1.f),
          "VOPMULA/VOPMSUB compute the cross product");

    cpu.m_vu0[1] = make_vector(6.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[2] = make_vector(0.f, 2.f, 0.f, 0.f);
    // VDIV Q, vf1.x, vf2.y: fsf bits 22:21 = 0, ftf bits 24:23 = 1.
    const uint32_t vdiv = special2(0u, 2u, 1u, 0x38u) | (1u << 23);
    check(run_macro(cpu, runtime, vdiv) == 0 && vu0.m_q_pending == 1u,
          "VDIV queues Q");
    check(ee::ee_vu0_macro_scalar(&cpu, &runtime,
              special2(0u, 0u, 0u, 0x3bu), 0x00107d60u) == 0 &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == bits(3.f),
          "VWAITQ retires VDIV quotient");

    check(run_macro(cpu, runtime, special1(0u, 1u, 0u, 0x1du, 0x32u)) == 0 &&
          vu0.m_vi[1] == 0xfffdu &&
          run_macro(cpu, runtime, special1(0u, 1u, 1u, 2u, 0x30u)) == 0 &&
          vu0.m_vi[2] == 0xfffau,
          "VIADDI sign-extends imm5 and VIADD wraps to 16 bits");
    check(run_macro(cpu, runtime, special2(8u, 5u, 2u, 0x3du)) == 0 &&
          lane(cpu.m_vu0[5], 0) == 0xfffffffau &&
          run_macro(cpu, runtime, special2(0u, 3u, 5u, 0x3cu)) == 0 &&
          vu0.m_vi[3] == 0xfffau,
          "VMFIR sign-extends and VMTIR truncates");

    vu0.m_clip_flags = 0u;
    cpu.m_vu0[1] = make_vector(5.f, -5.f, 0.5f, 0.f);
    cpu.m_vu0[2] = make_vector(0.f, 0.f, 0.f, -1.f);
    check(run_macro(cpu, runtime, special2(14u, 2u, 1u, 0x1fu)) == 0 &&
          vu0.m_clip_flags == 0x9u,
          "VCLIPw judges x>+|w| and y<-|w|");

    vu0.m_vi[21] = bits(2.f);
    cpu.m_vu0[1] = make_vector(1.f, 3.f, 0.f, 0.f);
    const uint32_t mac_before_max = vu0.m_mac_flags;
    check(run_macro(cpu, runtime, special1(12u, 0u, 1u, 4u, 0x1du)) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(2.f) &&
          lane(cpu.m_vu0[4], 1) == bits(3.f) &&
          vu0.m_mac_flags == mac_before_max,
          "VMAXi selects per lane without touching MAC flags");

    cpu.m_vu0[1] = make_vector(0.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[1].m_lo = 1u;
    check(run_macro(cpu, runtime, special2(0u, 0u, 1u, 0x42u)) == 0 &&
          vu0.m_vi[20] == 0x3f800001u &&
          run_macro(cpu, runtime, special2(8u, 6u, 0u, 0x40u)) == 0 &&
          lane(cpu.m_vu0[6], 0) == 0x3f800002u,
          "VRINIT seeds R and VRNEXT advances the LFSR");

    check(run_macro(cpu, runtime, special1(0u, 4u, 0u, 2u, 0x32u)) == 0 &&
          run_macro(cpu, runtime, special1(0u, 5u, 0u, 9u, 0x32u)) == 0 &&
          run_macro(cpu, runtime, special2(4u, 5u, 4u, 0x3fu)) == 0 &&
          vu0.m_data_memory[0x24] == 9u &&
          run_macro(cpu, runtime, special2(4u, 6u, 4u, 0x3eu)) == 0 &&
          vu0.m_vi[6] == 9u,
          "VISWR/VILWR use VI*16 plus the selected lane");
}
} // namespace

int main()
{
    using ee::e_ee_vu0_sync_result;

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::s_ee_runtime runtime{};
    runtime.m_vu0_state = &vu0;
    runtime.m_vu0_sync = sync_halted;

    // Original US game sequence, executing a four-column matrix transform.
    // The columns of an identity matrix must preserve every source vector.
    cpu.m_vu0[4] = make_vector(1.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[5] = make_vector(0.f, 1.f, 0.f, 0.f);
    cpu.m_vu0[6] = make_vector(0.f, 0.f, 1.f, 0.f);
    cpu.m_vu0[7] = make_vector(0.f, 0.f, 0.f, 1.f);
    cpu.m_vu0[8] = make_vector(2.f, 3.f, 4.f, 1.f);
    cpu.m_vu0[9] = make_vector(-1.f, 5.f, 7.f, 1.f);
    cpu.m_vu0[10] = make_vector(10.f, -8.f, 6.f, 1.f);
    cpu.m_vu0[11] = make_vector(1.f, 2.f, 3.f, 1.f);
    const ee::s_ee_u128 expected[4] = {
        cpu.m_vu0[8], cpu.m_vu0[9], cpu.m_vu0[10], cpu.m_vu0[11]
    };
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t result = ee::ee_vu0_macro_fmac(
            &cpu, &runtime, s_matrix_words[i], 0x00108240u + 4u * i
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "one authentic SDBZ macro executes and synchronizes halted VU0");
        if ((i & 3u) == 3u)
        {
            check(same_vector(cpu.m_vu0[8u + (i / 4u)], expected[i / 4u]),
                  "original SDBZ matrix chain preserves identity transform");
        }
    }

    // VMULAx initializes ACC, VMADDAy/z read it, and VMADDw writes VF
    // without overwriting ACC. Check the destination and negative MAC sign.
    cpu.m_vu0[4] = make_vector(-2.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[8] = make_vector(3.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) == 0 &&
          lane(vu0.m_acc, 0) == bits(-6.f) &&
          (vu0.m_mac_flags & 0x80u) != 0,
          "VMULAx initializes ACC and records signed X MAC flag");

    // Masked-off lanes retain their ACC or VF payload, but clear MAC lanes.
    vu0.m_acc = make_vector(9.f, 8.f, 7.f, 6.f);
    vu0.m_mac_flags = 0xffffu;
    const uint32_t x_only = (s_matrix_words[0] & ~(15u << 21)) | (8u << 21);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          lane(vu0.m_acc, 0) == bits(-6.f) &&
          lane(vu0.m_acc, 1) == bits(8.f) &&
          lane(vu0.m_acc, 2) == bits(7.f) &&
          lane(vu0.m_acc, 3) == bits(6.f) &&
          (vu0.m_mac_flags & 0x7777u) == 0,
          "lane mask preserves unmapped ACC lanes and clears their MAC flags");

    // Manual section 3.3.2: sticky Z/S/U/O are the OR accumulation
    // of current flags, never merely the previous instruction's flags.
    vu0.m_status_flags = 0u;
    cpu.m_vu0[4] = make_vector(-2.f, 0.f, 0.f, 0.f);
    cpu.m_vu0[8] = make_vector(3.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0082u) == 0x0082u,
          "negative product sets current S and sticky SS");
    cpu.m_vu0[4] = make_vector(2.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0082u) == 0x0080u,
          "positive product clears current S but preserves sticky SS");
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0080u) != 0u,
          "sticky SS persists across multiple later normal FMACs");

    // Existing sticky I/D and OS cannot be erased by ordinary FMAC
    // results, including with masked-off lanes. Current flags remain
    // consistent with active lanes rather than the old MAC register.
    vu0.m_status_flags = 0x0e30u;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0e30u) == 0x0e30u &&
          (vu0.m_status_flags & 0x000fu) == 0u,
          "sticky OS/IS/DS and current I/D preserved across normal FMAC");

    // The result can generate a new sticky overflow, which survives a
    // subsequent operation even after the current overflow bit clears.
    vu0.m_status_flags = 0u;
    cpu.m_vu0[4] = make_vector(3.0e38f, 0.f, 0.f, 0.f);
    cpu.m_vu0[8] = make_vector(2.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0208u) == 0x0208u,
          "FMAC overflow sets current O and sticky OS");
    cpu.m_vu0[4] = make_vector(1.f, 0.f, 0.f, 0.f);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, x_only, 0x108240) == 0 &&
          (vu0.m_status_flags & 0x0208u) == 0x0200u,
          "ordinary FMAC clears current O but preserves sticky OS");

    // A VF0 destination discards writes but still updates flags. The
    // physical backing of VF0 must be ignored for reads and remain unchanged.
    cpu.m_vu0[0] = make_vector(21.f, 22.f, 23.f, 24.f);
    const ee::s_ee_u128 vf0_before = cpu.m_vu0[0];
    const uint32_t to_vf0 = s_matrix_words[3] & ~(31u << 6);
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, to_vf0, 0x10824c) == 0 &&
          same_vector(cpu.m_vu0[0], vf0_before),
          "VMADDw discards VF0 destination without corrupting backing");

    // Retail SDBZ vector-length function starting at EE 0x001069ec.
    // The exact original macro words are VMUL.xyz, VADDy.x, VADDz.x,
    // VSQRT Q, VF4.x, VWAITQ and VADDq.x VF4, VF0, Q.
    constexpr uint32_t k_vmul_xyz = 0x4bc4212au;
    constexpr uint32_t k_vaddy_x = 0x4b042101u;
    constexpr uint32_t k_vaddz_x = 0x4b042102u;
    constexpr uint32_t k_vsqrt = 0x4a0403bdu;
    constexpr uint32_t k_vwaitq = 0x4a0003bfu;
    constexpr uint32_t k_vaddq_x = 0x4b000120u;
    cpu.m_vu0[4] = make_vector(3.f, 4.f, 0.f, 7.f);
    vu0.m_mac_flags = 0u;
    vu0.m_status_flags = 0u;
    vu0.m_acc = make_vector(17.f, 18.f, 19.f, 20.f);
    vu0.m_q = {};
    vu0.m_cycle = 12u;
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vmul_xyz, 0x001069ecu) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(9.f) &&
          lane(cpu.m_vu0[4], 1) == bits(16.f) &&
          lane(cpu.m_vu0[4], 2) == bits(0.f) &&
          lane(cpu.m_vu0[4], 3) == bits(7.f) &&
          lane(vu0.m_acc, 0) == bits(17.f),
          "retail VMUL.xyz updates only masked VF lanes, never ACC");
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddy_x, 0x001069f0u) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(25.f),
          "retail VADDy.x broadcasts the original VF4.y");
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddz_x, 0x001069f4u) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(25.f),
          "retail VADDz.x adds the zero Z component");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vsqrt, 0x001069f8u) == 0 &&
          vu0.m_q_pending == 1u &&
          vu0.m_q_ready_cycle == 19u &&
          vu0.m_q_pending_bits == bits(5.f) &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == 0u,
          "VSQRT starts seven-cycle pending Q without early result");
    const ee::s_ee_u128 vf_before_wait = cpu.m_vu0[4];
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddq_x, 0x00106a00u) ==
                  static_cast<uint32_t>(
                      ee::e_ee_vu0_sync_result::UNAVAILABLE) &&
          same_vector(cpu.m_vu0[4], vf_before_wait),
          "VADDq must not consume a still-pending Q");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x001069fcu) == 0 &&
          vu0.m_q_pending == 0u &&
          vu0.m_cycle == 19u &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == bits(5.f) &&
          vu0.m_vi[22] == bits(5.f),
          "VWAITQ retires original SQRT at seven VU0 virtual cycles");
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddq_x, 0x00106a00u) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(5.f) &&
          lane(cpu.m_vu0[4], 1) == bits(16.f) &&
          lane(cpu.m_vu0[4], 3) == bits(7.f) &&
          lane(vu0.m_acc, 0) == bits(17.f),
          "retail VADDq.x returns vector length without touching ACC");

    // VSQRT uses the absolute value, sets current/sticky invalid for a
    // negative scalar, and Q remains pending until a real WAITQ.
    cpu.m_vu0[4] = make_vector(-9.f, 0.f, 0.f, 0.f);
    vu0.m_status_flags = 0u;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vsqrt, 0x001069f8u) == 0 &&
          vu0.m_q_pending == 1u &&
          (vu0.m_status_flags & 0x410u) == 0x410u &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == bits(5.f),
          "negative VSQRT sets invalid but does not expose pending Q");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x001069fcu) == 0 &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == bits(3.f),
          "negative VSQRT retires sqrt of absolute scalar");

    // Unimplemented scalar SPECIAL2 (VDIV), unavailable micro-sync and
    // overlapping SQRT may not corrupt the VU architectural state.
    const ee::s_ee_vu0_state before_scalar_invalid = vu0;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vsqrt ^ 1u, 0x001069f8u) ==
                  static_cast<uint32_t>(
                      ee::e_ee_vu0_sync_result::INVALID_ARGUMENT) &&
          std::memcmp(&vu0, &before_scalar_invalid, sizeof(vu0)) == 0,
          "unimplemented scalar SPECIAL2 fails without writing state");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vsqrt, 0x001069f8u) == 0,
          "first VSQRT accepted");
    const ee::s_ee_vu0_state before_overlap = vu0;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vsqrt, 0x001069f8u) ==
                  static_cast<uint32_t>(
                      ee::e_ee_vu0_sync_result::UNAVAILABLE) &&
          std::memcmp(&vu0, &before_overlap, sizeof(vu0)) == 0,
          "overlapping Q producer fails closed");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x001069fcu) == 0,
          "Q drain after failed overlapping producer");

    // Actual EE 0x0010697c..0x00106990 vector-normalization path.
    // The 16:28 run passed VSQRT, then stopped at this VRSQRT.
    constexpr uint32_t k_vmul_to_vf5 = 0x4bc4216au;
    constexpr uint32_t k_vaddy_vf5 = 0x4b052941u;
    constexpr uint32_t k_vaddz_vf5 = 0x4b052942u;
    constexpr uint32_t k_vrsqrt = 0x4a6503beu;
    constexpr uint32_t k_vmulq_xyz = 0x4bc0211cu;
    cpu.m_vu0[4] = make_vector(3.f, 4.f, 0.f, 7.f);
    cpu.m_vu0[5] = make_vector(0.f, 0.f, 0.f, 27.f);
    // Original guest reads VF0.w=1, not the writable backing storage.
    cpu.m_vu0[0] = make_vector(8.f, 9.f, 10.f, 13.f);
    vu0.m_q = {};
    vu0.m_vi[22] = 0u;
    vu0.m_cycle = 30u;
    vu0.m_status_flags = 0u;
    vu0.m_acc = make_vector(10.f, 11.f, 12.f, 13.f);
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vmul_to_vf5, 0x0010697cu) == 0 &&
          lane(cpu.m_vu0[5], 0) == bits(9.f) &&
          lane(cpu.m_vu0[5], 1) == bits(16.f) &&
          lane(cpu.m_vu0[5], 2) == bits(0.f) &&
          lane(cpu.m_vu0[5], 3) == bits(27.f),
          "VMUL.xyz produces the original squared VF5 components");
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddy_vf5, 0x00106980u) == 0 &&
          ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vaddz_vf5, 0x00106984u) == 0 &&
          lane(cpu.m_vu0[5], 0) == bits(25.f),
          "VADDy/VADDz computes squared length 25");
    // The preceding VMUL.xyz produces a zero Z lane, which sets
    // sticky SZ. Scalar RSQRT must preserve that unrelated flag;
    // it only clears/updates current I/D on this successful input.
    const uint32_t status_before_rsqrt = vu0.m_status_flags;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vrsqrt, 0x00106988u) == 0 &&
          vu0.m_q_pending == 1u &&
          vu0.m_q_ready_cycle == 43u &&
          vu0.m_q_pending_bits == bits(1.f / 5.f) &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == 0u &&
          vu0.m_status_flags == (status_before_rsqrt & ~0x30u),
          "VRSQRT queues Q at 13 cycles without erasing FMAC sticky flags");
    const ee::s_ee_cpu_state early_cpu = cpu;
    const ee::s_ee_vu0_state early_vu0 = vu0;
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vmulq_xyz, 0x00106990u) ==
                  static_cast<uint32_t>(
                      ee::e_ee_vu0_sync_result::UNAVAILABLE) &&
          std::memcmp(&cpu, &early_cpu, sizeof(cpu)) == 0 &&
          std::memcmp(&vu0, &early_vu0, sizeof(vu0)) == 0,
          "VMULq cannot read Q before VWAITQ retires the producer");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x0010698cu) == 0 &&
          vu0.m_q_pending == 0u && vu0.m_cycle == 43u &&
          static_cast<uint32_t>(vu0.m_q.m_lo) == bits(1.f / 5.f) &&
          vu0.m_vi[22] == bits(1.f / 5.f),
          "VWAITQ retires genuine reciprocal SQRT at 13 virtual cycles");
    const ee::s_ee_u128 old_acc = vu0.m_acc;
    check(ee::ee_vu0_macro_fmac(
              &cpu, &runtime, k_vmulq_xyz, 0x00106990u) == 0 &&
          lane(cpu.m_vu0[4], 0) == bits(3.f * (1.f / 5.f)) &&
          lane(cpu.m_vu0[4], 1) == bits(4.f * (1.f / 5.f)) &&
          lane(cpu.m_vu0[4], 2) == bits(0.f) &&
          lane(cpu.m_vu0[4], 3) == bits(7.f) &&
          same_vector(vu0.m_acc, old_acc),
          "VMULq.xyz normalizes (3,4,0), preserving VF4.w and ACC");

    // Real VU0 RSQRT edge semantics, confirmed against PCSX2 VUops:
    // a negative denominator raises current/sticky I; zero denominator
    // raises D/DS, with signed saturation and 0/0 producing signed zero.
    cpu.m_vu0[5] = make_vector(-25.f, 0.f, 0.f, 0.f);
    vu0.m_status_flags = 0u;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vrsqrt, 0x00106988u) == 0 &&
          vu0.m_q_pending_bits == bits(1.f / 5.f) &&
          (vu0.m_status_flags & 0x410u) == 0x410u,
          "negative VRSQRT denominator takes abs and sets I/IS");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x0010698cu) == 0,
          "negative-denominator VRSQRT Q retires");

    cpu.m_vu0[5] = make_vector(0.f, 0.f, 0.f, 0.f);
    vu0.m_status_flags = 0u;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vrsqrt, 0x00106988u) == 0 &&
          vu0.m_q_pending_bits == 0x7f7fffffu &&
          (vu0.m_status_flags & 0x820u) == 0x820u,
          "positive zero denominator saturates +max and sets D/DS");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x0010698cu) == 0,
          "positive-zero Q retires");

    cpu.m_vu0[5] = make_vector(-0.f, 0.f, 0.f, 0.f);
    vu0.m_status_flags = 0u;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vrsqrt, 0x00106988u) == 0 &&
          vu0.m_q_pending_bits == 0xff7fffffu &&
          (vu0.m_status_flags & 0x820u) == 0x820u,
          "negative zero denominator saturates -max with D/DS");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x0010698cu) == 0,
          "negative-zero Q retires");

    // Force VF0.x=0 instead of VF0.w=1, with the same negative-zero
    // denominator, to check sign XOR and both I/D sticky flags.
    const uint32_t zero_numerator = k_vrsqrt & ~(3u << 21u);
    cpu.m_vu0[5] = make_vector(-0.f, 0.f, 0.f, 0.f);
    vu0.m_status_flags = 0u;
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, zero_numerator, 0x00106988u) == 0 &&
          vu0.m_q_pending_bits == 0x80000000u &&
          (vu0.m_status_flags & 0xc30u) == 0xc30u,
          "zero over negative zero yields -0 with I/D and sticky IS/DS");
    check(ee::ee_vu0_macro_scalar(
              &cpu, &runtime, k_vwaitq, 0x0010698cu) == 0,
          "zero-over-zero Q retires");

    check_general_macros(cpu, runtime, vu0);

    // Unsupported instructions or a busy VU0 must fail closed. Confirm no
    // architectural writes, not merely a non-zero return value.
    const ee::s_ee_cpu_state cpu_before = cpu;
    const ee::s_ee_vu0_state vu0_before = vu0;
    // SPECIAL1 funct 0x3a is reserved (VCALLMS is 0x38, VCALLMSR 0x39).
    const uint32_t unsupported = (s_matrix_words[0] & ~63u) | 0x3au;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, unsupported, 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT) &&
          std::memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0 &&
          std::memcmp(&vu0, &vu0_before, sizeof(vu0)) == 0,
          "reserved COP2 macro operation leaves CPU and VU0 unmodified");
    vu0.m_running = 1;
    const ee::s_ee_cpu_state busy_before = cpu;
    const ee::s_ee_vu0_state busy_vu0 = vu0;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
          std::memcmp(&cpu, &busy_before, sizeof(cpu)) == 0 &&
          std::memcmp(&vu0, &busy_vu0, sizeof(vu0)) == 0,
          "running VU0 without a finishing microengine cannot be bypassed");
    vu0.m_running = 0;
    runtime.m_vu0_sync = nullptr;
    check(ee::ee_vu0_macro_fmac(&cpu, &runtime, s_matrix_words[0], 0x108240) ==
              static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
          "missing VU0 synchronization callback cannot succeed");

    std::printf("vu0_macro_fmac_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
