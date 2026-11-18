/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 6 (see skyrim_native.c for the
 * rules; the addresses are listed in tools/native_overrides/g6.txt, calling-convention test setups in
 * tools/native_overrides/g6_abi.py). Depends on x86.h only. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
/* Guest memory other threads can see, accessed the way the lifted code's TL16 / TL32 / TS32 do (lift_prelude.h):
 * with LB_STRICT_ORDER, load-acquire / store-release on aligned addresses and a plain access with a fence on
 * unaligned ones; otherwise volatile. The g6_*_host forms take a host pointer the caller has checked is aligned. */
#if defined(__aarch64__) && defined(LB_STRICT_ORDER)
static inline uint16_t g6_tl16(uint32_t a) {
    if (__builtin_expect(!(a & 1), 1)) return __atomic_load_n((uint16_t *)(MEM + a), __ATOMIC_ACQUIRE);
    uint16_t v; memcpy(&v, MEM + a, sizeof v); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
static inline uint32_t g6_tl32(uint32_t a) {
    if (__builtin_expect(!(a & 3), 1)) return __atomic_load_n((uint32_t *)(MEM + a), __ATOMIC_ACQUIRE);
    uint32_t v; memcpy(&v, MEM + a, sizeof v); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
static inline void g6_ts32(uint32_t a, uint32_t v) {
    if (__builtin_expect(!(a & 3), 1)) { __atomic_store_n((uint32_t *)(MEM + a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(MEM + a, &v, sizeof v);
}
#define G6_LD_HOST(p) __atomic_load_n((uint32_t *)(p), __ATOMIC_ACQUIRE)
#define G6_ST_HOST(p, v) __atomic_store_n((uint32_t *)(p), (v), __ATOMIC_RELEASE)
#elif defined(__aarch64__)
#define g6_tl16(a) (*(volatile u16u *)(MEM + (uint32_t)(a)))
#define g6_tl32(a) (*(volatile u32u *)(MEM + (uint32_t)(a)))
#define g6_ts32(a, v) (*(volatile u32u *)(MEM + (uint32_t)(a)) = (v))
#define G6_LD_HOST(p) (*(volatile uint32_t *)(p))
#define G6_ST_HOST(p, v) (*(volatile uint32_t *)(p) = (v))
#else
#define g6_tl16(a) M16(a)
#define g6_tl32(a) M32(a)
#define g6_ts32(a, v) (M32(a) = (v))
#define G6_LD_HOST(p) (*(u32u *)(p))
#define G6_ST_HOST(p, v) (*(u32u *)(p) = (v))
#endif

/* 0xCC4AC0: gathers 48-byte records by 16-bit index. cdecl: [esp+4] = destination, [esp+8] = record array,
 * [esp+0xC] = uint16 index array, [esp+0x10] = count (unsigned). For each index i in turn it copies the twelve
 * dwords of record i to the destination, one dword load and store after another, and moves the destination on by
 * 48 bytes.
 *
 * Kept exactly: every access in the guest's order (index load, then load / store per dword, so overlapping
 * buffers read what earlier stores wrote) and with its ordering; ecx = the destination when the count is 0, else
 * the end of what was written; edx = 0 after a copy; eax = the last dword copied (unchanged when the count is 0);
 * the guest's saves of ebx, esi and edi below the stack pointer, and their restores read back from there, as the
 * guest's pops do. Each record is copied through host pointers with fixed offsets when both it and the
 * destination are dword aligned and don't wrap the 4 GiB guest space (they always are in the game); otherwise
 * dword by dword with the guest's 32-bit address arithmetic. */
void f_CC4AC0(CPU *c) {
    uint32_t esp = c->r[4], n = M32(esp + 0x10), d = M32(esp + 4);
    c->r[1] = d;
    c->r[2] = n;
    c->r[4] = esp + 4;
    if (!n) return;
    M32(esp - 4) = c->r[3];
    M32(esp - 8) = c->r[6];
    M32(esp - 12) = c->r[7];
    uint32_t idx = M32(esp + 0xC), src = M32(esp + 8), v = 0;
    do {
        uint32_t s = (uint32_t)g6_tl16(idx) * 48u + src;
        if (__builtin_expect(!((s | d) & 3) && s <= 0xFFFFFFD0u && d <= 0xFFFFFFD0u, 1)) {
            uint8_t *ps = MEM + s, *pd = MEM + d;
            v = G6_LD_HOST(ps);      G6_ST_HOST(pd, v);
            v = G6_LD_HOST(ps + 4);  G6_ST_HOST(pd + 4, v);
            v = G6_LD_HOST(ps + 8);  G6_ST_HOST(pd + 8, v);
            v = G6_LD_HOST(ps + 12); G6_ST_HOST(pd + 12, v);
            v = G6_LD_HOST(ps + 16); G6_ST_HOST(pd + 16, v);
            v = G6_LD_HOST(ps + 20); G6_ST_HOST(pd + 20, v);
            v = G6_LD_HOST(ps + 24); G6_ST_HOST(pd + 24, v);
            v = G6_LD_HOST(ps + 28); G6_ST_HOST(pd + 28, v);
            v = G6_LD_HOST(ps + 32); G6_ST_HOST(pd + 32, v);
            v = G6_LD_HOST(ps + 36); G6_ST_HOST(pd + 36, v);
            v = G6_LD_HOST(ps + 40); G6_ST_HOST(pd + 40, v);
            v = G6_LD_HOST(ps + 44); G6_ST_HOST(pd + 44, v);
        } else {
            for (uint32_t k = 0; k < 48; k += 4) {
                v = g6_tl32(s + k);
                g6_ts32(d + k, v);
            }
        }
        idx += 2;
        d += 48;
    } while (--n);
    c->r[0] = v;
    c->r[1] = d;
    c->r[2] = 0;
    c->r[3] = M32(esp - 4);
    c->r[6] = M32(esp - 8);
    c->r[7] = M32(esp - 12);
}
#endif
