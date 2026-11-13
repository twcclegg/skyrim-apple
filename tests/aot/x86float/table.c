/* Models interpolation over float tables: locals of mixed width (float, double, uint16) side by side in one
 * frame, indexes computed from floats, and callbacks through a table of float functions. */
static float curve[33];
static uint16_t lut[64];
static uint32_t marks[8];

__attribute__((noinline)) static float lerp(float a, float b, float t) { return a + (b - a) * t; }
__attribute__((noinline)) static float ease(float t) { return t * t * (3.0f - 2.0f * t); }
__attribute__((noinline)) static float sharp(float t) { return t * t * t; }
__attribute__((noinline)) static float flat(float t) { return 0.5f + 0.0f * t; }
static float (*const shapes[4])(float) = {ease, sharp, flat, ease};

__attribute__((noinline)) static float sample(float x, int which) {
    if (x < 0) x = 0;
    if (x > 31.999f) x = 31.999f;
    int i = (int)x;
    float t = shapes[which & 3](x - (float)i);
    return lerp(curve[i], curve[i + 1], t);
}

__attribute__((noinline)) static uint32_t sweep(uint32_t seed, float speed) {
    double total = 0;
    float x = 0, peak = -1e30f;
    uint16_t steps = 0;
    for (int k = 0; k < 200; k++) {
        float y = sample(x, (int)(seed >> (k & 15)));
        total += (double)y * (double)speed;
        if (y > peak) { peak = y; marks[k & 7] += (uint32_t)k; }
        lut[(uint32_t)(y * 10.0f) & 63] += steps;
        x += speed * (1.0f + (float)(k & 3) * 0.25f);
        if (x > 32.0f) x -= 32.0f;
        steps++;
    }
    union { float f; uint32_t u; } p = {peak};
    union { double d; uint64_t q; } t = {total};
    return p.u ^ (uint32_t)t.q ^ (uint32_t)(t.q >> 32) ^ steps;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    for (int i = 0; i < 33; i++) curve[i] = (float)((a >> (i & 15)) & 255u) * 0.01f + (float)i * 0.1f;
    uint32_t r = sweep(a ^ b, 0.37f + (float)(b & 15u) * 0.05f);
    r ^= sweep(b, 1.5f);
    return r + lut[r & 63] + marks[r & 7];
}
