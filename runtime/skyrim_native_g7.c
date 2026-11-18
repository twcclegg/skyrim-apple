/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 7 (see skyrim_native.c for the
 * rules; the addresses are listed in tools/native_overrides/g7.txt, calling-convention test setups in
 * tools/native_overrides/g7_abi.py). Depends on x86.h only. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
/* Guest memory accesses the lifted code makes ordered (TL32/TS32 in lift_prelude.h, which only generated files
 * include), with the same three build variants: volatile by default on arm64, load-acquire / store-release under
 * LB_STRICT_ORDER (a plain access with a fence when unaligned), plain elsewhere. */
#if defined(__aarch64__) && !defined(LB_STRICT_ORDER)
#define G7_ALIGNED4(a) 0
#define G7_L32(a) (*(volatile u32u *)(MEM + (uint32_t)(a)))
#define G7_S32(a, v) (*(volatile u32u *)(MEM + (uint32_t)(a)) = (v))
#elif defined(__aarch64__)
/* aligned, and nine dwords from it don't wrap past 4 GiB (the macros wrap the address, a host pointer doesn't) */
#define G7_ALIGNED4(a) __builtin_expect(!((a) & 3) && (a) <= 0xFFFFFFDCu, 1)
static inline uint32_t G7_L32(uint32_t a) {
    if (__builtin_expect(!(a & 3), 1)) return __atomic_load_n((uint32_t *)(MEM + a), __ATOMIC_ACQUIRE);
    uint32_t v; memcpy(&v, MEM + a, sizeof v); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; }
static inline void G7_S32(uint32_t a, uint32_t v) {
    if (__builtin_expect(!(a & 3), 1)) { __atomic_store_n((uint32_t *)(MEM + a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(MEM + a, &v, sizeof v); }
#else
#define G7_ALIGNED4(a) 0
#define G7_L32(a) M32(a)
#define G7_S32(a, v) (M32(a) = (v))
#endif

/* 0x4488B0: 3x3 matrix transpose, out = in^T (nine dwords each, row-major). thiscall: ecx = in, [esp+4] = out,
 * callee pops it; called by 0x486C20 (out in the caller's frame) and others. The guest reads all nine
 * elements (in the order below) before it writes any, so in == out and any other overlap come out the same here.
 * Kept exactly: every element read and write in the guest's order with the guest's ordered accesses, and the
 * registers it leaves: eax = out, ecx = in[8], edx = in[7], the others as they were. Not written: the guest's
 * register saves and its three element copies below the stack pointer, dead once it returns. */
void f_4488B0(CPU *c) {
    uint32_t esp = c->r[4], in = c->r[1];
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7, a8;
    if (G7_ALIGNED4(in)) {  /* one alignment test instead of nine; the same accesses as G7_L32 */
        uint32_t *p = (uint32_t *)(MEM + in);
        a5 = __atomic_load_n(p + 5, __ATOMIC_ACQUIRE); a0 = __atomic_load_n(p + 0, __ATOMIC_ACQUIRE);
        a3 = __atomic_load_n(p + 3, __ATOMIC_ACQUIRE); a4 = __atomic_load_n(p + 4, __ATOMIC_ACQUIRE);
        a6 = __atomic_load_n(p + 6, __ATOMIC_ACQUIRE); a1 = __atomic_load_n(p + 1, __ATOMIC_ACQUIRE);
        a7 = __atomic_load_n(p + 7, __ATOMIC_ACQUIRE); a2 = __atomic_load_n(p + 2, __ATOMIC_ACQUIRE);
        a8 = __atomic_load_n(p + 8, __ATOMIC_ACQUIRE);
    } else {
        a5 = G7_L32(in + 0x14); a0 = G7_L32(in); a3 = G7_L32(in + 0xC); a4 = G7_L32(in + 0x10);
        a6 = G7_L32(in + 0x18); a1 = G7_L32(in + 4); a7 = G7_L32(in + 0x1C); a2 = G7_L32(in + 8);
        a8 = G7_L32(in + 0x20);
    }
    uint32_t out = M32(esp + 4);
    if (G7_ALIGNED4(out)) {
        uint32_t *q = (uint32_t *)(MEM + out);
        __atomic_store_n(q + 0, a0, __ATOMIC_RELEASE); __atomic_store_n(q + 6, a2, __ATOMIC_RELEASE);
        __atomic_store_n(q + 7, a5, __ATOMIC_RELEASE); __atomic_store_n(q + 3, a1, __ATOMIC_RELEASE);
        __atomic_store_n(q + 4, a4, __ATOMIC_RELEASE); __atomic_store_n(q + 2, a6, __ATOMIC_RELEASE);
        __atomic_store_n(q + 1, a3, __ATOMIC_RELEASE); __atomic_store_n(q + 5, a7, __ATOMIC_RELEASE);
        __atomic_store_n(q + 8, a8, __ATOMIC_RELEASE);
    } else {
        G7_S32(out, a0); G7_S32(out + 0x18, a2); G7_S32(out + 0x1C, a5); G7_S32(out + 0xC, a1);
        G7_S32(out + 0x10, a4); G7_S32(out + 8, a6); G7_S32(out + 4, a3); G7_S32(out + 0x14, a7);
        G7_S32(out + 0x20, a8);
    }
    c->r[0] = out;
    c->r[1] = a8;
    c->r[2] = a7;
    c->r[4] = esp + 8;
}
#endif
