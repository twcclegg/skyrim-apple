/* Example corpus program: a checksum loop in lifted-code style. Registers hold all the working state. */
#include "pinned_test.h"

/* esi = index of first word, ecx = count; result in eax. edx is scratch; ebx counts odd words. */
__attribute__((noinline)) static void checksum(void) {
    eax = 0;
    while (ecx != 0) {
        edx = M(esi);
        if (edx & 1) {
            ebx += 1;                 /* conditional write */
            eax ^= edx << 3;
        } else {
            eax += edx;
        }
        if (eax == 0x1234u && (edi = esi, edx > 7)) eax = ~eax;   /* write inside a condition */
        esi += 1;
        ecx -= 1;
    }
}

/* Calls checksum twice; keeps a running total in ebp across the calls. */
__attribute__((noinline)) static uint32_t twice(uint32_t first, uint32_t n) {
    esi = first; ecx = n;
    CALLV(checksum());
    ebp = eax;
    esi = first + n; ecx = n / 2;
    CALLV(checksum());
    if (ebp > eax) ebp -= eax; else ebp ^= eax;
    return ebp + ebx;
}

int main(void) {
    PIN_BEGIN();
    for (int i = 0; i < MEM_WORDS; i++) mem[i] = (uint32_t)i * 2654435761u + 12345u;
    SET_REGS(1, 2, 3, 4, 5, 6, 7, 8);
    REPORT("empty", CALL(twice(0, 0)));
    SET_REGS(0, 0, 0, 0, 0, 0, 0, 0);
    REPORT("short", CALL(twice(10, 5)));
    SET_REGS(0xffffffffu, 9, 9, 0x80000000u, 1, 1, 1, 1);
    REPORT("long", CALL(twice(200, 100)));
    mem[17] = 0x1234u; mem[18] = 0;
    SET_REGS(0, 0, 0, 0, 0, 0, 0, 0);
    REPORT("magic", CALL(twice(17, 3)));
    PIN_END();
    return 0;
}
