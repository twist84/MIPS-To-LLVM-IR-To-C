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

define internal <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %index) alwaysinline {
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

; Lane order is x,y,z,w; guest mask/flag bits are 8,4,2,1.
define internal i32 @ee_vu0_ir_pack_mask(<4 x i1> %lanes) alwaysinline {
  %reversed = shufflevector <4 x i1> %lanes, <4 x i1> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
  %packed = bitcast <4 x i1> %reversed to i4
  %bits = zext i4 %packed to i32
  ret i32 %bits
}

define internal <4 x i32> @ee_vu0_ir_order(<4 x i32> %x) alwaysinline {
  %negative = icmp slt <4 x i32> %x, zeroinitializer
  %neg = xor <4 x i32> %x, <i32 -1, i32 -1, i32 -1, i32 -1>
  %pos = xor <4 x i32> %x, <i32 -2147483648, i32 -2147483648, i32 -2147483648, i32 -2147483648>
  %key = select <4 x i1> %negative, <4 x i32> %neg, <4 x i32> %pos
  ret <4 x i32> %key
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
  ; Broadcast and outer-product permutations are formed once for all lanes.
  %a_outer = shufflevector <4 x i32> %a, <4 x i32> poison, <4 x i32> <i32 1, i32 2, i32 0, i32 3>
  %b_outer = shufflevector <4 x i32> %b, <4 x i32> poison, <4 x i32> <i32 2, i32 0, i32 1, i32 3>
  %av = select i1 %outer, <4 x i32> %a_outer, <4 x i32> %a
  %bc_word = extractelement <4 x i32> %b, i32 %bc
  %bc_insert = insertelement <4 x i32> poison, i32 %bc_word, i32 0
  %bc_vector = shufflevector <4 x i32> %bc_insert, <4 x i32> poison, <4 x i32> zeroinitializer
  %scalar_insert = insertelement <4 x i32> poison, i32 %scalar, i32 0
  %scalar_vector = shufflevector <4 x i32> %scalar_insert, <4 x i32> poison, <4 x i32> zeroinitializer
  %b_bc = select i1 %is_bc, <4 x i32> %bc_vector, <4 x i32> %b
  %b_permuted = select i1 %outer, <4 x i32> %b_outer, <4 x i32> %b_bc
  %rhs = select i1 %use_scalar, <4 x i32> %scalar_vector, <4 x i32> %b_permuted
  %lane_bits = add <4 x i32> zeroinitializer, <i32 8, i32 4, i32 2, i32 1>
  %mask_insert = insertelement <4 x i32> poison, i32 %mask, i32 0
  %mask_vector = shufflevector <4 x i32> %mask_insert, <4 x i32> poison, <4 x i32> zeroinitializer
  %selected_bits = and <4 x i32> %mask_vector, %lane_bits
  %selected = icmp ne <4 x i32> %selected_bits, zeroinitializer
  ; Outer products do not compute W. A selected W keeps its old MAC bits;
  ; an unselected W clears its MAC group exactly like the scalar path.
  %eligible = select i1 %outer, <4 x i1> <i1 true, i1 true, i1 true, i1 false>, <4 x i1> <i1 true, i1 true, i1 true, i1 true>
  %active = and <4 x i1> %selected, %eligible
  br i1 %flags, label %arithmetic, label %minmax
minmax:
  %ak = call <4 x i32> @ee_vu0_ir_order(<4 x i32> %av)
  %bk = call <4 x i32> @ee_vu0_ir_order(<4 x i32> %rhs)
  %ge = icmp uge <4 x i32> %ak, %bk
  %le = icmp ule <4 x i32> %ak, %bk
  %pick = select i1 %max, <4 x i1> %ge, <4 x i1> %le
  %extreme = select <4 x i1> %pick, <4 x i32> %av, <4 x i32> %rhs
  %extreme_out = select <4 x i1> %active, <4 x i32> %extreme, <4 x i32> %dest_old
  br label %commit
arithmetic:
  %latch_insert = insertelement <4 x i32> poison, i32 %latch_old, i32 0
  %latch_vector = shufflevector <4 x i32> %latch_insert, <4 x i32> poison, <4 x i32> zeroinitializer
  %latch_bits = and <4 x i32> %latch_vector, %lane_bits
  %latched = icmp ne <4 x i32> %latch_bits, zeroinitializer
  ; Inactive lanes cannot contribute Booth work or sticky flags.
  %safe_a = select <4 x i1> %active, <4 x i32> %av, <4 x i32> zeroinitializer
  %safe_b = select <4 x i1> %active, <4 x i32> %rhs, <4 x i32> zeroinitializer
  %tuple = call { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } @vu_fmac4(i32 %kind, <4 x i32> %acc, <4 x i32> %safe_a, <4 x i32> %safe_b, <4 x i1> %latched)
  %bits = extractvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %tuple, 0
  %overflow = extractvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %tuple, 1
  %underflow = extractvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %tuple, 2
  %product_sticky = extractvalue { <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> } %tuple, 3
  %exp = and <4 x i32> %bits, <i32 2139095040, i32 2139095040, i32 2139095040, i32 2139095040>
  %zero = icmp eq <4 x i32> %exp, zeroinitializer
  %sign = icmp slt <4 x i32> %bits, zeroinitializer
  %z_active = and <4 x i1> %zero, %active
  %s_active = and <4 x i1> %sign, %active
  %u_active = and <4 x i1> %underflow, %active
  %o_active = and <4 x i1> %overflow, %active
  %z = call i32 @ee_vu0_ir_pack_mask(<4 x i1> %z_active)
  %s_bits = call i32 @ee_vu0_ir_pack_mask(<4 x i1> %s_active)
  %u_bits = call i32 @ee_vu0_ir_pack_mask(<4 x i1> %u_active)
  %o_bits = call i32 @ee_vu0_ir_pack_mask(<4 x i1> %o_active)
  %s = shl i32 %s_bits, 4
  %u = shl i32 %u_bits, 8
  %o = shl i32 %o_bits, 12
  %zs = or i32 %z, %s
  %uo = or i32 %u, %o
  %lane_flags = or i32 %zs, %uo
  %selected_w = extractelement <4 x i1> %selected, i32 3
  %preserve_w = and i1 %outer, %selected_w
  %keep = select i1 %preserve_w, i32 -61167, i32 -65536
  %cleared = and i32 %mac_old, %keep
  %new_mac = or i32 %cleared, %lane_flags
  %active_mask = call i32 @ee_vu0_ir_pack_mask(<4 x i1> %active)
  %not_active = xor i32 %active_mask, -1
  %latch_clear = and i32 %latch_old, %not_active
  %updated_latch = or i32 %latch_clear, %o_bits
  %new_latch = select i1 %to_acc, i32 %updated_latch, i32 %latch_old
  %sticky_active = select <4 x i1> %active, <4 x i32> %product_sticky, <4 x i32> zeroinitializer
  %sticky0 = extractelement <4 x i32> %sticky_active, i32 0
  %sticky1 = extractelement <4 x i32> %sticky_active, i32 1
  %sticky2 = extractelement <4 x i32> %sticky_active, i32 2
  %sticky3 = extractelement <4 x i32> %sticky_active, i32 3
  %sticky01 = or i32 %sticky0, %sticky1
  %sticky23 = or i32 %sticky2, %sticky3
  %new_sticky = or i32 %sticky01, %sticky23
  %new_out = select <4 x i1> %active, <4 x i32> %bits, <4 x i32> %dest_old
  br label %commit
commit:
  %out_next = phi <4 x i32> [%extreme_out, %minmax], [%new_out, %arithmetic]
  %mac_next = phi i32 [%mac_old, %minmax], [%new_mac, %arithmetic]
  %latch_next = phi i32 [%latch_old, %minmax], [%new_latch, %arithmetic]
  %sticky_next = phi i32 [0, %minmax], [%new_sticky, %arithmetic]
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
; Fixed-point conversions, raw moves, CLIP and NOP. The encoding
; selects the destination mask; no host float or opcode callback.
define internal i32 @ee_vu0_ir_misc(ptr %cpu, ptr %rt, i32 %pc, i32 %word, i32 %op) {
entry:
  %vu = call ptr @ee_vu0_ir_sync(ptr %cpu, ptr %rt, i32 %pc)
  %missing = icmp eq ptr %vu, null
  br i1 %missing, label %fail, label %dispatch
dispatch:
  %nop = icmp eq i32 %op, 47
  br i1 %nop, label %success, label %prepare
prepare:
  %fs_shift = lshr i32 %word, 11
  %ft_shift = lshr i32 %word, 16
  %mask_shift = lshr i32 %word, 21
  %fs = and i32 %fs_shift, 31
  %ft = and i32 %ft_shift, 31
  %mask = and i32 %mask_shift, 15
  %source = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %fs)
  %old = call <4 x i32> @ee_vu0_ir_read_vf(ptr %cpu, i32 %ft)
  %clip = icmp eq i32 %op, 31
  br i1 %clip, label %clip_prepare, label %vector_prepare
clip_prepare:
  %w = extractelement <4 x i32> %old, i32 3
  %abs_w = and i32 %w, 2147483647
  %limit = call i32 @vu_key(i32 %abs_w)
  %neg_limit = sub i32 0, %limit
  %clip_ptr = getelementptr i8, ptr %vu, i64 OFF_CLIP
  %old_clip = load i32, ptr %clip_ptr
  %shifted = shl i32 %old_clip, 6
  %history = and i32 %shifted, 16777215
  br label %clip_lane
clip_lane:
  %c = phi i32 [0, %clip_prepare], [%c_next, %clip_lane]
  %clip_bits = phi i32 [%history, %clip_prepare], [%new_clip, %clip_lane]
  %raw = extractelement <4 x i32> %source, i32 %c
  %value = call i32 @vu_key(i32 %raw)
  %above = icmp sgt i32 %value, %limit
  %below = icmp slt i32 %value, %neg_limit
  %positive = select i1 %above, i32 1, i32 0
  %negative = select i1 %below, i32 2, i32 0
  %judge = or i32 %positive, %negative
  %cshift = shl i32 %c, 1
  %lane_bits = shl i32 %judge, %cshift
  %new_clip = or i32 %clip_bits, %lane_bits
  %c_next = add i32 %c, 1
  %clip_done = icmp eq i32 %c_next, 3
  br i1 %clip_done, label %clip_commit, label %clip_lane
clip_commit:
  store i32 %new_clip, ptr %clip_ptr
  %vi18 = getelementptr i8, ptr %vu, i64 OFF_VI18
  store i32 %new_clip, ptr %vi18
  br label %success
vector_prepare:
  %vf0 = icmp eq i32 %ft, 0
  br i1 %vf0, label %success, label %config
config:
  %is_abs = icmp eq i32 %op, 29
  %rotate = icmp eq i32 %op, 49
  %convert = icmp ult i32 %op, 24
  %to_float = icmp ult i32 %op, 20
  %idx = and i32 %op, 3
  %frac = extractelement <4 x i32> <i32 0, i32 4, i32 12, i32 15>, i32 %idx
  br label %lane
lane:
  %n = phi i32 [0, %config], [%next, %merge]
  %out = phi <4 x i32> [%old, %config], [%out_next, %merge]
  %shift = sub i32 3, %n
  %bit = shl i32 1, %shift
  %selected = and i32 %mask, %bit
  %active = icmp ne i32 %selected, 0
  br i1 %active, label %read, label %merge
read:
  %n1 = add i32 %n, 1
  %rotated = and i32 %n1, 3
  %index = select i1 %rotate, i32 %rotated, i32 %n
  %bits = extractelement <4 x i32> %source, i32 %index
  br i1 %convert, label %conversion, label %move
conversion:
  br i1 %to_float, label %itof, label %ftoi
itof:
  %float_bits = call i32 @vu_from_fixed(i32 %bits, i32 %frac)
  br label %result
ftoi:
  %int_bits = call i32 @vu_to_fixed(i32 %bits, i32 %frac)
  br label %result
move:
  %absolute = and i32 %bits, 2147483647
  %moved = select i1 %is_abs, i32 %absolute, i32 %bits
  br label %result
result:
  %r = phi i32 [%float_bits, %itof], [%int_bits, %ftoi], [%moved, %move]
  %updated = insertelement <4 x i32> %out, i32 %r, i32 %n
  br label %merge
merge:
  %out_next = phi <4 x i32> [%out, %lane], [%updated, %result]
  %next = add i32 %n, 1
  %done = icmp eq i32 %next, 4
  br i1 %done, label %commit, label %lane
commit:
  %base = getelementptr i8, ptr %cpu, i64 OFF_CPU_VF
  %dst = getelementptr <4 x i32>, ptr %base, i32 %ft
  store <4 x i32> %out_next, ptr %dst, align 4
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
    substitute("OFF_CLIP", offsetof(ee::s_ee_vu0_state, m_clip_flags));
    substitute("OFF_VI18", offsetof(ee::s_ee_vu0_state, m_vi) + 18 * sizeof(uint32_t));
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
