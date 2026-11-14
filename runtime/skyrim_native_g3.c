/* Native replacements for hot Skyrim guest functions, group 3 (listed in tools/native_overrides/g3.txt).
 *
 * Same rules as runtime/skyrim_native.c: each one must leave what other code can observe exactly as the lifted
 * guest does, checked by tools/native_test.py (instruction-exact, or at the calling convention for the entries in
 * tools/native_overrides/g3_abi.py). The arithmetic repeats the guest's x87 operations as doubles, in the same
 * order and with the same operand order (NaN propagation), with the same roundings to float where the guest stores
 * to a float temporary.
 *
 * Guest data the guest moved with integer instructions is read here with plain loads, as the guest's own float
 * loads and stores already are: none of it is a flag another thread waits on.
 *
 * Depends on x86.h only, so the test can compile this file on its own. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
void f_C8EB60_guest(CPU *c);
void f_40A4B0_guest(CPU *c);
void f_ABED70_guest(CPU *c);

static inline float g3_f32(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static inline uint32_t g3_bits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
/* do [a, a + la) and [b, b + lb) share a byte (guest addresses, modulo 2^32) */
static inline int g3_overlap(uint32_t a, uint32_t la, uint32_t b, uint32_t lb) {
    return (uint32_t)(a - b) < lb || (uint32_t)(b - a) < la;
}

/* 0xC8EB60 (cdecl): 4x4 world matrix for shader constants from a 3x3 rotation, a translation and a scale.
 * [esp+4] = out (16 floats), [esp+8] = rot (9 floats, row-major), [esp+12] = translation (3 floats),
 * [esp+16] = scale. out holds the transposed rotation times scale (out[4i+j] = rot[3j+i] * scale), w = 0, and the
 * translation minus the camera-relative origin at 0x1B913FC in the last row, w = 1. The guest moves each rotation
 * element through its rot argument slot with integer moves, pushes esi, and leaves eax = out, ecx = translation,
 * edx = 0, the last rotation element in the rot slot and three x87 registers below the top holding scale, the
 * last stored product and the last translation difference.
 *
 * Without overlap (always, in the game) everything is loaded first and stored after; if out overlaps an input or
 * the argument area, or an input overlaps the slots the guest writes, the lifted body runs instead. So does a NaN
 * scale: a product of two NaNs keeps one of the two payloads, and which one depends on the operand order the
 * compiler picks for a multiplication it treats as commutative, in the lifted body as here. */
void f_C8EB60(CPU *c) {
    uint32_t esp = c->r[4], out = M32(esp + 4), rot = M32(esp + 8), tr = M32(esp + 12);
    float scale = MF32(esp + 16);
    if (scale != scale || g3_overlap(out, 0x40, rot, 0x24) || g3_overlap(out, 0x40, tr, 0xC) ||
        g3_overlap(out, 0x40, esp - 4, 0x18) || g3_overlap(out, 0x40, 0x1B913FCu, 0xC) ||
        g3_overlap(rot, 0x24, esp - 4, 0x10) || g3_overlap(tr, 0xC, esp - 4, 0x10) ||
        g3_overlap(esp - 4, 0x10, 0x1B913FCu, 0xC)) {
        f_C8EB60_guest(c);
        return;
    }
    uint32_t t1 = (c->top - 1) & 7, t2 = (c->top - 2) & 7, t3 = (c->top - 3) & 7;
    double s = (double)scale;
    uint32_t rb[9];
    float r[9];
    for (int k = 0; k < 9; k++) { rb[k] = M32(rot + 4 * k); r[k] = g3_f32(rb[k]); }
    float tx = MF32(tr), ty = MF32(tr + 4), tz = MF32(tr + 8);
    float ox = MF32(0x1B913FCu), oy = MF32(0x1B91400u), oz = MF32(0x1B91404u);
    double p5 = (double)r[5] * s;
    double w = (double)tz - (double)oz;
    MF32(out + 0x00) = (float)((double)r[0] * s);
    MF32(out + 0x04) = (float)((double)r[3] * s);
    MF32(out + 0x08) = (float)((double)r[6] * s);
    M32(out + 0x0C) = 0;
    MF32(out + 0x10) = (float)((double)r[1] * s);
    MF32(out + 0x14) = (float)((double)r[4] * s);
    MF32(out + 0x18) = (float)((double)r[7] * s);
    M32(out + 0x1C) = 0;
    MF32(out + 0x20) = (float)((double)r[2] * s);
    MF32(out + 0x24) = (float)p5;
    MF32(out + 0x28) = (float)(s * (double)r[8]);
    M32(out + 0x2C) = 0;
    MF32(out + 0x30) = (float)((double)tx - (double)ox);
    MF32(out + 0x34) = (float)((double)ty - (double)oy);
    MF32(out + 0x38) = (float)w;
    M32(out + 0x3C) = 0x3F800000u;
    M32(esp - 4) = c->r[6];
    M32(esp + 8) = rb[8];
    c->st[t3] = s; c->st[t2] = p5; c->st[t1] = w;
    c->r[0] = out; c->r[1] = tr; c->r[2] = 0; c->r[4] = esp + 4;
}

/* Length of the float vector at ecx with the guest's roundings: the sum of squares (doubles of the floats) is
 * rounded to float, its double square root (the CRT's sqrt at 0xF52D84, called with the value on the stack and
 * in ST(0)) is rounded to float again. Returns false, computing nothing, when the guest's sqrt would take its
 * special-value paths (infinity or NaN, or a negative input, which a sum of squares can't be). */
static inline int g3_length(float x, float y, float z, float *len) {
    double sum = (double)x * (double)x + (double)y * (double)y;
    sum = sum + (double)z * (double)z;
    double v = (double)(float)sum;
    uint64_t b; memcpy(&b, &v, 8);
    uint32_t hi = (uint32_t)(b >> 32);
    if ((hi & 0x7FF00000u) == 0x7FF00000u || (hi & 0x80000000u)) return 0;
    *len = (float)sqrt(v);
    return 1;
}

/* 0x40A4B0 (thiscall, no arguments): length of the vector at ecx, returned in ST(0) as a float value (and its
 * bits in eax). The guest calls the CRT sqrt, which saves, maybe changes and restores the x87 control word and
 * otherwise only touches the stack below esp; it is done inline here. Infinite or NaN sums go to the guest. */
void f_40A4B0(CPU *c) {
    uint32_t ecx = c->r[1];
    float len;
    if (!g3_length(MF32(ecx), MF32(ecx + 4), MF32(ecx + 8), &len)) { f_40A4B0_guest(c); return; }
    c->r[0] = g3_bits(len);
    c->top = (c->top - 1) & 7;
    c->st[c->top] = (double)len;
    c->r[4] += 4;
}

/* 0xABED70 (thiscall, callee pops 12): NiPlane from three points. ecx = plane (normal x, y, z, constant),
 * [esp+4] = p0, [esp+8] = p1, [esp+12] = p2. normal = unit cross product (0x420560, which uses 0x40A4B0) of
 * (p1 - p0) and (p2 - p1), each difference rounded to float; zero when the cross product's length is not above
 * 1e-6. constant = normal . p0. Returns the plane in eax. The guest writes the normal's z through the p1 argument
 * slot, and leaves the x87 condition codes of its length-against-1e-6 compare. Inlined here with every float
 * rounding the three guest functions make; the normal is written, read back and combined with p0 in the guest's
 * order, so a plane that overlaps p0 comes out the same. Infinite or NaN cross products go to the guest. */
void f_ABED70(CPU *c) {
    uint32_t esp = c->r[4], out = c->r[1], p0 = M32(esp + 4), p1 = M32(esp + 8), p2 = M32(esp + 12);
    float ax = (float)((double)MF32(p1) - (double)MF32(p0));
    float ay = (float)((double)MF32(p1 + 4) - (double)MF32(p0 + 4));
    float az = (float)((double)MF32(p1 + 8) - (double)MF32(p0 + 8));
    float bx = (float)((double)MF32(p2) - (double)MF32(p1));
    float by = (float)((double)MF32(p2 + 4) - (double)MF32(p1 + 4));
    float bz = (float)((double)MF32(p2 + 8) - (double)MF32(p1 + 8));
    /* 0x420560 with this = a, argument = b */
    float rx = (float)((double)bz * (double)ay - (double)by * (double)az);
    float ry = (float)((double)az * (double)bx - (double)ax * (double)bz);
    float rz = (float)((double)by * (double)ax - (double)bx * (double)ay);
    float len;
    if (!g3_length(rx, ry, rz, &len)) { f_ABED70_guest(c); return; }
    double l = (double)len, eps = (double)MF32(0x1079CFCu);
    float nx, ny, nz;
    if (l != l || eps != eps) { c->c0 = c->c2 = c->c3 = 1; }
    else { c->c0 = eps < l; c->c2 = 0; c->c3 = eps == l; }
    if (eps < l) {
        double inv = (double)(float)(1.0 / l);
        nx = (float)(inv * (double)rx);
        ny = (float)(inv * (double)ry);
        nz = (float)(inv * (double)rz);
    } else {
        nx = ny = nz = 0.0f;
    }
    /* back in 0xABED70 */
    MF32(out) = nx;
    MF32(out + 4) = ny;
    MF32(esp + 8) = nz;
    double z = (double)MF32(esp + 8);
    MF32(out + 8) = (float)z;
    double d = (double)MF32(p0 + 4) * (double)MF32(out + 4);
    d = d + (double)MF32(p0) * (double)MF32(out);
    double e = z * (double)MF32(p0 + 8);
    MF32(out + 12) = (float)(e + d);
    c->r[0] = out;
    c->r[4] = esp + 16;
}
#endif
