/* Native replacements, group 1: scene-graph transform math (see runtime/skyrim_native.c for the rules).
 *
 * These guest functions are small x87 routines that spend most of their lifted time on overhead rather than
 * arithmetic: calls into tiny helpers (each spilling and reloading the CPU state), temporaries bounced through
 * the guest stack, a rep movsd going through memmove, and x87 register write-backs. The natives below inline
 * those helpers and keep everything in host registers, doing the same double operations in the same order and
 * the same float roundings, and touching guest memory a caller can see in the same order (so overlapping
 * arguments behave the same). They are checked at the calling convention (tools/native_overrides/g1_abi.py):
 * what they leave in the callee's dead stack area below the return address differs (and 0xCDA260, which
 * returns nothing, leaves other stale values in ecx, edx and the x87 registers); everything else (eax, ecx, edx, the x87
 * registers and top, condition codes, the argument slots and all other memory) matches. Inputs with NaNs, and
 * other rare cases noted below, are handed to the guest code (f_<addr>_guest).
 *
 * Depends on x86.h only, so the test can compile this file on its own. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
void f_4EC9C0_guest(CPU *c);
void f_CDA170_guest(CPU *c);

/* Guest memory through a local copy of the base: the runtime's MEM is a writable global that every guest store
 * could alias, which would make the compiler reload it after each one. */
#define G32(a) (*(u32u *)(mb + (uint32_t)(a)))
#define GF(a) (*(f32u *)(mb + (uint32_t)(a)))
#define D(a) ((double)GF(a))

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* NaN inputs go to the guest code. The lifted guest computes in host doubles, where an operation on two NaNs
 * returns one of them, and which one depends on the operand order the compiler picked when it compiled the guest
 * (addition and multiplication commute for it). Without NaNs among the inputs, every NaN that arises is the
 * host's default NaN and the order doesn't matter. The default NaN itself (0x7FC00000 as a float) is a harmless
 * input as long as it isn't negated: nan_in lets it through, nan_any doesn't. */
static inline uint32_t nan_in(const uint8_t *mb, uint32_t a, unsigned n) {
    uint32_t bad = 0;
    for (unsigned k = 0; k < n; k++) {
        uint32_t u; memcpy(&u, mb + (uint32_t)(a + 4 * k), 4);
        bad |= ((u & 0x7FFFFFFFu) > 0x7F800000u) & (u != 0x7FC00000u);
    }
    return bad;
}
static inline uint32_t nan_any(const uint8_t *mb, uint32_t a, unsigned n) {
    uint32_t bad = 0;
    for (unsigned k = 0; k < n; k++) { uint32_t u; memcpy(&u, mb + (uint32_t)(a + 4 * k), 4); bad |= (u & 0x7FFFFFFFu) > 0x7F800000u; }
    return bad;
}
/* nan_in for a whole transform (13 floats), four at a time where NEON is there */
#if defined(__aarch64__)
#include <arm_neon.h>
static inline uint32_t nan_xf(const uint8_t *mb, uint32_t a) {
    const uint8_t *p = mb + a;  /* byte loads: guest pointers need not be aligned */
    uint32x4_t m = vdupq_n_u32(0x7FFFFFFFu), inf = vdupq_n_u32(0x7F800000u), dn = vdupq_n_u32(0x7FC00000u), bad;
    uint32x4_t u0 = vreinterpretq_u32_u8(vld1q_u8(p)), u1 = vreinterpretq_u32_u8(vld1q_u8(p + 16));
    uint32x4_t u2 = vreinterpretq_u32_u8(vld1q_u8(p + 32)), u3 = vreinterpretq_u32_u8(vld1q_u8(p + 36));
    bad = vbicq_u32(vcgtq_u32(vandq_u32(u0, m), inf), vceqq_u32(u0, dn));
    bad = vorrq_u32(bad, vbicq_u32(vcgtq_u32(vandq_u32(u1, m), inf), vceqq_u32(u1, dn)));
    bad = vorrq_u32(bad, vbicq_u32(vcgtq_u32(vandq_u32(u2, m), inf), vceqq_u32(u2, dn)));
    bad = vorrq_u32(bad, vbicq_u32(vcgtq_u32(vandq_u32(u3, m), inf), vceqq_u32(u3, dn)));
    return vmaxvq_u32(bad);
}
#else
static inline uint32_t nan_xf(const uint8_t *mb, uint32_t a) { return nan_in(mb, a, 13); }
#endif

/* 0x4EC9C0: NiTransform composition, out = A * B (thiscall: ecx = A, [esp+4] = out, [esp+8] = B; returns out
 * in eax; callee pops 8). A transform is a 3x3 rotation (row-major floats at +0), translation (+0x24) and
 * uniform scale (+0x30): out.scale = A.scale * B.scale, out.rot = A.rot * B.rot (the guest helper 0x447ED0,
 * through a stack temporary), out.trans = A.trans + A.scale * (A.rot * B.trans) (helper 0x474CD0, using A as
 * it is after out.rot was written, as the guest does when out is A). The guest leaves the translation's x and
 * y in its two argument slots. Called for every node of every scene-graph update, also from 0xCDA260 per bone. */
void f_4EC9C0(CPU *c) {
    uint8_t *mb = MEM;
    uint32_t E = c->r[4], A = c->r[1], top = c->top;
    uint32_t out = G32(E + 4), B = G32(E + 8);
    /* rep movsd backwards (never in practice), or NaNs */
    if (__builtin_expect(c->df | nan_xf(mb, A) | nan_xf(mb, B), 0)) { f_4EC9C0_guest(c); return; }
    GF(out + 0x30) = (float)(D(B + 0x30) * D(A + 0x30));
    /* 0x447ED0(ecx = A, temp, B): all reads before the copy of the temporary into out */
    double a0 = D(A), a1 = D(A + 4), a2 = D(A + 8), a3 = D(A + 0xC), a4 = D(A + 0x10), a5 = D(A + 0x14);
    double a6 = D(A + 0x18), a7 = D(A + 0x1C), a8 = D(A + 0x20);
    double b0 = D(B), b1 = D(B + 4), b2 = D(B + 8), b3 = D(B + 0xC), b4 = D(B + 0x10), b5 = D(B + 0x14);
    double b6 = D(B + 0x18), b7 = D(B + 0x1C), b8 = D(B + 0x20);
    float r0 = (float)((b0 * a0 + b3 * a1) + b6 * a2);
    float r3 = (float)((a4 * b3 + a3 * b0) + b6 * a5);
    float r6 = (float)((a7 * b3 + a6 * b0) + b6 * a8);
    float r1 = (float)((b1 * a0 + b4 * a1) + a2 * b7);
    float r4 = (float)((b4 * a4 + b1 * a3) + b7 * a5);
    float r7 = (float)((b4 * a7 + a6 * b1) + b7 * a8);
    float r2 = (float)((b2 * a0 + b5 * a1) + a2 * b8);
    float r5 = (float)((b5 * a4 + b2 * a3) + b8 * a5);
    float r8 = (float)((b5 * a7 + a6 * b2) + b8 * a8);
    GF(out) = r0; GF(out + 4) = r1; GF(out + 8) = r2; GF(out + 0xC) = r3; GF(out + 0x10) = r4;
    GF(out + 0x14) = r5; GF(out + 0x18) = r6; GF(out + 0x1C) = r7; GF(out + 0x20) = r8;
    /* 0x474CD0(ecx = A, temp, B + 0x24): A re-read, out may be A */
    uint32_t v = B + 0x24;
    float x = (float)((D(A + 4) * D(v + 4) + D(v) * D(A)) + D(A + 8) * D(v + 8));
    float y = (float)((D(A + 0xC) * D(v) + D(A + 0x10) * D(v + 4)) + D(A + 0x14) * D(v + 8));
    float z = (float)((D(A + 0x18) * D(v) + D(A + 0x1C) * D(v + 4)) + D(A + 0x20) * D(v + 8));
    G32(E + 8) = G32(A + 0x30);
    double s = D(E + 8);
    GF(E + 8) = (float)((double)x * s);
    double ys = (double)y * s;
    GF(E + 4) = (float)ys;
    float zs = (float)(s * (double)z);
    float tx = (float)(D(A + 0x24) + D(E + 8));
    GF(E + 8) = tx;
    float ty = (float)(D(A + 0x28) + D(E + 4));
    GF(E + 4) = ty;
    double tz = D(A + 0x2C) + (double)zs;
    GF(out + 0x28) = ty;
    GF(out + 0x24) = tx;
    float tzf = (float)tz;
    GF(out + 0x2C) = tzf;
    c->st[(top + 5) & 7] = s;
    c->st[(top + 6) & 7] = ys;
    c->st[(top + 7) & 7] = tz;
    c->r[0] = out;
    c->r[1] = fbits(tzf);
    c->r[2] = fbits(tx);
    c->r[4] = E + 12;
}

/* 0xCDA170: per-frame camera/shader transform setup (thiscall: ecx = object, [esp+4] = transform T; callee
 * pops 8, the second argument is unused). Inlines 0xC8EB60, which writes the 4x4 matrix at +0x690: T's
 * rotation scaled by T's scale, transposed into rows with a zero fourth column, translation minus the global
 * world origin at 0x1B913FC, and 1.0. Then with M = T.rot * T.scale (on the stack), stores M^T * v for
 * v = +0x3D8 into +0x3F0 and for v = +0x3E4 into +0x3FC (0x486C20: transpose 0x4488B0, then 0x474CD0).
 * Leaves T's scale in its argument slot, as the guest does. */
void f_CDA170(CPU *c) {
    uint8_t *mb = MEM;
    uint32_t E = c->r[4], th = c->r[1], top = c->top;
    uint32_t T = G32(E + 4);
    if (__builtin_expect(nan_xf(mb, T) | nan_in(mb, 0x1B913FCu, 3) | nan_in(mb, th + 0x3D8, 6), 0)) {
        f_CDA170_guest(c);
        return;
    }
    /* 0xC8EB60(o = th + 0x690, R = T, T + 0x24, scale as a float), reads and writes interleaved as there */
    uint32_t o = th + 0x690;
    double s = (double)(float)D(T + 0x30);
    GF(o) = (float)(D(T) * s);
    GF(o + 4) = (float)(D(T + 0xC) * s);
    double r6 = D(T + 0x18);
    G32(o + 0xC) = 0;
    GF(o + 8) = (float)(r6 * s);
    GF(o + 0x10) = (float)(D(T + 4) * s);
    GF(o + 0x14) = (float)(D(T + 0x10) * s);
    double r7 = D(T + 0x1C);
    G32(o + 0x1C) = 0;
    GF(o + 0x18) = (float)(r7 * s);
    GF(o + 0x20) = (float)(D(T + 8) * s);
    GF(o + 0x24) = (float)(D(T + 0x14) * s);
    double r8 = D(T + 0x20);
    G32(o + 0x2C) = 0;
    GF(o + 0x28) = (float)(s * r8);
    GF(o + 0x30) = (float)(D(T + 0x24) - D(0x1B913FCu));
    GF(o + 0x34) = (float)(D(T + 0x28) - D(0x1B91400u));
    double tz = D(T + 0x2C) - D(0x1B91404u);
    G32(o + 0x3C) = 0x3F800000u;
    GF(o + 0x38) = (float)tz;
    /* M = T.rot * scale, with the scale read back from the argument slot it was stored to */
    GF(E + 4) = (float)D(T + 0x30);
    double t0 = D(T), s2 = D(E + 4);
    float m0 = (float)(t0 * s2), m1 = (float)(D(T + 4) * s2), m2 = (float)(D(T + 8) * s2);
    float m3 = (float)(D(T + 0xC) * s2), m4 = (float)(D(T + 0x10) * s2), m5 = (float)(D(T + 0x14) * s2);
    float m6 = (float)(D(T + 0x18) * s2);
    float m7 = (float)(D(T + 0x1C) * s2), m8 = (float)(s2 * D(T + 0x20));
    /* 0x474CD0 on the transpose [m0 m3 m6; m1 m4 m7; m2 m5 m8] */
    double d0 = m0, d1 = m3, d2 = m6, d3 = m1, d4 = m4, d5 = m7, d6 = m2, d7 = m5, d8 = m8;
    uint32_t v = th + 0x3D8;
    GF(th + 0x3F0) = (float)((d1 * D(v + 4) + D(v) * d0) + d2 * D(v + 8));
    GF(th + 0x3F4) = (float)((d3 * D(v) + d4 * D(v + 4)) + d5 * D(v + 8));
    GF(th + 0x3F8) = (float)((d6 * D(v) + d7 * D(v + 4)) + d8 * D(v + 8));
    v = th + 0x3E4;
    float x = (float)((d1 * D(v + 4) + D(v) * d0) + d2 * D(v + 8));
    float y = (float)((d3 * D(v) + d4 * D(v + 4)) + d5 * D(v + 8));
    double last = d8 * D(v + 8);
    float z = (float)((d6 * D(v) + d7 * D(v + 4)) + last);
    GF(th + 0x3FC) = x;
    GF(th + 0x400) = y;
    GF(th + 0x404) = z;
    c->st[(top + 5) & 7] = s2;
    c->st[(top + 6) & 7] = last;
    c->st[(top + 7) & 7] = (double)z;
    c->r[0] = E - 0x30;  /* the guest returns its stack temporary's address */
    c->r[1] = fbits(z);
    c->r[2] = fbits(y);
    c->r[4] = E + 12;
}
/* ---- 0xCDA260 and the pure transform helpers it inlines ---- */
void f_CDA260_guest(CPU *c);

typedef struct { float r[9], t[3], s; } Xf;  /* NiTransform as laid out in guest memory: rotation, translation, scale */

static inline int ov(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) { return a < b + blen && b < a + alen; }

static inline void xf_load(const uint8_t *mb, uint32_t a, Xf *x) { memcpy(x, mb + a, sizeof *x); }

/* 0x4EC9C0's arithmetic (with 0x447ED0 and 0x474CD0) for a result that overlaps neither input */
static inline Xf xf_mul(const Xf *A, const Xf *B) {
    Xf o;
    double a0 = A->r[0], a1 = A->r[1], a2 = A->r[2], a3 = A->r[3], a4 = A->r[4], a5 = A->r[5];
    double a6 = A->r[6], a7 = A->r[7], a8 = A->r[8];
    double b0 = B->r[0], b1 = B->r[1], b2 = B->r[2], b3 = B->r[3], b4 = B->r[4], b5 = B->r[5];
    double b6 = B->r[6], b7 = B->r[7], b8 = B->r[8];
    o.s = (float)((double)B->s * (double)A->s);
    o.r[0] = (float)((b0 * a0 + b3 * a1) + b6 * a2);
    o.r[3] = (float)((a4 * b3 + a3 * b0) + b6 * a5);
    o.r[6] = (float)((a7 * b3 + a6 * b0) + b6 * a8);
    o.r[1] = (float)((b1 * a0 + b4 * a1) + a2 * b7);
    o.r[4] = (float)((b4 * a4 + b1 * a3) + b7 * a5);
    o.r[7] = (float)((b4 * a7 + a6 * b1) + b7 * a8);
    o.r[2] = (float)((b2 * a0 + b5 * a1) + a2 * b8);
    o.r[5] = (float)((b5 * a4 + b2 * a3) + b8 * a5);
    o.r[8] = (float)((b5 * a7 + a6 * b2) + b8 * a8);
    double v0 = B->t[0], v1 = B->t[1], v2 = B->t[2], s = A->s;
    float x = (float)((a1 * v1 + v0 * a0) + a2 * v2);
    float y = (float)((a3 * v0 + a4 * v1) + a5 * v2);
    float z = (float)((a6 * v0 + a7 * v1) + a8 * v2);
    o.t[0] = (float)((double)A->t[0] + (double)(float)((double)x * s));
    o.t[1] = (float)((double)A->t[1] + (double)(float)((double)y * s));
    o.t[2] = (float)((double)A->t[2] + (double)(float)(s * (double)z));
    return o;
}

/* 0x580F80: inverse of a transform (transposed rotation, 1/scale, -(R^T t) / scale) */
static inline Xf xf_inv(const Xf *N) {
    Xf o;
    o.r[0] = N->r[0]; o.r[1] = N->r[3]; o.r[2] = N->r[6];
    o.r[3] = N->r[1]; o.r[4] = N->r[4]; o.r[5] = N->r[7];
    o.r[6] = N->r[2]; o.r[7] = N->r[5]; o.r[8] = N->r[8];
    o.s = (float)(1.0 / (double)N->s);
    double v0 = (float)-(double)N->t[0], v1 = (float)-(double)N->t[1], v2 = (float)-(double)N->t[2];
    double a0 = o.r[0], a1 = o.r[1], a2 = o.r[2], a3 = o.r[3], a4 = o.r[4], a5 = o.r[5];
    double a6 = o.r[6], a7 = o.r[7], a8 = o.r[8], is = o.s;
    float x = (float)((a1 * v1 + v0 * a0) + a2 * v2);
    float y = (float)((a3 * v0 + a4 * v1) + a5 * v2);
    float z = (float)((a6 * v0 + a7 * v1) + a8 * v2);
    o.t[0] = (float)((double)x * is);
    o.t[1] = (float)((double)y * is);
    o.t[2] = (float)(is * (double)z);
    return o;
}

/* 0xC8EB60 for a transform held natively: 4x4 matrix with W.rot * s transposed into its rows, a zero fourth
 * column, the translation minus the world origin and 1.0 */
static inline void wr_C8EB60(uint8_t *mb, uint32_t o, const Xf *W) {
    double s = W->s;
    GF(o) = (float)((double)W->r[0] * s);
    GF(o + 4) = (float)((double)W->r[3] * s);
    G32(o + 0xC) = 0;
    GF(o + 8) = (float)((double)W->r[6] * s);
    GF(o + 0x10) = (float)((double)W->r[1] * s);
    GF(o + 0x14) = (float)((double)W->r[4] * s);
    G32(o + 0x1C) = 0;
    GF(o + 0x18) = (float)((double)W->r[7] * s);
    GF(o + 0x20) = (float)((double)W->r[2] * s);
    GF(o + 0x24) = (float)((double)W->r[5] * s);
    G32(o + 0x2C) = 0;
    GF(o + 0x28) = (float)(s * (double)W->r[8]);
    GF(o + 0x30) = (float)((double)W->t[0] - D(0x1B913FCu));
    GF(o + 0x34) = (float)((double)W->t[1] - D(0x1B91400u));
    double tz = (double)W->t[2] - D(0x1B91404u);
    G32(o + 0x3C) = 0x3F800000u;
    GF(o + 0x38) = (float)tz;
}

/* 0xCDBA70: the same matrix untransposed, translation in the fourth column (the fourth row is left alone) */
static inline void wr_CDBA70(uint8_t *mb, uint32_t o, const Xf *W) {
    double s = W->s;
    GF(o) = (float)((double)W->r[0] * s);
    GF(o + 4) = (float)((double)W->r[1] * s);
    GF(o + 8) = (float)((double)W->r[2] * s);
    GF(o + 0xC) = W->t[0];
    GF(o + 0x10) = (float)((double)W->r[3] * s);
    GF(o + 0x14) = (float)((double)W->r[4] * s);
    GF(o + 0x18) = (float)((double)W->r[5] * s);
    GF(o + 0x1C) = W->t[1];
    GF(o + 0x20) = (float)((double)W->r[6] * s);
    GF(o + 0x24) = (float)((double)W->r[7] * s);
    GF(o + 0x28) = (float)(s * (double)W->r[8]);
    GF(o + 0x2C) = W->t[2];
    GF(o + 0xC) = (float)((double)W->t[0] - D(0x1B913FCu));
    GF(o + 0x1C) = (float)((double)W->t[1] - D(0x1B91400u));
    GF(o + 0x2C) = (float)((double)W->t[2] - D(0x1B91404u));
    G32(o + 0x3C) = 0x3F800000u;
}

/* 0xCDA260: skinning palette (thiscall: ecx = renderer, [esp+4] = skin instance P, [esp+8] = transform X,
 * [esp+0xC] = byte flag, [esp+0x10] = matrix size in vec4 rows (3 or 4), [esp+0x14] = byte flag "no X";
 * callee pops 0x14). Once per frame and size (P+0x1C/P+0x24 against the frame counter at [[ecx+0x658]+0x3C]),
 * for each bone i with a world transform bones[i] (P+0x18): W = bones[i] * skinToBone[i] (the skin data P+8,
 * array +0x40 with stride 0x4C, count +0x44), then unless the "no X" flag W = (X * (skinData.root (+0xC) *
 * inverse(rootNode.world))) * W (root node P+0x10, world transform +0x54), and writes it to the palette buffer P+0x2C as a 3x4 row matrix (stride 0x30) or a 4x4 matrix
 * (stride 0x40, written by 0xC8EB60 or 0xCDBA70 after a one-time fill of the untouched elements). The intermediate
 * transforms only ever live in the guest's own stack frame, so they are kept natively; guest memory outside it is
 * read and written in the guest's order. A palette buffer that has to grow (a free and an allocation) goes to the
 * guest code. */
void f_CDA260(CPU *c) {
    if (__builtin_expect(c->df, 0)) { f_CDA260_guest(c); return; }  /* its callees' rep movsd backwards */
    uint8_t *mb = MEM;
    uint32_t E = c->r[4], th = c->r[1];
    uint32_t P = G32(E + 4), mode = G32(E + 0x10), clock = G32(th + 0x658);
    uint32_t frame = G32(clock + 0x3C);
    if (G32(P + 0x1C) == frame && mode == G32(P + 0x24)) {  /* palette already built this frame */
        c->r[0] = frame; c->r[1] = clock; c->r[4] = E + 0x18;
        return;
    }
    uint32_t sk = G32(P + 8), count = G32(sk + 0x44), buf = G32(P + 0x2C), bones = G32(P + 0x18);
    uint32_t data = G32(sk + 0x40), node = G32(P + 0x10), X = G32(E + 8);
    /* Pointers the guest reads after its first stores are read here before them: go to the guest code when a
     * store could change one (the palette buffer, P's frame, count and size, the flag argument), or when the
     * buffer has to grow (a free and an allocation in the guest), or a NaN is among the inputs. */
    uint32_t len = count * (mode == 4 ? 0x40 : 0x30), nb = count * 4;
    if (__builtin_expect(count > 0x10000 || (count * mode) << 4 > G32(P + 0x28) || ov(buf, len, P, 0x30) ||
                         ov(buf, len, sk + 0x40, 8) || ov(buf, len, bones, nb) || ov(buf, len, E, 0x18) ||
                         ov(P + 0x1C, 12, sk + 0x40, 8) || ov(P + 0x1C, 12, bones, nb) ||
                         ov(E + 0xC, 1, sk + 0x40, 8) || ov(E + 0xC, 1, bones, nb), 0)) {
        f_CDA260_guest(c);
        return;
    }
    uint32_t bad = nan_in(mb, node + 0x54, 9) | nan_any(mb, node + 0x54 + 0x24, 3) | nan_in(mb, node + 0x54 + 0x30, 1) |
                   nan_xf(mb, sk + 0xC) | nan_xf(mb, X) | nan_in(mb, 0x1B913FCu, 3);
    for (uint32_t i = 0, d = data; i < count; i++, d += 0x4C) {
        uint32_t bone = G32(bones + i * 4);
        if (bone) bad |= nan_xf(mb, bone) | nan_xf(mb, d);
    }
    if (__builtin_expect(bad, 0)) { f_CDA260_guest(c); return; }
    G32(P + 0x1C) = frame;
    if (mode == 3) mb[(uint32_t)(E + 0xC)] = 1;
    G32(P + 0x20) = count;
    G32(P + 0x24) = mode;
    uint8_t flag = mb[(uint32_t)(E + 0xC)], noX = mb[(uint32_t)(E + 0x14)];
    if (mode == 4)
        for (uint32_t k = 0, m = buf; k < count; k++, m += 0x40) {
            if (flag) { G32(m + 0x38) = 0; G32(m + 0x34) = 0; G32(m + 0x30) = 0; }
            else { G32(m + 0x2C) = 0; G32(m + 0x1C) = 0; G32(m + 0xC) = 0; }
            G32(m + 0x3C) = 0x3F800000u;
        }
    Xf N, R, Xt, L, W;
    xf_load(mb, node + 0x54, &N);
    L = xf_inv(&N);
    xf_load(mb, sk + 0xC, &R);  /* the guest reads P+8 again here */
    W = xf_mul(&R, &L);
    xf_load(mb, X, &Xt);
    L = xf_mul(&Xt, &W);
    for (uint32_t i = 0; i < count; i++, data += 0x4C) {
        uint32_t bone = G32(bones + i * 4);
        if (!bone) continue;
        Xf A, B;
        xf_load(mb, bone, &A);
        xf_load(mb, data, &B);
        W = xf_mul(&A, &B);
        if (!noX) W = xf_mul(&L, &W);
        if (mode != 4) {
            uint32_t o = buf + 0x30 * i;
            double s = W.s;
            GF(o) = (float)((double)W.r[0] * s);
            GF(o + 0x10) = (float)((double)W.r[3] * s);
            GF(o + 0x20) = (float)((double)W.r[6] * s);
            GF(o + 4) = (float)((double)W.r[1] * s);
            GF(o + 0x14) = (float)((double)W.r[4] * s);
            GF(o + 0x24) = (float)((double)W.r[7] * s);
            GF(o + 8) = (float)((double)W.r[2] * s);
            GF(o + 0x18) = (float)((double)W.r[5] * s);
            GF(o + 0x28) = (float)(s * (double)W.r[8]);
            GF(o + 0xC) = (float)((double)W.t[0] - D(0x1B913FCu));
            GF(o + 0x1C) = (float)((double)W.t[1] - D(0x1B91400u));
            GF(o + 0x2C) = (float)((double)W.t[2] - D(0x1B91404u));
        } else if (flag) wr_CDBA70(mb, buf + 0x40 * i, &W);
        else wr_C8EB60(mb, buf + 0x40 * i, &W);
    }
    c->r[0] = data;  /* the guest's last bone-data pointer */
    c->r[4] = E + 0x18;
}
#endif
