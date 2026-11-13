/* Models call-heavy code with long argument lists in mixed conventions: every call needs stack arguments
 * written just before it, between reads of the caller's own locals. */
static uint32_t g[16];

__attribute__((noinline)) static uint32_t c9(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t h, uint32_t i, uint32_t j) {
    g[a & 15] += j;
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * h + 8 * i + 9 * j;
}
__attribute__((noinline)) X86_STDCALL static uint32_t s5(uint32_t a, uint32_t b, uint64_t c, uint32_t d, uint8_t e) {
    g[d & 15] ^= (uint32_t)c;
    return (a ^ b) + (uint32_t)(c >> 32) * d + e;
}
__attribute__((noinline)) X86_FASTCALL static uint32_t f6(uint32_t a, uint32_t b, uint32_t c, uint16_t d, uint32_t e, uint32_t f) {
    return (a - b) * (c | 1) + d + (e ^ f);
}
__attribute__((noinline)) X86_REGPARM(3) static uint32_t r7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t h) {
    g[h & 15] -= a;
    return a ^ b ^ c ^ (d + e + f + h);
}
__attribute__((noinline)) X86_STDCALL static uint32_t s0(void) { return g[3] + 1; }
__attribute__((noinline)) X86_STDCALL static uint32_t s1(uint32_t a) { return a * 3 + g[a & 15]; }

__attribute__((noinline)) static uint32_t hub(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t p = x + y, q = y * z, r = z - x, s = x ^ y ^ z, t = p * 3, u = q + r;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 6; i++) {
        acc += c9(p, q, r, s, t, u, i, acc, x);
        acc ^= s5(q, r, (uint64_t)s << 32 | t, u, (uint8_t)i);
        acc += s0();
        acc -= f6(r, s, t, (uint16_t)u, p, q);
        acc ^= s1(acc) + s1(p);
        acc += r7(s, t, u, p, q, r, acc);
        uint32_t t1 = s1(q), t2 = s0(), t3 = r7(u, t, s, r, q, p, i), t4 = s5(p, p, q, r, 1);
        acc += c9(t1, t2, f6(p, q, r, (uint16_t)s, t, u), i, t3, t4, acc, y, z);   /* f6 has no side effects */
        p ^= acc; q += i; r = r * 5 + 1; s -= q; t ^= r; u += s;
    }
    return acc + p + q + r + s + t + u;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t r = hub(a, b, a + b);
    r ^= hub(b, r, a);
    return r + g[r & 15];
}
