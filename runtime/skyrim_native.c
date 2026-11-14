/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions.
 *
 * Each function here is listed in HOST_OVERRIDES_SKYRIM in tools/lift.py, which makes the lifter emit the
 * guest body as f_<addr>_guest instead of f_<addr>. A replacement must leave everything other code can observe
 * exactly as the lifted body does: registers, guest memory, the x87 registers it used (as stale values too),
 * the top index and the condition codes. tools/native_test.py runs both on the same random states and compares
 * all of that; add a function here only together with a passing run of it.
 *
 * Depends on x86.h only, so the test can compile this file on its own. */
#include "x86.h"

#pragma clang fp contract(off)  /* one rounding per operation, as the lifted body does */

#ifdef GAME_SKYRIM
/* 0x472580: which side of a plane a sphere is on. ecx = plane (normal x, y, z, then d), [esp+4] = sphere
 * (centre x, y, z, then radius), callee pops the argument. With dist = normal . centre - radius: returns 2
 * when dist <= -d, 1 when dist >= d, else 0 (also when either is NaN). Called from culling; about a quarter of
 * the main thread's lifted-code time in the world. The guest computes in x87 through two float temporaries in the argument slot; the same roundings are
 * kept here. */
void f_472580_guest(CPU *c);
void f_472580(CPU *c) {
    uint32_t esp = c->r[4], plane = c->r[1], sphere = M32(esp + 4);
    /* With two NaN operands the host picks which payload survives by operand order, which the compiler is free
     * to choose; NaN inputs never occur in culling, so they take the lifted body and match it exactly. */
    for (int k = 0; k < 4; k++)
        if (MF32(sphere + 4 * k) != MF32(sphere + 4 * k) || MF32(plane + 4 * k) != MF32(plane + 4 * k)) {
            f_472580_guest(c);
            return;
        }
    uint32_t t1 = (c->top - 1) & 7, t2 = (c->top - 2) & 7;
    double dist = (double)MF32(sphere + 4) * (double)MF32(plane + 4);
    dist += (double)MF32(sphere) * (double)MF32(plane);
    dist += (double)MF32(sphere + 8) * (double)MF32(plane + 8);
    MF32(esp + 4) = (float)dist;
    dist = (double)MF32(esp + 4) - (double)MF32(sphere + 12);
    MF32(esp + 4) = (float)dist;
    dist = (double)MF32(esp + 4);
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
    c->r[0] = result;
    c->r[4] = esp + 8;
}
#endif
