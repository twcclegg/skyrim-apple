/* Checks for runtime/jitter.c: the sequence, the shift it puts into a frustum, and that a frustum the game keeps,
 * copies or sets again never collects more than one shift. Built and run by tests/temporal/run.py. */
#include "jitter.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(what, ...) do { \
    g_checks++; \
    if (!(what)) { g_failed++; printf("FAILED %s:%d: %s: ", __FILE__, __LINE__, #what); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int near(float a, float b, float within) { return fabsf(a - b) <= within; }

/* Where a point at tangents (x right, y up) lands on a width x height picture drawn with this frustum. */
static void project(const float f[4], float x, float y, float width, float height, float out[2]) {
    out[0] = (x - f[0]) / (f[1] - f[0]) * width;
    out[1] = (f[2] - y) / (f[2] - f[3]) * height;
}

static const float WORLD[4] = {-0.849427f, 0.849427f, 0.477803f, -0.477803f};
static const float FIRST[4] = {-0.5f, 0.5f, 0.28125f, -0.28125f};

static void sequence(void) {
    float sum[2] = {0, 0}, p[2];
    int cells[4][4] = {{0}};
    for (int i = 0; i < 32; i++) {
        jitter_sequence(i, 32, p);
        CHECK(p[0] > -0.5f && p[0] < 0.5f && p[1] > -0.5f && p[1] < 0.5f, "position %d is %f %f", i, p[0], p[1]);
        sum[0] += p[0];
        sum[1] += p[1];
        cells[(int)((p[1] + 0.5f) * 4)][(int)((p[0] + 0.5f) * 4)]++;
    }
    CHECK(fabsf(sum[0] / 32) < 0.03f && fabsf(sum[1] / 32) < 0.03f, "mean %f %f", sum[0] / 32, sum[1] / 32);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) CHECK(cells[y][x] >= 1, "32 positions leave cell %d,%d of a 4x4 grid empty", x, y);
    float again[2];
    jitter_sequence(3, 8, p);
    jitter_sequence(3 + 8 * 5, 8, again);
    CHECK(p[0] == again[0] && p[1] == again[1], "the sequence does not repeat after its length");
    jitter_sequence(7, 0, p);
    CHECK(p[0] == 0 && p[1] == 0, "no positions, but a shift of %f %f", p[0], p[1]);
}

static void shift_direction(void) {
    float f[4], before[2], after[2], shift[2];
    jitter_fixed(3, 2);
    memcpy(f, WORLD, sizeof f);
    project(f, 0.1f, 0.2f, 1280, 720, before);
    CHECK(jitter_apply(f, 1280, 720) == 1, "a fresh frustum was not shifted");
    project(f, 0.1f, 0.2f, 1280, 720, after);
    CHECK(near(after[0] - before[0], 3, 1e-3f) && near(after[1] - before[1], 2, 1e-3f),
          "asked for 3 right 2 down, the picture moved %f %f", after[0] - before[0], after[1] - before[1]);
    CHECK(near(f[1] - f[0], WORLD[1] - WORLD[0], 1e-6f) && near(f[2] - f[3], WORLD[2] - WORLD[3], 1e-6f),
          "the shift changed the field of view");
    CHECK(jitter_remove(f, shift) == 1 && shift[0] == 3 && shift[1] == 2 && !memcmp(f, WORLD, sizeof f),
          "taking the shift out gave %f %f and a different frustum", shift[0], shift[1]);
    float other[4] = {-1, 1, 0.5f, -0.5f};
    CHECK(jitter_remove(other, shift) == 0 && shift[0] == 0 && shift[1] == 0, "a frustum never shifted came back shifted");
    float zero[4] = {0, 0, 0, 0};
    CHECK(jitter_remove(zero, shift) == 0, "an all-zero frustum matched an empty slot");
    memcpy(f, WORLD, sizeof f);
    CHECK(jitter_apply(f, 0, 0) == 0 && !memcmp(f, WORLD, sizeof f), "shifted with no picture size");
}

/* The game as it was measured: the frustum stays in the camera from frame to frame, the camera is updated once a
 * frame, and around the first-person pass the frustum is replaced, copied to another camera and put back. */
static void frames(void) {
    float camera[4], viewmodel[4], saved[4], shift[2], p[2], clean[4], q[2];
    jitter_configure(8);
    jitter_next_frame();
    memcpy(camera, WORLD, sizeof camera);
    for (int frame = 0; frame < 40; frame++) {
        jitter_pixels(p);
        jitter_apply(camera, 1280, 720);                  /* the frame's update */
        CHECK(jitter_apply(camera, 1280, 720) == 0, "frame %d: a second update shifted again", frame);
        memcpy(saved, camera, sizeof saved);
        memcpy(camera, FIRST, sizeof camera);             /* first-person field of view */
        jitter_apply(camera, 1280, 720);
        memcpy(viewmodel, camera, sizeof viewmodel);
        memcpy(camera, saved, sizeof camera);             /* and back */
        jitter_apply(camera, 1280, 720);

        memcpy(clean, camera, sizeof clean);
        CHECK(jitter_remove(clean, shift) == 1, "frame %d: the world frustum is not a shifted one", frame);
        CHECK(shift[0] == p[0] && shift[1] == p[1], "frame %d: shift %f %f, this frame's is %f %f", frame, shift[0],
              shift[1], p[0], p[1]);
        CHECK(!memcmp(clean, WORLD, sizeof clean), "frame %d: the frustum drifted to %f %f %f %f", frame, clean[0],
              clean[1], clean[2], clean[3]);
        project(camera, 0, 0, 1280, 720, q);
        CHECK(near(q[0] - 640, p[0], 2e-3f) && near(q[1] - 360, p[1], 2e-3f), "frame %d: centre moved %f %f, wanted %f %f",
              frame, q[0] - 640, q[1] - 360, p[0], p[1]);
        memcpy(clean, viewmodel, sizeof clean);
        CHECK(jitter_remove(clean, shift) == 1 && shift[0] == p[0] && shift[1] == p[1] && !memcmp(clean, FIRST, sizeof clean),
              "frame %d: the first-person frustum has shift %f %f", frame, shift[0], shift[1]);
        project(viewmodel, 0, 0, 1280, 720, q);
        CHECK(near(q[0] - 640, p[0], 2e-3f) && near(q[1] - 360, p[1], 2e-3f),
              "frame %d: first-person centre moved %f %f", frame, q[0] - 640, q[1] - 360);
        jitter_next_frame();
    }
    /* the field of view changes (a zoom): the new frustum is shifted from where the game set it */
    float zoom[4] = {-0.4f, 0.4f, 0.225f, -0.225f};
    memcpy(camera, zoom, sizeof camera);
    jitter_pixels(p);
    jitter_apply(camera, 1280, 720);
    memcpy(clean, camera, sizeof clean);
    CHECK(jitter_remove(clean, shift) == 1 && !memcmp(clean, zoom, sizeof clean) && shift[0] == p[0], "zoomed frustum");
    /* a menu comes up (the bridge passes a width of 0): the frustum goes back as the game set it, and the next
     * frame without a menu is shifted again */
    jitter_next_frame();
    CHECK(jitter_apply(camera, 0, 720) == 1 && !memcmp(camera, zoom, sizeof camera), "a menu left the shift in");
    CHECK(jitter_apply(camera, 0, 720) == 0 && jitter_remove(camera, shift) == 0, "shifted under a menu");
    jitter_next_frame();
    jitter_pixels(p);
    CHECK(jitter_apply(camera, 1280, 720) == 1, "no shift after the menu");
    memcpy(clean, camera, sizeof clean);
    CHECK(jitter_remove(clean, shift) == 1 && !memcmp(clean, zoom, sizeof clean) && shift[0] == p[0] && shift[1] == p[1],
          "the shift after the menu is %f %f, not %f %f", shift[0], shift[1], p[0], p[1]);
    /* jitter goes off: the next update gives the game its frustum back, and after that nothing is touched */
    jitter_next_frame();
    jitter_configure(0);
    CHECK(!jitter_on(), "still on");
    CHECK(jitter_apply(camera, 1280, 720) == 1 && !memcmp(camera, zoom, sizeof camera), "the last shift stayed in");
    CHECK(jitter_apply(camera, 1280, 720) == 0 && !memcmp(camera, zoom, sizeof camera), "touched with jitter off");
    CHECK(jitter_remove(camera, shift) == 0, "an unshifted frustum reported a shift");
}

static void scaled(void) {
    float p[2], q[2];
    jitter_configure(8);
    jitter_next_frame();
    jitter_pixels(p);
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_next_frame();
    jitter_pixels(q);
    CHECK(p[0] == q[0] && p[1] == q[1], "eight frames on, the position is %f %f, not %f %f", q[0], q[1], p[0], p[1]);
    jitter_test_scale(16);
    jitter_next_frame();
    jitter_pixels(p);
    CHECK(fabsf(p[0]) <= 8 && fabsf(p[1]) <= 8 && p[0] == floorf(p[0]), "scaled by 16: %f %f", p[0], p[1]);
    jitter_test_scale(1);
    jitter_configure(0);
}

int main(void) {
    sequence();
    shift_direction();
    frames();
    scaled();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
