/* Models printf-style callees: they walk their caller's outgoing arguments through a pointer, so every
 * argument has to be in guest memory at the call even though the caller never reads it back. */
static uint32_t acc[8];

__attribute__((noinline)) static uint32_t sum_n(uint32_t n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    uint32_t s = n * 0x01000193u;
    for (uint32_t i = 0; i < n; i++) s = (s ^ __builtin_va_arg(ap, uint32_t)) * 16777619u;
    __builtin_va_end(ap);
    return s;
}

__attribute__((noinline)) static uint32_t mixed(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    uint32_t s = 7;
    for (; *fmt; fmt++) {
        if (*fmt == 'd') s = s * 33 + __builtin_va_arg(ap, uint32_t);
        else if (*fmt == 'q') { uint64_t q = __builtin_va_arg(ap, uint64_t); s = s * 33 + (uint32_t)q + (uint32_t)(q >> 32); }
        else if (*fmt == 'p') { uint32_t *p = __builtin_va_arg(ap, uint32_t *); *p += s; s ^= *p; }
        else s += (uint8_t)*fmt;
    }
    __builtin_va_end(ap);
    return s;
}

__attribute__((noinline)) static uint32_t relay(uint32_t k, uint32_t a, uint32_t b, uint32_t c) {
    /* locals live across variadic calls; the arguments are built from them each time */
    uint32_t x = a + k, y = b ^ k, z = c * (k | 1), w = a - c;
    uint32_t r = sum_n(0);
    r += sum_n(1, x);
    acc[k & 7] += r;
    r ^= sum_n(4, x, y, z, w);
    r += sum_n(7, w, z, y, x, x + y, y + z, z + w);
    acc[(k + 1) & 7] ^= r;
    x += r; y -= r; z ^= r; w += x;
    r += mixed("dqd", x, (uint64_t)y << 32 | z, w);
    r ^= mixed("dpdp", z, &acc[k & 7], y, &acc[(k + 3) & 7]);
    return r + x + y + z + w;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (uint32_t k = 0; k < 9; k++) r = r * 3 + relay(k, a + r, b ^ k, a * b + k);
    uint32_t local = r;
    r += mixed("pdq-", &local, r, (uint64_t)a * b);
    return r ^ local ^ acc[r & 7];
}
