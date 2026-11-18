/* Models frames whose size is only known at run time (variable-length arrays and alloca) next to ordinary
 * ones: the stack pointer moves by amounts the translator cannot follow in some functions. */
static uint32_t sink[16];

__attribute__((noinline)) static uint32_t fill(uint32_t *p, uint32_t n, uint32_t seed) {
    uint32_t s = seed;
    for (uint32_t i = 0; i < n; i++) { s = s * 69069u + 1; p[i] = s >> 4; }
    return s;
}

__attribute__((noinline)) static uint32_t vla(uint32_t n, uint32_t seed) {
    uint32_t before = seed * 3;
    uint32_t buf[n];
    uint32_t after = fill(buf, n, seed);
    uint32_t sum = before;
    for (uint32_t i = 0; i < n; i++) sum = sum * 33 + buf[i];
    sink[n & 15] += sum;
    return sum ^ after;
}

__attribute__((noinline)) static uint32_t grow(uint32_t n, uint32_t seed) {
    uint32_t total = 0;
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t *p = __builtin_alloca(i * sizeof(uint32_t));
        total += fill(p, i, seed + i);
        total ^= p[i - 1] + p[0];
    }
    return total;
}

__attribute__((noinline)) static uint32_t plain(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = a * b, y = b + c, z = c ^ a;
    sink[x & 15] ^= y;
    return vla((x & 15) + 1, y) + grow((z & 7) + 1, x) + x + y + z;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t r = plain(a, b, a ^ b);
    r += vla((r & 31) + 1, b);
    r ^= plain(r, a, b);
    return r + sink[r & 15];
}
