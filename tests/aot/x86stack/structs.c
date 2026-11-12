/* Models aggregates passed and returned by value: hidden result pointers, argument blocks copied onto the
 * stack, and temporaries the caller keeps while the callee works on its own copy. */
struct vec { uint32_t x, y, z; };
struct big { uint32_t w[6]; uint16_t tag; uint8_t on; };
static struct vec store[8];

__attribute__((noinline)) static struct vec add(struct vec a, struct vec b) {
    a.x += b.x; a.y ^= b.y; a.z -= b.z;
    return a;
}

__attribute__((noinline)) static struct vec scale(struct vec a, uint32_t k) {
    struct vec r = {a.x * k, a.y * (k + 1), a.z * (k | 1)};
    a.x = a.y = a.z = 0;                  /* the callee's copy only */
    return r;
}

__attribute__((noinline)) static uint32_t dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

__attribute__((noinline)) static struct big widen(struct vec v, uint32_t salt) {
    struct big b;
    for (uint32_t i = 0; i < 6; i++) b.w[i] = (i & 1 ? v.x : v.y) + v.z * i + salt;
    b.tag = (uint16_t)(v.x >> 3);
    b.on = (uint8_t)(salt & 1);
    return b;
}

__attribute__((noinline)) static uint32_t digest(struct big b, uint32_t k) {
    uint32_t h = b.tag + b.on;
    for (uint32_t i = 0; i < 6; i++) { h = (h ^ b.w[i]) * 16777619u; b.w[i] = k; }
    return h;
}

__attribute__((noinline)) static uint32_t orbit(uint32_t a, uint32_t b) {
    struct vec p = {a, b, a ^ b}, v = {b | 1, a + 3, 7}, keep = p;
    uint32_t h = 0;
    for (uint32_t i = 0; i < 10; i++) {
        p = add(p, scale(v, i + 1));
        store[i & 7] = p;
        v = add(scale(v, 3), keep);
        h = h * 31 + dot(p, v);
        struct big w = widen(p, h);
        h ^= digest(w, i) + w.w[i % 6] + w.tag;
        keep.y += h;
    }
    return h + dot(keep, p);
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t r = orbit(a, b);
    r += orbit(b + r, a);
    return r ^ store[r & 7].x ^ store[(r >> 3) & 7].z;
}
