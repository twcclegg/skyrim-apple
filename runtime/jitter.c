/* Sub-pixel camera jitter for temporal upscaling. See jitter.h. */
#include "jitter.h"
#include <stdatomic.h>
#include <string.h>

#define KEPT 8

/* What the game set and what it was given back, so a shifted frustum is recognised when it comes round again. */
typedef struct Shifted { float set[4], given[4], pixels[2]; int used; } Shifted;

static atomic_int g_phases;                  /* wanted; read at each frame */
static _Atomic float g_fixed_x, g_fixed_y;
static atomic_int g_fixed;
static _Atomic float g_scale = 1;
static int g_index;                           /* main thread from here on */
static float g_now[2];
static int g_now_valid;
static Shifted g_shifted[KEPT];
static int g_next;

void jitter_configure(int phases) {
    atomic_store(&g_fixed, 0);
    atomic_store(&g_phases, phases < 0 ? 0 : phases > 256 ? 256 : phases);
}

void jitter_fixed(float x, float y) {
    atomic_store(&g_fixed_x, x);
    atomic_store(&g_fixed_y, y);
    atomic_store(&g_fixed, 1);
    atomic_store(&g_phases, 0);
}

void jitter_test_scale(float scale) { atomic_store(&g_scale, scale); }

int jitter_on(void) { return atomic_load(&g_fixed) || atomic_load(&g_phases) > 0; }

static float halton(int index, int base) {
    float result = 0, fraction = 1;
    for (int i = index; i > 0; i /= base) {
        fraction /= (float)base;
        result += fraction * (float)(i % base);
    }
    return result;
}

void jitter_sequence(int index, int phases, float out[2]) {
    if (phases <= 0) { out[0] = out[1] = 0; return; }
    int i = index % phases + 1;  /* Halton starts at 1: index 0 is the corner, not a sample */
    out[0] = halton(i, 2) - 0.5f;
    out[1] = halton(i, 3) - 0.5f;
}

void jitter_pixels(float out[2]) {
    if (!g_now_valid) {
        if (atomic_load(&g_fixed)) {
            g_now[0] = atomic_load(&g_fixed_x);
            g_now[1] = atomic_load(&g_fixed_y);
        } else {
            jitter_sequence(g_index, atomic_load(&g_phases), g_now);
            g_now[0] *= atomic_load(&g_scale);
            g_now[1] *= atomic_load(&g_scale);
        }
        g_now_valid = 1;
    }
    out[0] = g_now[0];
    out[1] = g_now[1];
}

void jitter_next_frame(void) {
    g_index = (g_index + 1) & 0xFFFFF;
    g_now_valid = 0;
}

/* The newest frustum handed back that is bit for bit this one. */
static const Shifted *shifted_find(const float frustum[4]) {
    for (int back = 1; back <= KEPT; back++) {
        const Shifted *entry = &g_shifted[(g_next + KEPT - back) % KEPT];
        if (entry->used && !memcmp(frustum, entry->given, sizeof entry->given)) return entry;
    }
    return 0;
}

int jitter_apply(float frustum[4], uint32_t width, uint32_t height) {
    /* The game leaves the frustum alone from frame to frame unless the field of view changes, and copies it out
     * and back around the first-person pass, so what arrives is often one handed back earlier: start again from
     * what the game set. */
    const Shifted *was = shifted_find(frustum);
    float set[4], pixels[2] = {0, 0};
    memcpy(set, was ? was->set : frustum, sizeof set);
    if (jitter_on() && width && height) jitter_pixels(pixels);
    if (pixels[0] == 0 && pixels[1] == 0) {
        if (!was) return 0;
        memcpy(frustum, set, sizeof set);  /* jitter went off: give the game its frustum back */
        return 1;
    }
    /* Left and right span the width, top and bottom the height. Moving the window left moves the picture right;
     * moving it up (top is the larger tangent) moves the picture down. */
    float across = -pixels[0] * (set[1] - set[0]) / (float)width;
    float up = pixels[1] * (set[2] - set[3]) / (float)height;
    float given[4] = {set[0] + across, set[1] + across, set[2] + up, set[3] + up};
    if (!memcmp(given, frustum, sizeof given)) return 0;  /* already this frame's */
    Shifted *entry = &g_shifted[g_next];
    g_next = (g_next + 1) % KEPT;
    memcpy(entry->set, set, sizeof set);
    memcpy(entry->given, given, sizeof given);
    entry->pixels[0] = pixels[0];
    entry->pixels[1] = pixels[1];
    entry->used = 1;
    memcpy(frustum, given, sizeof given);
    return 1;
}

int jitter_remove(float frustum[4], float pixels[2]) {
    pixels[0] = pixels[1] = 0;
    const Shifted *entry = shifted_find(frustum);
    if (!entry) return 0;
    memcpy(frustum, entry->set, sizeof entry->set);
    pixels[0] = entry->pixels[0];
    pixels[1] = entry->pixels[1];
    return 1;
}
