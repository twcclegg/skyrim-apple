/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 5 (see skyrim_native.c for the
 * rules; the addresses are listed in tools/native_overrides/g5.txt, calling-convention test setups in
 * tools/native_overrides/g5_abi.py). Shared guest data is read and written through the same ordered accessors
 * (TL32 / TS32) as the lifted code, so LB_STRICT_ORDER keeps its meaning here. */
#pragma clang fp contract(off)  /* one rounding per operation, as the lifted body does */
#include "x86.h"
#include "lift_prelude.h"

#ifdef GAME_SKYRIM
void f_472580(CPU *c);

/* The two frustum-culling loops below call the plane/sphere test 0x472580 (see skyrim_native.c) up to six times
 * with the same first operand P (ecx: x, y, z, then w) and a different plane each time (the argument: x, y, z,
 * then w). Here the test is done inline: P is loaded once, the float temporaries the guest keeps in its argument
 * slot stay in registers (same roundings), and the x87 state of the last test (two registers below the top,
 * C0/C2/C3) is written to the CPU once, when the loop ends or before a real call. A plane or P holding an
 * infinity or a NaN makes the real call instead, exactly as the guest does it. */
typedef struct {
    double px, py, pz, pd, st1, st2;
    uint32_t c0, c2, c3;
    int loaded, finite, pending;
} G5Cull;

/* exponent not all ones: neither infinity nor NaN */
static inline int g5_finite(uint32_t a) { return (M32(a) & 0x7F800000u) != 0x7F800000u; }

/* The result of 0x472580(ecx = P, argument = plane S) in *res, or 0 when the real call must be made. P is read
 * at the first test, as the guest first reads it there. */
static inline int g5_test(G5Cull *s, uint32_t P, uint32_t S, uint32_t *res) {
    if (!s->loaded) {
        s->loaded = 1;
        s->finite = g5_finite(P) & g5_finite(P + 4) & g5_finite(P + 8) & g5_finite(P + 12);
        s->px = (double)MF32(P); s->py = (double)MF32(P + 4); s->pz = (double)MF32(P + 8);
        s->pd = (double)MF32(P + 12);
    }
    if (__builtin_expect(!(s->finite & g5_finite(S) & g5_finite(S + 4) & g5_finite(S + 8) & g5_finite(S + 12)), 0))
        return 0;
    double dist = (double)MF32(S + 4) * s->py;
    dist += (double)MF32(S) * s->px;
    dist += (double)MF32(S + 8) * s->pz;
    float t = (float)dist;
    dist = (double)t - (double)MF32(S + 12);
    t = (float)dist;
    dist = (double)t;
    double d = s->pd;
    s->st1 = dist;
    if (-d < dist) {
        s->st2 = d; s->c0 = d < dist; s->c2 = 0; s->c3 = d == dist;
        *res = d <= dist ? 1 : 0;
    } else {
        s->st2 = -d; s->c0 = 0; s->c2 = 0; s->c3 = -d == dist;
        *res = 2;
    }
    s->pending = 1;
    return 1;
}

static inline void g5_flush(CPU *c, G5Cull *s) {
    if (s->pending) {
        c->st[(c->top - 1) & 7] = s->st1; c->st[(c->top - 2) & 7] = s->st2;
        c->c0 = s->c0; c->c2 = s->c2; c->c3 = s->c3;
        s->pending = 0;
    }
}

/* 1 when [a, a + 16) and the dword at m overlap (a mask write would change P between the guest's calls) */
static inline int g5_overlaps(uint32_t a, uint32_t m) { return (uint32_t)(m - a + 3) < 19; }

/* 0xB1B6C0: culls a bounding sphere against a node's six planes. thiscall with the object unused: [esp+4] =
 * sphere P, [esp+8] = node (six planes of 16 bytes, then at +0x60 the mask of planes still to test), callee pops
 * both. al = 0 when the mask is empty. Otherwise, for each plane i whose bit is set in the mask (re-read each
 * time), it tests P against the plane (0x472580): a result of 1 clears bit i (the sphere is inside that plane, so
 * its children need not test it); any other result stops with al = 1. Having gone through all six, al = 0.
 * eax, ecx and edx end as the guest leaves them with the native 0x472580. Not written: the guest's register
 * saves and each call's return address and argument, all below the stack pointer once it returns. */
void f_B1B6C0_guest(CPU *c);
void f_B1B6C0(CPU *c) {
    uint32_t esp = c->r[4], P = M32(esp + 4), node = M32(esp + 8);
    uint32_t eax = c->r[0] & 0xFFFFFF00u, ecx = node, edx = c->r[2];
    if (TL32(node + 0x60) == 0) {
        c->r[0] = eax; c->r[1] = ecx; c->r[4] = esp + 12;
        return;
    }
    if (__builtin_expect(g5_overlaps(P, node + 0x60), 0)) { f_B1B6C0_guest(c); return; }
    uint32_t saved[4] = {c->r[3], c->r[5], c->r[6], c->r[7]};  /* the registers the guest pushes and pops */
    G5Cull s = {0};
    uint32_t edi = 0, esi = 1, ebx = node, ebp;
    for (;;) {
        ebp = TL32(ecx + 0x60);
        if (esi & ebp) {
            ecx = P;  /* the guest's ecx = [esp+0x14], its first argument */
            if (!g5_test(&s, P, ebx, &eax)) {
                g5_flush(c, &s);
                uint32_t sp = esp - 16;  /* below the four saved registers */
                M32(sp - 4) = ebx;
                M32(sp - 8) = 0xB1B6F1u;
                c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx;
                c->r[4] = sp - 8; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi;
                f_472580(c);
                eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3];
                ebp = c->r[5]; esi = c->r[6]; edi = c->r[7];
                esp = c->r[4] + 16;
            }
            if (eax != 1) break;
            ecx = node;  /* [esp+0x18], the second argument */
            eax = ~esi & ebp;
            TS32(ecx + 0x60, eax);
        }
        edi++;
        ebx += 0x10;
        esi = esi << 1 | esi >> 31;
        if ((int32_t)edi >= 6) break;
    }
    g5_flush(c, &s);
    c->r[0] = (eax & 0xFFFFFF00u) | (edi != 6);
    c->r[1] = ecx; c->r[2] = edx; c->r[3] = saved[0];
    c->r[4] = esp + 12; c->r[5] = saved[1]; c->r[6] = saved[2]; c->r[7] = saved[3];
}

/* 0xB07B90: the same culling for an object holding its own planes. thiscall: ecx = object (six planes of 16
 * bytes at +0x2C, the mask at +0x8C), [esp+4] = sphere P, callee pops it. al starts at 1; for each plane i in
 * order while al is still 1, if bit i of the mask is set it tests P against the plane (0x472580): al = (result !=
 * 2), and a result of 1 clears bit i (the mask is read again for that). Returns al; ecx is kept, eax above al is
 * the last result's (zero) or the caller's when nothing was tested. Same inline test and the same parts of the
 * stack left unwritten as 0xB1B6C0. */
void f_B07B90_guest(CPU *c);
void f_B07B90(CPU *c) {
    uint32_t esp = c->r[4], obj = c->r[1], P = M32(esp + 4);
    if (__builtin_expect(g5_overlaps(P, obj + 0x8C), 0)) { f_B07B90_guest(c); return; }
    uint32_t saved[4] = {c->r[3], c->r[5], c->r[6], c->r[7]};
    uint32_t eax = (c->r[0] & 0xFFFFFF00u) | 1, ecx = obj, edx = c->r[2];
    uint32_t ebx = 0, ebp = obj + 0x2C, esi = 1, edi = ebp;
    G5Cull s = {0};
    for (;;) {
        if (!(uint8_t)eax) break;
        if (TL32(ecx + 0x8C) & esi) {
            ecx = P;  /* [esp+0x18], the argument */
            if (!g5_test(&s, P, edi, &eax)) {
                g5_flush(c, &s);
                uint32_t sp = esp - 20;  /* below the five saved registers */
                M32(sp - 4) = edi;
                M32(sp - 8) = 0xB07BC1u;
                c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx;
                c->r[4] = sp - 8; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi;
                f_472580(c);
                eax = c->r[0]; edx = c->r[2]; ebx = c->r[3];
                ebp = c->r[5]; esi = c->r[6]; edi = c->r[7];
                esp = c->r[4] + 20;
            }
            ecx = eax;
            eax = (eax & 0xFFFFFF00u) | (ecx != 2);
            if (ecx == 1) {
                ecx = ~esi;
                TS32(ebp + 0x60, TL32(ebp + 0x60) & ecx);
            }
        }
        ebx++;
        edi += 0x10;
        esi = esi << 1 | esi >> 31;
        if (ebx >= 6) break;
        ecx = obj;  /* [esp+0x10], where the guest saved it */
    }
    g5_flush(c, &s);
    c->r[0] = eax; c->r[1] = obj; c->r[2] = edx; c->r[3] = saved[0];
    c->r[4] = esp + 8; c->r[5] = saved[1]; c->r[6] = saved[2]; c->r[7] = saved[3];
}
#endif
