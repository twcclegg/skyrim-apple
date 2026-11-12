/* Models arithmetic with more live values than x86 has registers: 64-bit accumulators in loops that also
 * store to tables through computed pointers, so spilled values sit on the stack across unrelated stores. */
static uint64_t tab[32];
static uint32_t idx[64];

__attribute__((noinline)) static uint64_t mul_hi_lo(uint64_t a, uint64_t b, uint64_t *lo) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

__attribute__((noinline)) static uint64_t churn(uint64_t s0, uint64_t s1, uint32_t rounds, uint32_t *where) {
    uint64_t a = s0, b = s1, c = s0 ^ s1, d = s0 + s1, e = ~s0, f = s1 * 3;
    for (uint32_t i = 0; i < rounds; i++) {
        a += b; d ^= a; d = d << 16 | d >> 48;
        c += d; b ^= c; b = b << 12 | b >> 52;
        e += f + i; f ^= e >> 7; f *= 0x9e3779b97f4a7c15ull;
        where[(uint32_t)a & 31] += (uint32_t)e;        /* store through a pointer between uses of a..f */
        tab[(uint32_t)c & 31] ^= d + f;
        if ((uint32_t)b & 0x100) { uint64_t t = a; a = e; e = t; }
        if ((int64_t)c < 0) c -= a; else c += b >> 3;
    }
    return a ^ b ^ c ^ d ^ e ^ f;
}

__attribute__((noinline)) static uint32_t carries(uint32_t x, uint32_t y) {
    uint64_t acc = 0, lo = 0;
    uint32_t over = 0;
    for (uint32_t i = 0; i < 20; i++) {
        uint64_t before = acc;
        acc += (uint64_t)x * (y + i) + (acc >> 9);
        over += acc < before;
        acc ^= mul_hi_lo(acc, before | 1, &lo);
        idx[i] ^= (uint32_t)lo;
        x = x * 1103515245u + 12345u;
    }
    return (uint32_t)acc ^ (uint32_t)(acc >> 32) ^ over ^ (uint32_t)lo;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint64_t r = churn((uint64_t)a << 32 | b, (uint64_t)b * a + 1, 40 + (a & 15), idx);
    r += churn(r, tab[b & 31], 25, idx + (a & 3));
    uint32_t c = carries(a, b) + carries((uint32_t)r, (uint32_t)(r >> 32));
    return (uint32_t)r ^ (uint32_t)(r >> 29) ^ c ^ idx[c & 63];
}
