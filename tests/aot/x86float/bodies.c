/* Models a small particle step: float vectors passed and returned by value, many float temporaries live
 * across calls, square roots, and results folded into integer bits. */
struct v3 { float x, y, z; };
static uint32_t out[16];
static struct v3 pos[8], vel[8];

static uint32_t bits(float f) { union { float f; uint32_t u; } k; k.f = f; return k.u; }

__attribute__((noinline)) static struct v3 add(struct v3 a, struct v3 b) { struct v3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
__attribute__((noinline)) static struct v3 scale(struct v3 a, float k) { struct v3 r = {a.x * k, a.y * k, a.z * k}; return r; }
__attribute__((noinline)) static float dot(struct v3 a, struct v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
__attribute__((noinline)) static float len(struct v3 a) { return __builtin_sqrtf(dot(a, a)); }

__attribute__((noinline)) static struct v3 norm(struct v3 a) {
    float l = len(a);
    if (l < 1e-6f) { struct v3 z = {0, 0, 1}; return z; }
    return scale(a, 1.0f / l);
}

__attribute__((noinline)) static float step(int n, float dt, float drag) {
    float energy = 0, maxv = 0, minv = 1e30f;
    for (int i = 0; i < n; i++) {
        struct v3 g = {0, -9.8f * dt, 0};
        vel[i] = add(scale(vel[i], 1.0f - drag * dt), g);
        pos[i] = add(pos[i], scale(vel[i], dt));
        if (pos[i].y < 0) { pos[i].y = -pos[i].y; vel[i].y = -vel[i].y * 0.5f; }
        float v = len(vel[i]);
        energy += 0.5f * v * v + 9.8f * pos[i].y;
        if (v > maxv) maxv = v;
        if (v < minv) minv = v;
        struct v3 d = norm(vel[i]);
        out[i & 15] ^= bits(d.x) + bits(d.z) * 3u;
    }
    return energy + maxv * 0.25f - minv;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    for (int i = 0; i < 8; i++) {
        pos[i].x = (float)(int32_t)(a % 97u) + i; pos[i].y = (float)(b & 255u) * 0.5f + i; pos[i].z = (float)i;
        vel[i].x = (float)(int32_t)(b % 13u) - 6.0f; vel[i].y = (float)(i * 3) - 4.0f; vel[i].z = (float)(a & 7u) * 0.125f;
    }
    float e = 0;
    for (int k = 0; k < 20; k++) e = e * 0.5f + step(8, 0.016f + (float)(a & 3u) * 0.001f, 0.1f);
    return bits(e) ^ out[a & 15] ^ out[(b >> 4) & 15];
}
