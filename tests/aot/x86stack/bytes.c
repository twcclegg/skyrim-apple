/* Models locals narrower than a stack slot: flags, characters, 16-bit counters and small structs that the
 * compiler writes a byte at a time and may read back whole. */
struct rgb { uint8_t r, g, b, a; };
struct rec { uint16_t id; uint8_t kind; uint8_t flags; int16_t delta; uint16_t pad; };
union word { uint32_t u; uint16_t h[2]; uint8_t b[4]; };
static uint8_t out8[16];
static uint16_t out16[8];

__attribute__((noinline)) static uint32_t pack(struct rgb c) { return (uint32_t)c.r | c.g << 8 | c.b << 16 | (uint32_t)c.a << 24; }

__attribute__((noinline)) static struct rgb blend(struct rgb x, struct rgb y, uint8_t t) {
    struct rgb o;
    o.r = (uint8_t)((x.r * (255 - t) + y.r * t) >> 8);
    o.g = (uint8_t)((x.g * (255 - t) + y.g * t) >> 8);
    o.b = (uint8_t)(x.b ^ y.b);
    o.a = (uint8_t)(x.a > y.a ? x.a : y.a);
    return o;
}

__attribute__((noinline)) static uint32_t flags(uint32_t v) {
    uint8_t neg = (int32_t)v < 0, odd = v & 1, big = v > 0xffff, zero = v == 0;
    uint16_t lo = (uint16_t)v, hi = (uint16_t)(v >> 16);
    int8_t s = (int8_t)(v >> 7);
    int16_t w = (int16_t)(v >> 3);
    out8[v & 15] += neg + odd * 2 + big * 4 + zero * 8;
    out16[v & 7] ^= lo;
    if (neg) lo ^= hi;
    if (odd) hi += lo;
    if (big) s = (int8_t)(s + w);
    out8[(v >> 4) & 15] ^= (uint8_t)s;
    return (uint32_t)lo << 16 | hi | (uint32_t)(int32_t)s << 8 | (uint32_t)(int32_t)w;
}

__attribute__((noinline)) static uint32_t records(uint32_t seed) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 12; i++) {
        struct rec r;
        union word w;
        w.u = seed * (i + 1);
        r.id = w.h[0]; r.kind = w.b[2]; r.flags = w.b[3] & 0x0f; r.delta = (int16_t)(w.h[1] - 300); r.pad = 0;
        out16[i & 7] += r.id;
        if (r.kind & 1) r.delta = (int16_t)-r.delta;
        w.b[1] ^= r.flags; w.h[1] += (uint16_t)r.delta;
        sum = sum * 17 + w.u + r.kind + (uint32_t)(int32_t)r.delta;
        seed += sum >> 5;
    }
    return sum;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    struct rgb x = {(uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)(a >> 24)};
    struct rgb y = {(uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24)};
    uint32_t r = 0;
    for (uint32_t t = 0; t < 256; t += 37) {
        struct rgb m = blend(x, y, (uint8_t)t);
        r = r * 5 + pack(m);
        x.g = m.r; y.a = m.b;
    }
    r ^= flags(a) + flags(b) * 3 + flags(a ^ b) + flags(0) + flags(r);
    return r + records(a + b) + out8[r & 15] + out16[r & 7];
}
