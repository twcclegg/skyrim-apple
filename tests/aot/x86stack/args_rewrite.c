/* Models callees that treat their stack arguments as scratch: they overwrite them, take their address, and
 * pass the rewritten ones on. The caller's copies must stay what the caller made them. */
static uint32_t log_[16];
static uint32_t n_;

static void note(uint32_t v) { log_[n_++ & 15] ^= v * 2654435761u; }

__attribute__((noinline)) static uint32_t clobber(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    a += b; b ^= c; c = c * 3 + d; d = d - e; e = e << (f & 7); f = f + a;
    note(a); note(b); note(c);
    for (uint32_t i = 0; i < 5; i++) { a = a * 31 + f; f ^= e + i; e += d; }
    return a ^ b ^ c ^ d ^ e ^ f;
}

__attribute__((noinline)) static void bump(uint32_t *p, uint32_t by) { *p = *p * 5 + by; }

__attribute__((noinline)) static uint32_t through_pointer(uint32_t a, uint32_t b, uint32_t c) {
    bump(&a, b);
    bump(&c, a);
    b = b + a + c;
    bump(&b, 7);
    return a + b * 3 + c * 5;
}

__attribute__((noinline)) static uint32_t last(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    note(a + b);
    return (a - b) * (c | 1) + d;
}

__attribute__((noinline)) static uint32_t forward(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    /* rewrites its arguments in place, then hands them to the next function (a tail call when optimised) */
    a ^= d; d += b; b = c * 9; c = a + 1;
    if (a & 1) return last(d, c, b, a);
    return last(b, a, d, c);
}

__attribute__((noinline)) static uint32_t caller(uint32_t x, uint32_t y) {
    uint32_t p = x + 1, q = y ^ 0x55aa, r = x * y + 3, s = x - y, t = (x << 3) | 1, u = y >> 2;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 6; i++) {
        sum += clobber(p, q, r, s, t, u);
        log_[i] += sum;                       /* a store the compiler cannot place: forces reloads of p..u */
        sum ^= through_pointer(q, r, s);
        sum += forward(t, u, p, q);
        p += i; q ^= sum; r += s; s = s * 7 + 1; t ^= u; u += 3;
    }
    return sum + p + q + r + s + t + u;       /* the caller's copies, after every callee scribbled on its own */
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t v = caller(a, b);
    v ^= caller(b + n_, a ^ v);
    return v + log_[v & 15] + n_;
}
