/* Native replacements for hot Skyrim (TESV.exe 1.9.32.0.8) guest functions, group 4 (see skyrim_native.c for the
 * rules; the addresses are listed in tools/native_overrides/g4.txt, calling-convention test setups in
 * tools/native_overrides/g4_abi.py). Depends on x86.h only. */
#pragma clang fp contract(off)
#include "x86.h"

#ifdef GAME_SKYRIM
/* 0xAE4CB0: quicksort of an array of 16-bit indices by the float each one selects, largest first. thiscall:
 * ecx = object (+0x10: uint16 index array, +0x20: float key array), [esp+4] = first, [esp+8] = last (signed, both
 * inclusive), callee pops both. The guest partitions around the first element's key with a hole (Hoare style: the
 * right scan skips keys <= pivot, the left scan keys >= pivot, an unordered compare stops either scan), stores the
 * pivot index in the final hole, recurses into the left part and loops on the right part.
 *
 * Kept exactly: every index-array access in the guest's order (the array and key pointers are re-read from the
 * object where the guest re-reads them), eax / ecx / edx as the guest leaves them, the x87 registers it leaves
 * below the top (pivot, pivot copy, last key compared), C0/C2/C3 of the last compare, and the pivot float the guest
 * keeps in its first argument slot. Not written: the guest's register saves and locals below the stack pointer,
 * including those of its recursive calls, which are dead once it returns. The recursion is a host recursion with
 * the guest's shape (left part recursive, right part iterative), so its depth is the guest's. */
typedef struct { uint32_t eax, edx, p, pv; } AE4Out;

static void ae4cb0_sort(CPU *c, uint32_t self, int32_t lo, int32_t hi, uint32_t top, AE4Out *out) {
    uint32_t eax, edx, p, pv_bits;
    for (;;) {
        uint32_t idx = M32(self + 0x10);
        p = M16(idx + (uint32_t)lo * 2);
        uint32_t keys = M32(self + 0x20);
        eax = keys;
        int32_t first = lo, esi = lo, edi = hi;
        double k0 = (double)MF32(keys + p * 4);
        /* fld dword / fstp dword through a double: a signalling NaN comes back quiet (clang folds a plain
         * (float)(double) round trip, so it is spelled out) */
        pv_bits = M32(keys + p * 4);
        if ((pv_bits & 0x7F800000u) == 0x7F800000u && (pv_bits & 0x007FFFFFu)) pv_bits |= 0x00400000u;
        float pv;
        memcpy(&pv, &pv_bits, 4);
        c->st[(top - 1) & 7] = k0;
        if (lo < hi) {
            double piv = (double)pv, key;
            c->st[(top - 1) & 7] = piv;
            int sw = 0;
            for (;;) {
                /* right scan: from edi down while key <= pivot */
                uint32_t a = M32(self + 0x10) + (uint32_t)edi * 2;
                uint32_t kp = M32(self + 0x20);
                key = (double)MF32(kp + (uint32_t)M16(a) * 4);
                if (key <= piv) {
                    while (esi < edi) {
                        a -= 2; edi--;
                        key = (double)MF32(kp + (uint32_t)M16(a) * 4);
                        if (!(key <= piv)) break;
                    }
                }
                if (esi != edi) {
                    uint32_t ia = M32(self + 0x10);
                    M16(ia + (uint32_t)esi * 2) = M16(ia + (uint32_t)edi * 2);
                    esi++;
                }
                /* left scan: from esi up while key >= pivot */
                a = M32(self + 0x10) + (uint32_t)esi * 2;
                kp = M32(self + 0x20);
                key = (double)MF32(kp + (uint32_t)M16(a) * 4);
                if (key >= piv) {
                    while (esi < edi) {
                        a += 2; esi++;
                        key = (double)MF32(kp + (uint32_t)M16(a) * 4);
                        if (!(key >= piv)) break;
                    }
                }
                if (esi == edi) { sw = 1; break; }  /* eax: the status word, below */
                uint32_t ia = M32(self + 0x10);
                M16(ia + (uint32_t)edi * 2) = M16(ia + (uint32_t)esi * 2);
                edi--;
                eax = ia;
                if (!(esi < edi)) break;
            }
            /* the last compare (key against pivot): fcomp, fnstsw ax with top = pivot copy's slot */
            uint32_t c0, c2, c3;
            if (key != key || piv != piv) c0 = c2 = c3 = 1;
            else { c0 = key < piv; c2 = 0; c3 = key == piv; }
            c->c0 = c0; c->c2 = c2; c->c3 = c3;
            if (sw)
                eax = (c3 << 14) | (((top - 2) & 7) << 11) | (c2 << 10) | (c->c1 << 9) | (c0 << 8);
            c->st[(top - 2) & 7] = piv;
            c->st[(top - 3) & 7] = key;
        }
        edx = M32(self + 0x10);
        eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu);
        M16(edx + (uint32_t)esi * 2) = (uint16_t)p;
        if (first < esi) {
            ae4cb0_sort(c, self, first, esi - 1, top, out);
            eax = out->eax; edx = out->edx;
        }
        if (!(hi > esi)) break;
        lo = esi + 1;
    }
    out->eax = eax; out->edx = edx; out->p = p; out->pv = pv_bits;
}

void f_AE4CB0(CPU *c) {
    uint32_t esp = c->r[4];
    AE4Out out;
    ae4cb0_sort(c, c->r[1], (int32_t)M32(esp + 4), (int32_t)M32(esp + 8), c->top, &out);
    M32(esp + 4) = out.pv;  /* the pivot float in the first argument slot */
    c->r[0] = out.eax;
    c->r[1] = out.p;        /* pop ecx: the slot of the saved ecx holds the pivot index by then */
    c->r[2] = out.edx;
    c->r[4] = esp + 12;
}
#endif
