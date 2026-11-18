/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 8 (listed in
 * tools/native_overrides/g8.txt; calling-convention test setups in tools/native_overrides/g8_abi.py).
 *
 * Same rules as runtime/skyrim_native.c: each one leaves what other code can observe exactly as the lifted guest
 * does, checked by tools/native_test.py. They inline guest helpers (C runtime routines, 0x472580) or call guest
 * code, so they are checked at the calling convention against the guest's whole lifted call tree: the guest's
 * frames below the return address (saved registers, locals, return addresses of calls made inline here) are not
 * written. Everything else is left as the guest leaves it, including the registers a caller may not rely on and
 * the stack a guest callee sees.
 *
 * Depends on x86.h and rt_table.h only, so the test can compile this file on its own. */
#pragma clang fp contract(off)
#include "x86.h"
#include "rt_table.h"

#ifdef GAME_SKYRIM
/* Guest memory the lifted code accesses with TL32, TL8 and TS32 (data other threads may see) is accessed the same
 * way here: the definitions follow runtime/lift_prelude.h (load-acquire / store-release with LB_STRICT_ORDER on
 * arm64, else volatile), under other names because the test compiles this file after the prelude. */
#if defined(__aarch64__) && defined(LB_STRICT_ORDER)
static inline uint32_t g8_tl32(uint32_t a) {
    if (__builtin_expect(!(a & 3), 1)) return __atomic_load_n((uint32_t *)(MEM + a), __ATOMIC_ACQUIRE);
    uint32_t v;
    memcpy(&v, MEM + a, sizeof v);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return v;
}
static inline uint8_t g8_tl8(uint32_t a) { return __atomic_load_n(MEM + a, __ATOMIC_ACQUIRE); }
static inline void g8_ts32(uint32_t a, uint32_t v) {
    if (__builtin_expect(!(a & 3), 1)) { __atomic_store_n((uint32_t *)(MEM + a), v, __ATOMIC_RELEASE); return; }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(MEM + a, &v, sizeof v);
}
#elif defined(__aarch64__)
#define g8_tl32(a) (*(volatile u32u *)(MEM + (uint32_t)(a)))
#define g8_tl8(a) (*(volatile uint8_t *)(MEM + (uint32_t)(a)))
#define g8_ts32(a, v) (*(volatile u32u *)(MEM + (uint32_t)(a)) = (v))
#else
#define g8_tl32(a) M32(a)
#define g8_tl8(a) M8(a)
#define g8_ts32(a, v) (M32(a) = (v))
#endif

/* 0x472580's test (see runtime/skyrim_native.c), with the same roundings and the same x87 registers and condition
 * codes left behind; its two float temporaries in the argument slot, dead stack once it returns, are kept in
 * registers. Returns 3 for a NaN input, which takes the call to 0x472580 itself. */
static inline uint32_t g8_plane_side(CPU *c, uint32_t plane, uint32_t sphere) {
    for (int k = 0; k < 4; k++)
        if (MF32(sphere + 4 * k) != MF32(sphere + 4 * k) || MF32(plane + 4 * k) != MF32(plane + 4 * k)) return 3;
    uint32_t t1 = (c->top - 1) & 7, t2 = (c->top - 2) & 7;
    double dist = (double)MF32(sphere + 4) * (double)MF32(plane + 4);
    dist += (double)MF32(sphere) * (double)MF32(plane);
    dist += (double)MF32(sphere + 8) * (double)MF32(plane + 8);
    float tmp = (float)dist;
    dist = (double)tmp - (double)MF32(sphere + 12);
    tmp = (float)dist;
    dist = (double)tmp;
    double d = (double)MF32(plane + 12);
    uint32_t result;
    c->st[t1] = dist;
    if (dist != dist || d != d || -d < dist) {
        c->st[t2] = d;
        if (dist != dist || d != d) { c->c0 = c->c2 = c->c3 = 1; result = 0; }
        else { c->c0 = d < dist; c->c2 = 0; c->c3 = d == dist; result = d < dist ? 1 : d == dist ? 1 : 0; }
    } else {
        c->st[t2] = -d;
        c->c0 = 0; c->c2 = 0; c->c3 = -d == dist;
        result = 2;
    }
    return result;
}
/* the lifted code's indirect call through a per-call-site cache (see lift_terminator in tools/lift.py) */
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache);
void rt_host_call_guest(CPU *c, void (*fn)(CPU *));

void f_F531CC_guest(CPU *c);
void f_472580(CPU *c);
void f_F6029C_guest(CPU *c);

/* 0xF531CC: the C runtime's fabs(double), cdecl, the argument at [esp+4], the result in ST(0). The guest sets the
 * x87 control word to 0x133F through _ctrlfp (0xF60C6C), copies the argument with the sign bit cleared into a
 * local, restores the control word through _ctrlfp and loads the local; an infinity or NaN (exponent all ones)
 * goes through _sptype and the C runtime's error paths instead, which stay with the lifted body here.
 *
 * What the finite path leaves: ST(0) = the argument with the sign bit cleared (one push, onto the slot fldz also
 * wrote), the control word as it was, eax = 0x133F (the second _ctrlfp returns the control word it found,
 * sign-extended), ecx = the old control word (zero-extended: the first _ctrlfp stored it over its own argument
 * slot, which the guest pops into ecx last), edx and the condition codes untouched. */
void f_F531CC(CPU *c) {
    uint32_t esp = c->r[4];
    uint64_t bits = M64(esp + 4);
    if ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) {
        f_F531CC_guest(c);
        return;
    }
    bits &= 0x7FFFFFFFFFFFFFFFull;
    double v;
    memcpy(&v, &bits, sizeof v);
    uint32_t top = (c->top - 1) & 7;
    c->st[top] = v;
    c->top = top;
    c->r[0] = 0x133Fu;
    c->r[1] = c->fpu_cw;
    c->r[4] = esp + 4;
}

/* 0x45C640: is bit n of a 64-bit mask set. thiscall: ecx = object (mask: low half at +0x20, high half at +0x24),
 * [esp+4] = n, callee pops it. The guest shifts 1 left by n with the C runtime's _allshl (0xF54600: by cl, which
 * is the low byte of n; 0 from 64 on; from 32 on it masks cl to 5 bits and shifts into the high half), ANDs it
 * with the mask (low half read first) and returns 1 when either half is non-zero, else 0, in eax. Left as the
 * guest leaves them: ecx = n (its low byte masked to 5 bits when it is 32 to 63), edx = the high half of the
 * AND. */
void f_45C640(CPU *c) {
    uint32_t esp = c->r[4], self = c->r[1], n = M32(esp + 4), cl = n & 0xFF;
    uint32_t lo = 0, hi = 0;
    if (cl < 32) {
        lo = 1u << cl;
    } else if (cl < 64) {
        cl &= 0x1F;
        n = (n & 0xFFFFFF00u) | cl;
        hi = 1u << cl;
    }
    lo &= g8_tl32(self + 0x20);
    hi &= g8_tl32(self + 0x24);
    c->r[0] = (lo | hi) != 0;
    c->r[1] = n;
    c->r[2] = hi;
    c->r[4] = esp + 8;
}

/* 0xACE100: cull an object's bound against a culler's planes. thiscall: ecx = culler (+0x2C: six planes of 16
 * bytes, +0x8C: mask of planes still to test, +0x10D: a byte flag), [esp+4] = object (+0x88: bound, +0x94: zero
 * means nothing to do, +0x98: flags, +0: vtable), callee pops it. Saves the mask, then for each plane whose mask
 * bit is set calls 0x472580 (ecx = the bound, argument = the plane): 2 (outside) stops the loop, 1 (inside)
 * clears the plane's bit. Outside: clears bit 26 of the object's flags if the culler's flag is set. Not outside:
 * calls the object's virtual function at +0xC4 with the culler (ecx = object) and sets that flag bit if the
 * culler's flag is set. Either way the saved mask goes back into the culler.
 *
 * Here the plane test is 0x472580's own native code inlined (a NaN input still calls 0x472580), which saves a
 * guest call with all its register spills per plane. Every shared-data access the lifted code makes with TL32,
 * TL8 and TS32 is made here in the same order. The virtual call is made as the lifted code makes it, with the
 * registers, the x87 registers, the argument and the return address it sees from the lifted code, and everything
 * after it reads the stack and the registers the call leaves, as the lifted code does. */
void f_ACE100(CPU *c) {
    uint32_t esp = c->r[4], self = c->r[1], ebx0 = c->r[3], edx = c->r[2];
    uint32_t ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    uint32_t obj = M32(esp + 4);
    if (g8_tl32(obj + 0x94) == 0) {  /* pop ebx, pop ecx: the registers come back */
        c->r[0] = obj;
        c->r[4] = esp + 8;
        return;
    }
    uint32_t mask = g8_tl32(self + 0x8C);
    uint32_t i;
    for (i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(g8_tl32(self + 0x8C) & bit)) continue;
        uint32_t plane = self + 0x2C + 16 * i, r = g8_plane_side(c, obj + 0x88, plane);
        if (r == 3) {  /* push the plane, call 0x472580 as the lifted code does */
            M32(esp - 24) = plane;
            M32(esp - 28) = 0xACE144u;
            c->r[0] = obj; c->r[1] = obj + 0x88; c->r[3] = self; c->r[4] = esp - 28;
            c->r[5] = plane; c->r[6] = bit; c->r[7] = i;
            f_472580(c);
            r = c->r[0];
            edx = c->r[2];
            /* the guest pops its saved ebp, esi and edi after the loop */
            c->r[3] = ebx0; c->r[4] = esp; c->r[5] = ebp0; c->r[6] = esi0; c->r[7] = edi0;
        }
        if (r == 2) break;
        if (r == 1) g8_ts32(self + 0x8C, g8_tl32(self + 0x8C) & ~bit);
    }
    if (i != 6) {  /* outside */
        if (g8_tl8(self + 0x10D)) g8_ts32(obj + 0x98, g8_tl32(obj + 0x98) & 0xFBFFFFFFu);
        g8_ts32(self + 0x8C, mask);
        c->r[0] = obj; c->r[1] = mask; c->r[2] = edx; c->r[3] = ebx0;
        c->r[4] = esp + 8;
        return;
    }
    /* push ecx (the slot then holds the saved mask), push ebx; ebp, esi and edi are popped again by now */
    M32(esp - 4) = mask;
    M32(esp - 8) = ebx0;
    uint32_t vt = g8_tl32(obj), t = g8_tl32(vt + 0xC4);
    M32(esp - 12) = self;
    M32(esp - 16) = 0xACE17Eu;
    c->r[0] = t; c->r[1] = obj; c->r[2] = vt; c->r[3] = self; c->r[4] = esp - 16;
    {
        static const FnEntry *ic_;
        const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED);
        /* A cached lifted entry goes through rt_host_call_guest: with pinned registers a lifted function
         * takes them in host registers, not from the CPU struct this host code keeps them in. A cached
         * import shim (at or above GUEST_THUNK_BASE) uses the struct and is called directly. */
        if (e_ && e_->addr == t) {
            if (t >= GUEST_THUNK_BASE) e_->fn(c); else rt_host_call_guest(c, e_->fn);
        } else rt_call_ic(c, t, &ic_);
    }
    uint32_t sp = c->r[4], ebx = c->r[3], eax = c->r[0];
    if (g8_tl8(ebx + 0x10D)) {
        eax = M32(sp + 0xC);
        uint32_t m = M32(sp + 4);
        g8_ts32(eax + 0x98, g8_tl32(eax + 0x98) | 0x4000000u);
        g8_ts32(ebx + 0x8C, m);
    } else {
        g8_ts32(ebx + 0x8C, M32(sp + 4));
    }
    c->r[0] = eax;
    c->r[3] = M32(sp);
    c->r[1] = M32(sp + 4);
    c->r[4] = sp + 16;
}

/* 0xF6029C: the C runtime's SSE2 block copy, which its memcpy (0xF52E30, through 0xF60323) uses for 16-byte
 * aligned copies of 256 bytes and more. cdecl: [esp+4] = dst, [esp+8] = src, [esp+12] = size; copies size / 128
 * blocks of 128 bytes, each as eight 16-byte loads into xmm0-xmm7 and stores, four at a time. The lifted body moves
 * every 16 bytes through the CPU struct's xmm registers; here the bytes are moved with memmove and only the last
 * block is left in xmm0-xmm7, as the guest leaves it. ecx ends at 0; eax, edx, esi and edi are as they were.
 *
 * Same result as the guest's forward copy: without overlap; and when dst is below src, since every block's
 * loads come before its stores and the blocks go forward, no byte is overwritten before it has been read, which
 * also leaves the last block's source bytes unchanged until it is loaded. dst above src with overlap, a size
 * below 128 (the guest's loop counter would wrap and copy 2^32 blocks) and a range that wraps past 4 GiB take the
 * lifted body. */
void f_F6029C(CPU *c) {
    uint32_t esp = c->r[4], dst = M32(esp + 4), src = M32(esp + 8), bytes = M32(esp + 12) & ~0x7Fu;
    if (bytes == 0 || (dst > src && dst - src < bytes) || (uint64_t)src + bytes > 0x100000000ull ||
        (uint64_t)dst + bytes > 0x100000000ull) {
        f_F6029C_guest(c);
        return;
    }
    memcpy(c->xmm, MEM + src + bytes - 128, 128);
    memmove(MEM + dst, MEM + src, bytes);
    c->r[1] = 0;
    c->r[4] = esp + 4;
}
#endif
