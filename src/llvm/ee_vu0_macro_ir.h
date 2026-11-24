#pragma once
#include "ee_runtime.h"
#include <cstddef>
#include <string>

namespace llvm
{
struct s_vu0_ir_op
{
    uint32_t m_kind = 0;
    uint32_t m_source = 0;
    uint32_t m_broadcast = 0;
    bool m_acc = false;
};

inline bool decode_vu0_ir_fmac(uint32_t word, s_vu0_ir_op& result)
{
    const uint32_t funct = word & 63u;
    const bool special = funct >= 0x3cu;
    const uint32_t code = special ? (word & 3u) | ((word >> 4u) & 0x7cu) : funct;
    result.m_acc = special;
    result.m_broadcast = code & 3u;
    if (code < 0x10u || (code >= 0x18u && code < 0x1cu) ||
        (!special && code >= 0x10u && code < 0x18u))
    {
        constexpr uint32_t kinds[] = {0, 1, 3, 4, 5, 6, 2};
        result.m_kind = kinds[code >> 2u];
        result.m_source = 1;
        return true;
    }
    switch (code)
    {
    case 0x1c: result.m_kind = 2; result.m_source = 2; break;
    case 0x1d: if (special) { return false; } result.m_kind = 5; result.m_source = 3; break;
    case 0x1e: result.m_kind = 2; result.m_source = 3; break;
    case 0x1f: if (special) { return false; } result.m_kind = 6; result.m_source = 3; break;
    case 0x20: result.m_kind = 0; result.m_source = 2; break;
    case 0x21: result.m_kind = 3; result.m_source = 2; break;
    case 0x22: result.m_kind = 0; result.m_source = 3; break;
    case 0x23: result.m_kind = 3; result.m_source = 3; break;
    case 0x24: result.m_kind = 1; result.m_source = 2; break;
    case 0x25: result.m_kind = 4; result.m_source = 2; break;
    case 0x26: result.m_kind = 1; result.m_source = 3; break;
    case 0x27: result.m_kind = 4; result.m_source = 3; break;
    case 0x28: result.m_kind = 0; break;
    case 0x29: result.m_kind = 3; break;
    case 0x2a: result.m_kind = 2; break;
    case 0x2b: if (special) { return false; } result.m_kind = 5; break;
    case 0x2c: result.m_kind = 1; break;
    case 0x2d: result.m_kind = 4; break;
    case 0x2e: result.m_kind = special ? 2u : 4u; result.m_source = 4; break;
    case 0x2f: if (special) { return false; } result.m_kind = 6; break;
    default: return false;
    }
    return true;
}

inline std::string make_vu0_macro_ir()
{
    std::string ir = R"IR(
; All guest arithmetic and flag writeback is in IR. The only host
; call is the installed VU0 synchronization callback.
define internal ptr @ee_vu0_ir_sync(ptr %cpu, ptr %rt, i32 %pc) {
entry:
  %null = icmp eq ptr %rt, null
  br i1 %null, label %fail, label %fields
fields:
  %state_ptr = getelementptr i8, ptr %rt, i64 OFF_RT_STATE
  %cb_ptr = getelementptr i8, ptr %rt, i64 OFF_RT_SYNC
  %vu = load ptr, ptr %state_ptr
  %cb = load ptr, ptr %cb_ptr
  %no_state = icmp eq ptr %vu, null
  %no_cb = icmp eq ptr %cb, null
  %missing = or i1 %no_state, %no_cb
  br i1 %missing, label %fail, label %sync
sync:
  %user_ptr = getelementptr i8, ptr %rt, i64 OFF_RT_USER
  %user = load ptr, ptr %user_ptr
  %status = call i32 %cb(ptr %user, ptr %cpu, ptr %vu, i32 1, i32 %pc)
  %ok = icmp eq i32 %status, 0
  %result = select i1 %ok, ptr %vu, ptr null
  ret ptr %result
fail:
  ret ptr null
}

define internal <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %index) {
entry:
  %is_zero = icmp eq i32 %index, 0
  br i1 %is_zero, label %vf0, label %read
vf0:
  ret <4 x i32> <i32 0, i32 0, i32 0, i32 1065353216>
read:
  %base = getelementptr i8, ptr %cpu, i64 OFF_CPU_VF
  %address = getelementptr <4 x i32>, ptr %base, i32 %index
  %bits = load <4 x i32>, ptr %address, align 4
  ret <4 x i32> %bits
}

define internal i32 @ee_vu0_ir_order(i32 %x) alwaysinline {
  %negative = icmp slt i32 %x, 0
  %neg = xor i32 %x, -1
  %pos = xor i32 %x, -2147483648
  %key = select i1 %negative, i32 %neg, i32 %pos
  ret i32 %key
}

; kind: ADD SUB MUL MADD MSUB MAX MINI = 0..6
; source: vector broadcast Q I outer = 0..4
define internal i32 @ee_vu0_ir_fmac(ptr %cpu, ptr %rt, i32 %pc, i32 %fs, i32 %ft, i32 %fd, i32 %mask, i32 %kind, i32 %source, i32 %bc, i1 %to_acc) {
entry:
  %vu = call ptr @ee_vu0_ir_sync(ptr %cpu, ptr %rt, i32 %pc)
  %missing = icmp eq ptr %vu, null
  br i1 %missing, label %fail, label %ready
ready:
  %pending_ptr = getelementptr i8, ptr %vu, i64 OFF_PENDING
  %pending = load i32, ptr %pending_ptr
  %is_q = icmp eq i32 %source, 2
  %busy = icmp ne i32 %pending, 0
  %blocked = and i1 %is_q, %busy
  br i1 %blocked, label %fail, label %prepare
prepare:
  %a = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %fs)
  %b = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %ft)
  %acc_ptr = getelementptr i8, ptr %vu, i64 OFF_ACC
  %acc = load <4 x i32>, ptr %acc_ptr, align 4
  %fd_old = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %fd)
  %dest_old = select i1 %to_acc, <4 x i32> %acc, <4 x i32> %fd_old
  %q_ptr = getelementptr i8, ptr %vu, i64 OFF_Q
  %q = load i32, ptr %q_ptr
  %i_ptr = getelementptr i8, ptr %vu, i64 OFF_I
  %i = load i32, ptr %i_ptr
  %scalar = select i1 %is_q, i32 %q, i32 %i
  %is_i = icmp eq i32 %source, 3
  %use_scalar = or i1 %is_q, %is_i
  %is_bc = icmp eq i32 %source, 1
  %outer = icmp eq i32 %source, 4
  %flags = icmp ult i32 %kind, 5
  %max = icmp eq i32 %kind, 5
  %mac_ptr = getelementptr i8, ptr %vu, i64 OFF_MAC
  %mac_old = load i32, ptr %mac_ptr
  %latch_ptr = getelementptr i8, ptr %vu, i64 OFF_LATCH
  %latch_old = load i32, ptr %latch_ptr
  br label %lane
lane:
  %n = phi i32 [0, %prepare], [%next, %merge]
  %out = phi <4 x i32> [%dest_old, %prepare], [%out_next, %merge]
  %mac = phi i32 [%mac_old, %prepare], [%mac_next, %merge]
  %latch = phi i32 [%latch_old, %prepare], [%latch_next, %merge]
  %sticky = phi i32 [0, %prepare], [%sticky_next, %merge]
  %shift = sub i32 3, %n
  %bit = shl i32 1, %shift
  %groups = shl i32 4369, %shift
  %keep = xor i32 %groups, -1
  %cleared = and i32 %mac, %keep
  %selected = and i32 %mask, %bit
  %active = icmp ne i32 %selected, 0
  %masked_mac = select i1 %flags, i32 %cleared, i32 %mac
  br i1 %active, label %check_outer, label %merge
check_outer:
  %w = icmp eq i32 %n, 3
  %skip = and i1 %outer, %w
  br i1 %skip, label %merge, label %calculate
calculate:
  %n1 = add i32 %n, 1
  %n2 = add i32 %n, 2
  %cross_a = urem i32 %n1, 3
  %cross_b = urem i32 %n2, 3
  %a_lane = select i1 %outer, i32 %cross_a, i32 %n
  %bc_lane = select i1 %is_bc, i32 %bc, i32 %n
  %b_lane = select i1 %outer, i32 %cross_b, i32 %bc_lane
  %av = extractelement <4 x i32> %a, i32 %a_lane
  %bv = extractelement <4 x i32> %b, i32 %b_lane
  %rhs = select i1 %use_scalar, i32 %scalar, i32 %bv
  br i1 %flags, label %arithmetic, label %minmax
minmax:
  %ak = call i32 @ee_vu0_ir_order(i32 %av)
  %bk = call i32 @ee_vu0_ir_order(i32 %rhs)
  %ge = icmp uge i32 %ak, %bk
  %le = icmp ule i32 %ak, %bk
  %pick = select i1 %max, i1 %ge, i1 %le
  %extreme = select i1 %pick, i32 %av, i32 %rhs
  %extreme_out = insertelement <4 x i32> %out, i32 %extreme, i32 %n
  br label %merge
arithmetic:
  %accv = extractelement <4 x i32> %acc, i32 %n
  %latch_bit = and i32 %latch_old, %bit
  %latched = icmp ne i32 %latch_bit, 0
  %tuple = call { i32, i1, i1, i32 } @vu_fmac(i32 %kind, i32 %accv, i32 %av, i32 %rhs, i1 %latched)
  %bits = extractvalue { i32, i1, i1, i32 } %tuple, 0
  %overflow = extractvalue { i32, i1, i1, i32 } %tuple, 1
  %underflow = extractvalue { i32, i1, i1, i32 } %tuple, 2
  %product_sticky = extractvalue { i32, i1, i1, i32 } %tuple, 3
  %exp = and i32 %bits, 2139095040
  %zero = icmp eq i32 %exp, 0
  %sign = icmp slt i32 %bits, 0
  %z = select i1 %zero, i32 1, i32 0
  %s = select i1 %sign, i32 16, i32 0
  %u = select i1 %underflow, i32 256, i32 0
  %o = select i1 %overflow, i32 4096, i32 0
  %zs = or i32 %z, %s
  %uo = or i32 %u, %o
  %all = or i32 %zs, %uo
  %lane_flags = shl i32 %all, %shift
  %new_mac = or i32 %cleared, %lane_flags
  %not_bit = xor i32 %bit, -1
  %latch_clear = and i32 %latch, %not_bit
  %latch_set = select i1 %overflow, i32 %bit, i32 0
  %updated_latch = or i32 %latch_clear, %latch_set
  %new_latch = select i1 %to_acc, i32 %updated_latch, i32 %latch
  %new_sticky = or i32 %sticky, %product_sticky
  %new_out = insertelement <4 x i32> %out, i32 %bits, i32 %n
  br label %merge
merge:
  %out_next = phi <4 x i32> [%out, %lane], [%out, %check_outer], [%extreme_out, %minmax], [%new_out, %arithmetic]
  %mac_next = phi i32 [%masked_mac, %lane], [%mac, %check_outer], [%mac, %minmax], [%new_mac, %arithmetic]
  %latch_next = phi i32 [%latch, %lane], [%latch, %check_outer], [%latch, %minmax], [%new_latch, %arithmetic]
  %sticky_next = phi i32 [%sticky, %lane], [%sticky, %check_outer], [%sticky, %minmax], [%new_sticky, %arithmetic]
  %next = add i32 %n, 1
  %done = icmp eq i32 %next, 4
  br i1 %done, label %commit, label %lane
commit:
  br i1 %to_acc, label %write_acc, label %check_vf
write_acc:
  store <4 x i32> %out_next, ptr %acc_ptr, align 4
  store i32 %latch_next, ptr %latch_ptr
  br label %commit_flags
check_vf:
  %discard = icmp eq i32 %fd, 0
  br i1 %discard, label %commit_flags, label %write_vf
write_vf:
  %vf_base = getelementptr i8, ptr %cpu, i64 OFF_CPU_VF
  %dst = getelementptr <4 x i32>, ptr %vf_base, i32 %fd
  store <4 x i32> %out_next, ptr %dst, align 4
  br label %commit_flags
commit_flags:
  br i1 %flags, label %status, label %success
status:
  store i32 %mac_next, ptr %mac_ptr
  %vi17 = getelementptr i8, ptr %vu, i64 OFF_VI17
  store i32 %mac_next, ptr %vi17
  %gz = and i32 %mac_next, 15
  %gs = and i32 %mac_next, 240
  %gu = and i32 %mac_next, 3840
  %go = and i32 %mac_next, 61440
  %hz = icmp ne i32 %gz, 0
  %hs = icmp ne i32 %gs, 0
  %hu = icmp ne i32 %gu, 0
  %ho = icmp ne i32 %go, 0
  %fz = select i1 %hz, i32 65, i32 0
  %fs_flag = select i1 %hs, i32 130, i32 0
  %fu = select i1 %hu, i32 260, i32 0
  %fo = select i1 %ho, i32 520, i32 0
  %fzs = or i32 %fz, %fs_flag
  %fuo = or i32 %fu, %fo
  %f = or i32 %fzs, %fuo
  %status_ptr = getelementptr i8, ptr %vu, i64 OFF_STATUS
  %old_status = load i32, ptr %status_ptr
  %preserved = and i32 %old_status, 4080
  %with_flags = or i32 %preserved, %f
  %new_status = or i32 %with_flags, %sticky_next
  store i32 %new_status, ptr %status_ptr
  %vi16 = getelementptr i8, ptr %vu, i64 OFF_VI16
  store i32 %new_status, ptr %vi16
  br label %success
success:
  ret i32 0
fail:
  ret i32 1
}

; op 0=DIV, 1=SQRT, 2=RSQRT, 3=WAITQ. Schedule in virtual VU0
; cycles only; a failed sync or overlapping producer commits nothing.
define internal i32 @ee_vu0_ir_q(ptr %cpu, ptr %rt, i32 %pc, i32 %word, i32 %op) {
entry:
  %vu = call ptr @ee_vu0_ir_sync(ptr %cpu, ptr %rt, i32 %pc)
  %missing = icmp eq ptr %vu, null
  br i1 %missing, label %fail, label %fields
fields:
  %pending_ptr = getelementptr i8, ptr %vu, i64 OFF_PENDING
  %pending = load i32, ptr %pending_ptr
  %ready_ptr = getelementptr i8, ptr %vu, i64 OFF_READY
  %bits_ptr = getelementptr i8, ptr %vu, i64 OFF_QBITS
  %cycle_ptr = getelementptr i8, ptr %vu, i64 OFF_CYCLE
  %cycle = load i64, ptr %cycle_ptr
  %busy = icmp ne i32 %pending, 0
  %wait = icmp eq i32 %op, 3
  br i1 %wait, label %check_wait, label %check_produce
check_wait:
  br i1 %busy, label %retire, label %success
retire:
  %deadline = load i64, ptr %ready_ptr
  %early = icmp ult i64 %cycle, %deadline
  %new_cycle = select i1 %early, i64 %deadline, i64 %cycle
  store i64 %new_cycle, ptr %cycle_ptr
  %qbits = load i32, ptr %bits_ptr
  %q_ptr = getelementptr i8, ptr %vu, i64 OFF_Q
  store i32 %qbits, ptr %q_ptr
  %vi22 = getelementptr i8, ptr %vu, i64 OFF_VI22
  store i32 %qbits, ptr %vi22
  store i32 0, ptr %pending_ptr
  store i64 0, ptr %ready_ptr
  br label %success
check_produce:
  %rsqrt = icmp eq i32 %op, 2
  %latency = select i1 %rsqrt, i64 13, i64 7
  %latest = sub i64 -1, %latency
  %too_late = icmp ugt i64 %cycle, %latest
  %blocked = or i1 %busy, %too_late
  br i1 %blocked, label %fail, label %calculate
calculate:
  %fs_shift = lshr i32 %word, 11
  %ft_shift = lshr i32 %word, 16
  %fs = and i32 %fs_shift, 31
  %ft = and i32 %ft_shift, 31
  %fsf_shift = lshr i32 %word, 21
  %ftf_shift = lshr i32 %word, 23
  %fsf = and i32 %fsf_shift, 3
  %ftf = and i32 %ftf_shift, 3
  %av = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %fs)
  %bv = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %ft)
  %a = extractelement <4 x i32> %av, i32 %fsf
  %b = extractelement <4 x i32> %bv, i32 %ftf
  %sqrt = icmp eq i32 %op, 1
  %div = icmp eq i32 %op, 0
  br i1 %div, label %divide, label %root
divide:
  %quotient = call i32 @ee_cop1_radix_quotient(i32 %a, i32 %b)
  br label %publish
root:
  %root_bits = call i32 @ee_cop1_radix_root(i32 %b)
  br i1 %sqrt, label %publish, label %reciprocal
reciprocal:
  %rsqrt_bits = call i32 @ee_cop1_radix_quotient(i32 %a, i32 %root_bits)
  br label %publish
publish:
  %result = phi i32 [%quotient, %divide], [%root_bits, %root], [%rsqrt_bits, %reciprocal]
  %deadline_new = add i64 %cycle, %latency
  store i32 %result, ptr %bits_ptr
  store i64 %deadline_new, ptr %ready_ptr
  store i32 1, ptr %pending_ptr
  %status_ptr = getelementptr i8, ptr %vu, i64 OFF_STATUS
  %status = load i32, ptr %status_ptr
  %updated = call i32 @vu_q_status(i32 %status, i32 %a, i32 %b, i1 %sqrt, i1 %rsqrt)
  store i32 %updated, ptr %status_ptr
  %vi16 = getelementptr i8, ptr %vu, i64 OFF_VI16
  store i32 %updated, ptr %vi16
  br label %success
success:
  ret i32 0
fail:
  ret i32 1
}
)IR";
    // Derive layout offsets from the same C++ ABI used by the runtime.
    const auto substitute = [&ir](const char* token, size_t offset)
    {
        const std::string value = std::to_string(offset);
        const size_t length = std::char_traits<char>::length(token);
        for (size_t pos = ir.find(token); pos != std::string::npos;
             pos = ir.find(token, pos + value.size()))
        {
            ir.replace(pos, length, value);
        }
    };
    substitute("OFF_RT_STATE", offsetof(ee::s_ee_runtime, m_vu0_state));
    substitute("OFF_RT_SYNC", offsetof(ee::s_ee_runtime, m_vu0_sync));
    substitute("OFF_RT_USER", offsetof(ee::s_ee_runtime, m_vu0_user));
    substitute("OFF_PENDING", offsetof(ee::s_ee_vu0_state, m_q_pending));
    substitute("OFF_CPU_VF", offsetof(ee::s_ee_cpu_state, m_vu0));
    substitute("OFF_STATUS", offsetof(ee::s_ee_vu0_state, m_status_flags));
    substitute("OFF_LATCH", offsetof(ee::s_ee_vu0_state, m_acc_overflow_mask));
    substitute("OFF_READY", offsetof(ee::s_ee_vu0_state, m_q_ready_cycle));
    substitute("OFF_QBITS", offsetof(ee::s_ee_vu0_state, m_q_pending_bits));
    substitute("OFF_CYCLE", offsetof(ee::s_ee_vu0_state, m_cycle));
    substitute("OFF_VI16", offsetof(ee::s_ee_vu0_state, m_vi) + 16 * sizeof(uint32_t));
    substitute("OFF_VI17", offsetof(ee::s_ee_vu0_state, m_vi) + 17 * sizeof(uint32_t));
    substitute("OFF_VI22", offsetof(ee::s_ee_vu0_state, m_vi) + 22 * sizeof(uint32_t));
    substitute("OFF_ACC", offsetof(ee::s_ee_vu0_state, m_acc));
    substitute("OFF_MAC", offsetof(ee::s_ee_vu0_state, m_mac_flags));
    substitute("OFF_Q", offsetof(ee::s_ee_vu0_state, m_q));
    substitute("OFF_I", offsetof(ee::s_ee_vu0_state, m_vi) + 21 * sizeof(uint32_t));
    return ir;
}
}
