/* Models an interpreter loop: one large function with a switch, many locals of different lifetimes, calls
 * in some arms only, and returns from the middle. */
static uint32_t mem[64];
static uint8_t code[48];

__attribute__((noinline)) static uint32_t helper(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    mem[(a + op) & 63] += b ^ c;
    return (a * (op + 3)) ^ (b + d) ^ (c >> (op & 7));
}

__attribute__((noinline)) static uint32_t step_limit(uint32_t a) { return 60 + (a & 31); }

__attribute__((noinline)) static uint32_t interp(uint32_t a, uint32_t b) {
    uint32_t r0 = a, r1 = b, r2 = a ^ b, r3 = a + b, pc = 0, sp = 0, steps = 0, flag = 0;
    uint32_t stack[8] = {0};
    uint32_t limit = step_limit(a);
    while (steps++ < limit) {
        uint32_t op = code[pc % 48];
        pc++;
        switch (op & 15) {
        case 0: r0 += r1; break;
        case 1: r1 ^= r2 << (r3 & 7); break;
        case 2: r2 = helper(op, r0, r1, r2, r3); break;
        case 3: r3 -= r0; flag = r3 == 0; break;
        case 4: stack[sp++ & 7] = r0; break;
        case 5: r1 += stack[--sp & 7]; break;
        case 6: if (flag) pc += 3; break;
        case 7: mem[r2 & 63] = r3; break;
        case 8: r0 ^= mem[r1 & 63]; break;
        case 9: { uint32_t t = r0; r0 = r3; r3 = t; } break;
        case 10: if ((r0 & 0xff) == 0x5a) return r0 + r1 + steps; break;
        case 11: r2 += helper(op, r3, r2, r1, r0) + helper(op + 1, r0, r1, pc, sp); break;
        case 12: flag = (int32_t)r1 < (int32_t)r2; break;
        case 13: pc = r3 % 48; break;
        case 14: r3 = r3 * 5 + steps; break;
        default: r0 = r0 >> 1 | r0 << 31; break;
        }
    }
    return r0 ^ r1 ^ r2 ^ r3 ^ pc ^ sp ^ stack[sp & 7];
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t s = a * 2654435761u + b;
    for (uint32_t i = 0; i < 48; i++) { s = s * 1103515245u + 12345u; code[i] = (uint8_t)(s >> 16); }
    for (uint32_t i = 0; i < 64; i++) mem[i] = b + i;
    uint32_t r = interp(a, b);
    return r + interp(r, a) + mem[r & 63];
}
