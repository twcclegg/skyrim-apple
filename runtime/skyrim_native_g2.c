/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 2 (vector and matrix math).
 *
 * Listed in tools/native_overrides/g2.txt; see runtime/skyrim_native.c for the rules. Each replacement computes
 * with the same double operations, in the same order and with the same float roundings as the lifted x87 code,
 * and hands inputs it does not handle to the lifted body (f_<addr>_guest), or, for the two that are checked
 * instruction for instruction (the harness has no lifted body to call there), to a copy of the guest's own
 * order. tools/native_test.py checks 0x447ED0 and 0x474CD0 instruction for instruction, the others at the
 * calling convention (tools/native_overrides/g2_abi.py), because they inline guest helpers they call.
 *
 * Arithmetic goes through MUL/ADD/SUB/DIV, which keep the guest's operand order: the compiler may otherwise swap
 * the operands of a commutative operation, and when both are NaNs the result's payload is the first one's.
 *
 * The runtime is not built with a constant guest base, so every guest store would make the compiler reload MEM;
 * each function reads it once into a local. Depends on x86.h only. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
#define G32(a) (*(u32u *)(m + (uint32_t)(a)))
#define G8(a) (*(uint8_t *)(m + (uint32_t)(a)))
#define GF(a) (*(f32u *)(m + (uint32_t)(a)))
#define GD(a) ((double)GF(a))
#define HD(p, o) ((double)*(const f32u *)((p) + (o)))  /* p: host pointer to an object known not to wrap */

#if defined(__aarch64__)
#define G2_OP(name, insn) \
    static inline double name(double a, double b) { double r; __asm__(insn " %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); return r; }
#else
#define G2_OP(name, insn) static inline double name(double a, double b) { return a OP_##name b; }
#define OP_MUL *
#define OP_ADD +
#define OP_SUB -
#define OP_DIV /
#endif
G2_OP(MUL, "fmul")
G2_OP(ADD, "fadd")
G2_OP(SUB, "fsub")
G2_OP(DIV, "fdiv")
/* x87 "fld x; fmul y; fld z; fmul w; faddp; fld u; fmul v; faddp": (x * y + z * w) + u * v */
#define DOT3(x, y, z, w, u, v) ADD(ADD(MUL(x, y), MUL(z, w)), MUL(u, v))

/* do [a, a + na) and [b, b + nb) overlap in the 32-bit guest address space? (a - b in (-na, nb), one compare) */
static inline int g2_overlap(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) {
    return (uint32_t)(a - b + na - 1) < na + nb - 1;
}
static inline uint32_t g2_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* 0x447ED0: 3x3 float matrix product, out = A * B (row-major). thiscall: ecx = A, [esp+4] = out, [esp+8] = B;
 * returns out in eax, edx = B, callee pops 8. The lifted body loads every element three times (each float store
 * to out may alias the inputs); when out overlaps neither input, each element is loaded and widened once here,
 * through one host pointer per matrix. Overlapping calls (in-place products) go through the guest's own order,
 * where a store to out feeds the loads after it. */
#define G2_MM(o, x1, y1, x2, y2, x3, y3)                                                         \
    t7 = MUL(GD(x1), GD(y1)); t6 = MUL(GD(x2), GD(y2)); t7 = ADD(t7, t6); t6 = MUL(GD(x3), GD(y3)); \
    t7 = ADD(t7, t6); GF(out + (o)) = (float)t7;
void f_447ED0(CPU *c) {
    uint8_t *m = MEM;
    uint32_t esp = c->r[4], a = c->r[1], out = G32(esp + 4), b = G32(esp + 8);
    double t6, t7;
    if (__builtin_expect(g2_overlap(out, 36, a, 36) || g2_overlap(out, 36, b, 36) || a > 0xFFFFFFDCu ||
                         b > 0xFFFFFFDCu, 0)) {
        G2_MM(0, b, a, b + 12, a + 4, b + 24, a + 8)
        G2_MM(12, a + 16, b + 12, a + 12, b, b + 24, a + 20)
        G2_MM(24, a + 28, b + 12, a + 24, b, b + 24, a + 32)
        G2_MM(4, b + 4, a, b + 16, a + 4, a + 8, b + 28)
        G2_MM(16, b + 16, a + 16, b + 4, a + 12, b + 28, a + 20)
        G2_MM(28, b + 16, a + 28, a + 24, b + 4, b + 28, a + 32)
        G2_MM(8, b + 8, a, b + 20, a + 4, a + 8, b + 32)
        G2_MM(20, b + 20, a + 16, b + 8, a + 12, b + 32, a + 20)
        G2_MM(32, b + 20, a + 28, a + 24, b + 8, b + 32, a + 32)
    } else {
        const uint8_t *pa = m + a, *pb = m + b;
        double a0 = HD(pa, 0), a1 = HD(pa, 4), a2 = HD(pa, 8), a3 = HD(pa, 12), a4 = HD(pa, 16);
        double a5 = HD(pa, 20), a6 = HD(pa, 24), a7 = HD(pa, 28), a8 = HD(pa, 32);
        double b0 = HD(pb, 0), b1 = HD(pb, 4), b2 = HD(pb, 8), b3 = HD(pb, 12), b4 = HD(pb, 16);
        double b5 = HD(pb, 20), b6 = HD(pb, 24), b7 = HD(pb, 28), b8 = HD(pb, 32);
        GF(out) = (float)DOT3(b0, a0, b3, a1, b6, a2);
        GF(out + 12) = (float)DOT3(a4, b3, a3, b0, b6, a5);
        GF(out + 24) = (float)DOT3(a7, b3, a6, b0, b6, a8);
        GF(out + 4) = (float)DOT3(b1, a0, b4, a1, a2, b7);
        GF(out + 16) = (float)DOT3(b4, a4, b1, a3, b7, a5);
        GF(out + 28) = (float)DOT3(b4, a7, a6, b1, b7, a8);
        GF(out + 8) = (float)DOT3(b2, a0, b5, a1, a2, b8);
        GF(out + 20) = (float)DOT3(b5, a4, b2, a3, b8, a5);
        t6 = MUL(b8, a8);
        t7 = ADD(ADD(MUL(b5, a7), MUL(a6, b2)), t6);
        GF(out + 32) = (float)t7;
    }
    c->st[(c->top + 6) & 7] = t6;  /* the x87 registers the guest used, as it leaves them */
    c->st[(c->top + 7) & 7] = t7;
    c->r[0] = out;
    c->r[2] = b;
    c->r[4] = esp + 12;
}

/* 0x474CD0: 3x3 float matrix times vector, out = M * v. thiscall: ecx = M, [esp+4] = out, [esp+8] = v; callee
 * pops 8. The guest rounds each component to float through stack temporaries ([esp-8], [esp-4] and the v
 * argument slot), copies them to out as integers and leaves ecx/edx holding the bits of z/y; all of that is
 * kept. The guest loads v once per row; here once, unless M, v or out overlap those temporaries (never in real
 * calls), which goes through the guest's own order. */
void f_474CD0(CPU *c) {
    uint8_t *m = MEM;
    uint32_t esp = c->r[4], mat = c->r[1], out = G32(esp + 4), v = G32(esp + 8);
    uint32_t xb, yb, zb;
    double t6, t7;
    if (__builtin_expect(g2_overlap(mat, 36, esp - 8, 20) || g2_overlap(v, 12, esp - 8, 20) ||
                         g2_overlap(out, 12, esp - 8, 20) || mat > 0xFFFFFFDCu || v > 0xFFFFFFF4u, 0)) {
        uint32_t s = esp - 8;  /* the guest's esp; each step as the lifted body orders it */
        t7 = MUL(GD(mat + 4), GD(v + 4)); t6 = MUL(GD(v), GD(mat)); t7 = ADD(t7, t6);
        t6 = MUL(GD(mat + 8), GD(v + 8)); t7 = ADD(t7, t6); GF(s + 0x10) = (float)t7;
        t7 = MUL(GD(mat + 12), GD(v)); t6 = MUL(GD(mat + 16), GD(v + 4)); t7 = ADD(t7, t6);
        t6 = MUL(GD(mat + 20), GD(v + 8)); t7 = ADD(t7, t6); GF(s) = (float)t7;
        t7 = GD(mat + 24); yb = G32(s); t7 = MUL(t7, GD(v)); t6 = MUL(GD(mat + 28), GD(v + 4)); t7 = ADD(t7, t6);
        t6 = GD(mat + 32); xb = G32(s + 0x10); t6 = MUL(t6, GD(v + 8)); out = G32(s + 0xC);
        G32(out) = xb; G32(out + 4) = yb;
        t7 = ADD(t7, t6); GF(s + 4) = (float)t7; zb = G32(s + 4); G32(out + 8) = zb;
    } else {
        const uint8_t *pm = m + mat, *pv = m + v;
        double v0 = HD(pv, 0), v1 = HD(pv, 4), v2 = HD(pv, 8);
        xb = g2_bits((float)DOT3(HD(pm, 4), v1, v0, HD(pm, 0), HD(pm, 8), v2));
        yb = g2_bits((float)DOT3(HD(pm, 12), v0, HD(pm, 16), v1, HD(pm, 20), v2));
        t6 = MUL(HD(pm, 32), v2);
        t7 = ADD(ADD(MUL(HD(pm, 24), v0), MUL(HD(pm, 28), v1)), t6);
        zb = g2_bits((float)t7);
        G32(esp + 8) = xb;
        G32(esp - 8) = yb;
        G32(esp - 4) = zb;
        G32(out) = xb;
        G32(out + 4) = yb;
        G32(out + 8) = zb;
    }
    c->st[(c->top + 6) & 7] = t6;
    c->st[(c->top + 7) & 7] = t7;
    c->r[0] = out;
    c->r[1] = zb;
    c->r[2] = yb;
    c->r[4] = esp + 12;
}

/* 0x420560: out = normalize(a x b), or (0, 0, 0) when the length is not above 1e-6 (the float at 0x1079CFC).
 * thiscall: ecx = a, [esp+4] = out, [esp+8] = b; returns out in eax, callee pops 8. The guest calls the vector
 * length (0x40A4B0), which calls the C runtime's sqrt (0xF52D84, with its control word checks); both are done
 * here with the same roundings: the cross product to float, the squared length to float, the root to float.
 * The CRT sqrt is a plain fsqrt when the control word is the default 0x27F and the argument is finite; anything
 * else takes the lifted body, as do vectors overlapping the guest's stack temporaries. Also kept: the float x
 * component left in the b argument slot. */
void f_420560_guest(CPU *c);
void f_420560(CPU *c) {
    uint8_t *m = MEM;
    uint32_t esp = c->r[4], a = c->r[1], out = G32(esp + 4), b = G32(esp + 8);
    double a0 = GD(a), a1 = GD(a + 4), a2 = GD(a + 8), b0 = GD(b), b1 = GD(b + 4), b2 = GD(b + 8);
    float x = (float)SUB(MUL(b2, a1), MUL(b1, a2));
    float y = (float)SUB(MUL(a2, b0), MUL(a0, b2));
    float z = (float)SUB(MUL(b1, a0), MUL(b0, a1));
    double xd = x, yd = y, zd = z;
    float sq = (float)ADD(ADD(MUL(xd, xd), MUL(yd, yd)), MUL(zd, zd));
    if (__builtin_expect(c->fpu_cw != 0x27F || (g2_bits(sq) & 0x7F800000u) == 0x7F800000u ||
                         g2_overlap(a, 12, esp - 0x80, 0x8C) || g2_overlap(b, 12, esp - 0x80, 0x8C) ||
                         g2_overlap(out, 12, esp - 0x80, 0x8C), 0)) {
        f_420560_guest(c);
        return;
    }
    double len = (double)(float)sqrt((double)sq);
    uint32_t ox = 0, oy = 0, oz = 0;
    if ((double)GF(0x1079CFCu) < len) {
        double inv = (double)(float)DIV(1.0, len);
        x = (float)MUL(inv, xd);
        ox = g2_bits(x);
        oy = g2_bits((float)MUL(inv, yd));
        oz = g2_bits((float)MUL(inv, zd));
    }
    GF(esp + 8) = x;
    G32(out) = ox;
    G32(out + 4) = oy;
    G32(out + 8) = oz;
    c->r[0] = out;
    c->r[4] = esp + 12;
}

/* 0xAFC100: box against frustum planes; false when the box is entirely outside one enabled plane. thiscall:
 * ecx = box (centre x, y, z at +0x10, half extents at +0x20), [esp+4] = planes (6 x (nx, ny, nz, d), enabled
 * mask at +0x60); returns a bool in al, callee pops 4. Per enabled plane: outside when
 * d > (float)(centre . n) - (float)(extent . |n|). The guest gets |n| from three calls to the C runtime's fabs
 * (0xF531CC) per plane; here the sign bit is cleared (exact: a float's |x| widened and rounded back). A
 * non-finite normal component, which fabs handles through its error paths, takes the lifted body, as do a box
 * or planes overlapping the guest's stack frame. */
void f_AFC100_guest(CPU *c);
void f_AFC100(CPU *c) {
    uint8_t *m = MEM;
    uint32_t esp = c->r[4], box = c->r[1], pl = G32(esp + 4), mask = G32(pl + 0x60);
    if (__builtin_expect(g2_overlap(box, 0x2C, esp - 0x100, 0x100) || g2_overlap(pl, 0x64, esp - 0x100, 0x100), 0)) {
        f_AFC100_guest(c);
        return;
    }
    double c0 = GD(box + 0x10), c1 = GD(box + 0x14), c2 = GD(box + 0x18);
    double e0 = GD(box + 0x20), e1 = GD(box + 0x24), e2 = GD(box + 0x28);
    uint32_t result = 1;
    for (uint32_t i = 0; i < 6; i++, pl += 16) {
        if (!(mask & (1u << i)))
            continue;
        uint32_t bx = G32(pl), by = G32(pl + 4), bz = G32(pl + 8);
        if (__builtin_expect((bx & 0x7F800000u) == 0x7F800000u || (by & 0x7F800000u) == 0x7F800000u ||
                             (bz & 0x7F800000u) == 0x7F800000u, 0)) {
            f_AFC100_guest(c);
            return;
        }
        double nx = GD(pl), ny = GD(pl + 4), nz = GD(pl + 8);
        uint32_t ux = bx & 0x7FFFFFFFu, uy = by & 0x7FFFFFFFu, uz = bz & 0x7FFFFFFFu;
        float fx, fy, fz;
        memcpy(&fx, &ux, 4);
        memcpy(&fy, &uy, 4);
        memcpy(&fz, &uz, 4);
        float s1 = (float)DOT3(c1, ny, nx, c0, c2, nz);
        float s2 = (float)DOT3(e1, (double)fy, e0, (double)fx, e2, (double)fz);
        if (GD(pl + 12) > SUB((double)s1, (double)s2)) {
            result = 0;
            break;
        }
    }
    c->r[0] = (c->r[0] & 0xFFFFFF00u) | result;  /* the rest of eax is not part of the result */
    c->r[4] = esp + 8;
}

/* 0xAFBF80: the other box/frustum test of the same class; false when all eight corners of the box are strictly
 * outside one enabled plane. thiscall: ecx = box, [esp+4] = planes (as 0xAFC100); bool in al, callee pops 4.
 * The guest builds the corners with a call (0xAFBB70: centre +- extent, rounded to float) into a stack array,
 * then for each enabled plane rounds (corner . n - d) to float and compares it with 0. A box with zero z
 * extent only checks the four corners with z = centre - extent. That flag is stored in the low byte of the
 * planes argument slot, as the guest does. A box or planes overlapping the guest's stack frame or that slot
 * take the lifted body. */
void f_AFBF80_guest(CPU *c);
void f_AFBF80(CPU *c) {
    uint8_t *m = MEM;
    uint32_t esp = c->r[4], box = c->r[1], pl = G32(esp + 4), mask = G32(pl + 0x60);
    if (__builtin_expect(g2_overlap(box, 0x2C, esp - 0x100, 0x108) || g2_overlap(pl, 0x64, esp - 0x100, 0x108), 0)) {
        f_AFBF80_guest(c);
        return;
    }
    c->r[4] = esp + 8;
    if (!mask) {
        c->r[0] = (c->r[0] & 0xFFFFFF00u) | 1;
        return;
    }
    double c0 = GD(box + 0x10), c1 = GD(box + 0x14), c2 = GD(box + 0x18);
    double e0 = GD(box + 0x20), e1 = GD(box + 0x24), e2 = GD(box + 0x28);
    uint32_t flat = e2 == 0.0;
    G8(esp + 4) = (uint8_t)flat;
    double xs[2] = {(double)(float)ADD(e0, c0), (double)(float)SUB(c0, e0)};
    double ys[2] = {(double)(float)ADD(e1, c1), (double)(float)SUB(c1, e1)};
    double zs[2] = {(double)(float)ADD(e2, c2), (double)(float)SUB(c2, e2)};
    uint32_t result = 1;
    for (uint32_t i = 0; i < 6 && result; i++, pl += 16) {
        if (!(mask & (1u << i)))
            continue;
        double nx = GD(pl), ny = GD(pl + 4), nz = GD(pl + 8), d = GD(pl + 12);
        uint32_t outside = 1;
        for (uint32_t k = flat; k < 8; k += 1 + flat) {  /* corner k: x by bit 2, y by bit 1, z by bit 0 */
            float dist = (float)SUB(DOT3(xs[k >> 2], nx, ys[(k >> 1) & 1], ny, zs[k & 1], nz), d);
            if (!((double)dist < 0.0)) {
                outside = 0;
                break;
            }
        }
        if (outside)
            result = 0;
    }
    c->r[0] = (c->r[0] & 0xFFFFFF00u) | result;
}
#endif
