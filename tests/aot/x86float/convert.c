/* Models game-style number handling: float/double/int conversions (x87 truncation through the control
 * word), rounding, comparisons that set integer flags, and doubles split into 32-bit halves. */
static int32_t ints[16];
static uint32_t halves[8];
static double acc;

__attribute__((noinline)) static int32_t trunc_i(float f) { return (int32_t)f; }
__attribute__((noinline)) static uint32_t trunc_u(double d) { return d < 0 ? 0u : d > 4e9 ? 0xFFFFFFFFu : (uint32_t)d; }
__attribute__((noinline)) static int64_t wide(double d) { return d < -9e18 || d > 9e18 ? 0 : (int64_t)d; }
__attribute__((noinline)) static double mix(double a, float b, int32_t c, double d) { return a * b + (double)c - d / (b + 2.5); }

__attribute__((noinline)) static uint32_t classify(float x, float lo, float hi) {
    uint32_t r = 0;
    if (x < lo) r |= 1;
    if (x > hi) r |= 2;
    if (x == lo) r |= 4;
    if (!(x >= lo && x <= hi)) r |= 8;
    if (x != x) r |= 16;
    return r;
}

__attribute__((noinline)) static uint32_t split(double d, int k) {
    union { double d; uint32_t w[2]; uint64_t q; } u;
    u.d = d;
    halves[k & 7] ^= u.w[0];
    halves[(k + 1) & 7] += u.w[1];
    u.w[0] ^= 0x55555555u;
    return u.w[1] ^ (uint32_t)(u.q >> 7) ^ (uint32_t)(u.d > 0);
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t h = 0;
    double d = (double)(int32_t)a * 0.001 + (double)b;
    float f = (float)(int32_t)(b ^ a) / 3.0f;
    for (int i = 0; i < 16; i++) {
        ints[i] = trunc_i(f * (float)i) + trunc_i(-f) * 2;
        h = h * 31 + trunc_u(d * i) + (uint32_t)wide(d * 1e6 * i);
        h ^= classify(f, -100.0f + i, 100.0f * i) << (i & 15);
        d = mix(d, f, ints[i], d * 0.5 + 1.0);
        f = (float)(d * 1e-3) - (float)ints[(i + 3) & 15];
        h += split(d, i);
        acc += d;
        if (d > 1e12 || d < -1e12) d = 3.0;
    }
    return h ^ halves[h & 7] ^ (uint32_t)ints[a & 15] ^ (uint32_t)(int32_t)acc;
}
