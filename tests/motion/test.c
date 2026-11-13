/* Checks for runtime/motion_path.c: the camera's path between rendered frames. No game code or data.
 * Each check prints a line; the exit status is the number that failed. */
#include "motion_path.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
#define CHECK(ok, ...) do { \
    int ok_ = (ok); \
    printf("%s  ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
    if (!ok_) g_failed++; \
} while (0)

static unsigned g_seed = 12345;
static double noise(void) {  /* -1..1, the same every run */
    g_seed = g_seed * 1664525u + 1013904223u;
    return (double)(g_seed >> 8) / 8388608.0 - 1.0;
}

#define DISPLAY (1.0 / 90.0)

/* A game presenting frames while a display asks for the camera at its own rate. speed(t) is the player's speed
 * along east at time t; frame_ms the game's frame time; jitter the fraction it varies by. Returns through the
 * pointers the largest difference between one display step and the mean step, with the path and with the newest
 * frame held, and the largest distance past the true position. */
typedef double (*Speed)(double t);
static double run_speed(double t) { (void)t; return 370; }                       /* running: 5.3 m/s */
static double stop_speed(double t) { return t < 1.0 ? 370 : 0; }                  /* runs for a second, then stops */

static double position_at(Speed speed, double t) {
    double x = 0;
    for (double s = 0; s < t; s += 0.0005) x += speed(s) * 0.0005;
    return x;
}

static void play(Speed speed, double frame_ms, double jitter, double seconds, double *path_wobble,
                 double *held_wobble, double *overshoot, double *behind_ms) {
    motion_configure(1, -1, 0.020);
    double next_frame = 0.01, frame_x = 0, last_path = 0, last_held = 0;
    double steps_path[4096], steps_held[4096], behind = 0;
    int n = 0, known = 0, behind_n = 0;
    *overshoot = 0;
    for (double t = 0.01; t < seconds && n < 4096; t += DISPLAY) {
        while (next_frame <= t) {
            frame_x = position_at(speed, next_frame);
            float p[3] = {(float)frame_x, 0, 0};
            motion_camera(next_frame, p);
            next_frame += frame_ms / 1000.0 * (1.0 + jitter * noise());
        }
        skyrim_motion m;
        /* Drawing starts up to 2 ms late; the display time it draws for is exact. */
        double now = t + 0.002 * fabs(noise());
        if (!motion_query_at(now, t + 0.030, (float)frame_x, 0, 0, &m) || !m.has_moved) continue;
        double x = frame_x + m.moved[0], truth = position_at(speed, t);
        if (x - truth > *overshoot) *overshoot = x - truth;
        if (known > 20) {  /* after the first frames, once the frame interval is known */
            steps_path[n] = x - last_path;
            steps_held[n] = frame_x - last_held;
            if (speed(t) > 0) { behind += (truth - x) / speed(t); behind_n++; }
            n++;
        }
        last_path = x;
        last_held = frame_x;
        known++;
    }
    double mean_path = 0, mean_held = 0;
    for (int i = 0; i < n; i++) { mean_path += steps_path[i] / n; mean_held += steps_held[i] / n; }
    *path_wobble = *held_wobble = 0;
    for (int i = 0; i < n; i++) {
        *path_wobble = fmax(*path_wobble, fabs(steps_path[i] - mean_path));
        *held_wobble = fmax(*held_wobble, fabs(steps_held[i] - mean_held));
    }
    *behind_ms = behind_n ? behind / behind_n * 1000 : 0;
}

int main(void) {
    float v[3], a[3] = {0, 0, 0}, b[3] = {10, 20, 30}, c[3] = {20, 40, 60};
    skyrim_motion m;

    /* One path. */
    MotionPath p = {0};
    CHECK(motion_path_at(&p, 1.0, 0.02, v) == MOTION_NONE, "an empty path has no value");
    motion_path_push(&p, 1.00, a, 4000);
    CHECK(motion_path_at(&p, 1.5, 0.02, v) == MOTION_HELD && v[0] == 0, "one sample is held");
    motion_path_push(&p, 1.02, b, 4000);
    motion_path_push(&p, 1.04, c, 4000);
    CHECK(motion_path_at(&p, 1.03, 0.02, v) == MOTION_BETWEEN && fabsf(v[0] - 15) < 1e-3f && fabsf(v[2] - 45) < 1e-3f,
          "halfway between two samples: %.2f %.2f %.2f", v[0], v[1], v[2]);
    CHECK(motion_path_at(&p, 1.05, 0.02, v) == MOTION_AHEAD && fabsf(v[0] - 25) < 1e-3f,
          "10 ms past the newest sample continues at the last rate: %.2f", v[0]);
    CHECK(motion_path_at(&p, 1.50, 0.02, v) == MOTION_HELD && fabsf(v[0] - 30) < 1e-3f,
          "long past the newest sample stops after 20 ms: %.2f", v[0]);
    CHECK(motion_path_at(&p, 0.50, 0.02, v) == MOTION_HELD && v[0] == 0, "before the oldest sample holds the oldest");
    float far[3] = {5000, 0, 0};
    CHECK(motion_path_push(&p, 1.06, far, 4000) == 1 && p.n == 1, "a jump faster than the limit restarts the path");
    float near[3] = {5001, 0, 0};
    CHECK(motion_path_push(&p, 1.50, near, 4000) == 1 && p.n == 1, "a gap of over a quarter second restarts the path");
    for (int i = 0; i < 30; i++) {
        float s[3] = {5001 + (float)i, 0, 0};
        motion_path_push(&p, 1.50 + 0.02 * (i + 1), s, 4000);
    }
    CHECK(p.n == MOTION_KEEP && motion_path_at(&p, 2.09, 0.02, v) == MOTION_BETWEEN && fabsf(v[0] - 5029.5f) < 1e-2f,
          "a full path keeps the newest samples: %.2f", v[0]);

    /* Off unless asked for. */
    motion_configure(0, -1, 0.02);
    motion_camera(1.0, a);
    CHECK(motion_query_at(1.0, 0, 0, 0, 0, &m) == 0 && !m.has_moved && !m.has_offset, "off: nothing is reported");

    /* Running at 45 game frames a second with frame times varying by a fifth, shown at 90 Hz. */
    double path, held, over, behind;
    play(run_speed, 22.2, 0.2, 4.0, &path, &held, &over, &behind);
    CHECK(path < 0.6 && held > 3.0, "running at 45 fps: display steps vary by %.2f units with the path, %.2f holding "
          "the newest frame (a step is %.2f)", path, held, 370 * DISPLAY);
    CHECK(over < 0.5, "running: never more than %.2f units ahead of the player", over);
    CHECK(behind > 5 && behind < 45, "running: %.1f ms behind the player on average", behind);
    play(run_speed, 33.3, 0.2, 4.0, &path, &held, &over, &behind);
    CHECK(path < 0.9 && held > 4.0, "running at 30 fps: steps vary by %.2f units with the path, %.2f held", path, held);
    play(stop_speed, 22.2, 0.2, 2.0, &path, &held, &over, &behind);
    CHECK(over < 0.5, "stopping: the view passes the player by at most %.2f units", over);

    /* The game stalls: the path continues for 20 ms and then waits. */
    motion_configure(1, 0.0222, 0.020);
    for (int i = 0; i < 6; i++) {
        float s[3] = {370 * 0.0222f * i, 0, 0};
        motion_camera(1.0 + 0.0222 * i, s);
    }
    double newest_t = 1.0 + 0.0222 * 5, newest_x = 370 * 0.0222 * 5;
    motion_query_at(newest_t + 0.0222 + 0.010, 0, (float)newest_x, 0, 0, &m);
    CHECK(m.has_moved && fabsf(m.moved[0] - 3.7f) < 0.05f, "a late frame: 10 ms on, the camera is taken %.2f units on", m.moved[0]);
    motion_query_at(newest_t + 0.2, 0, (float)newest_x, 0, 0, &m);
    CHECK(m.has_moved && fabsf(m.moved[0] - 7.4f) < 0.05f, "a stall: the camera is taken no further than 20 ms on (%.2f units)", m.moved[0]);

    /* A fast-travel: the frame still on screen is from before it and is left where it is. */
    float there[3] = {90000, 0, 0};
    motion_camera(newest_t + 0.0222, there);
    motion_query_at(newest_t + 0.03, 0, (float)newest_x, 0, 0, &m);
    CHECK(!m.has_moved, "after a teleport the old frame isn't shifted");
    motion_query_at(newest_t + 0.03, 0, 90000, 0, 0, &m);
    CHECK(m.has_moved && m.moved[0] == 0, "and the new frame starts a new path");
    motion_camera(newest_t + 0.05, NULL);
    CHECK(motion_query_at(newest_t + 0.06, 0, 90000, 0, 0, &m) == 0, "a frame without the world camera ends the path");

    /* The steering offset while the stick turns the view through south (+-pi). */
    motion_configure(1, 0.0222, 0.020);
    motion_camera(2.0, a);
    motion_offset(2.0, 3.10f, 1);
    motion_camera(2.0222, a);
    motion_offset(2.0222, -3.12f, 1);  /* 3.10 + 0.063 wrapped */
    motion_query_at(2.0222 + 0.0111, 0, 0, 0, 0, &m);
    CHECK(m.has_offset && fabsf(m.offset - 3.1316f) < 2e-3f, "offset halfway across +-pi takes the short way: %.4f", m.offset);
    motion_offset(2.0444, 1.0f, 1);
    motion_query_at(2.0444 + 0.0111, 0, 0, 0, 0, &m);
    CHECK(m.has_offset && fabsf(m.offset - 1.0f) < 1e-3f, "a re-anchor isn't blended in: %.4f", m.offset);
    motion_offset(2.0666, 0, 0);
    motion_query_at(2.07, 0, 0, 0, 0, &m);
    CHECK(!m.has_offset, "steering let go: no offset");

    /* The display's time: even steps however late drawing starts; a time from another clock is ignored. */
    motion_configure(1, 0.0222, 0.020);
    for (int i = 0; i < 8; i++) {
        float s[3] = {50.0f * i, 0, 0};
        motion_camera(3.0 + 0.0222 * i, s);
    }
    double step_min = 1e9, step_max = -1e9, last = 0;
    for (int i = 0; i < 6; i++) {
        double shown = 3.1 + DISPLAY * i, now = shown - 0.030 + (i % 2 ? 0.004 : 0.0);
        motion_query_at(now, shown, 0, 0, 0, &m);
        if (i > 0) { step_min = fmin(step_min, m.moved[0] - last); step_max = fmax(step_max, m.moved[0] - last); }
        last = m.moved[0];
    }
    CHECK(step_max - step_min < 1.5 && step_min > 22, "drawing starts 0 or 4 ms late: steps of %.2f to %.2f units (%.2f is one "
          "display frame)", step_min, step_max, 50 / 0.0222 * DISPLAY);
    motion_query_at(3.14, 86400.0, 0, 0, 0, &m);
    skyrim_motion at_now;
    motion_query_at(3.14, 0, 0, 0, 0, &at_now);
    CHECK(m.has_moved && m.moved[0] == at_now.moved[0], "a display time off the clock falls back to now");

    /* Game axes to headset axes, against the compositor's own placement: a frame whose game camera has heading h
     * is drawn facing headset heading h + turn, which looks along (sin, 0, -cos) with right = (cos, 0, sin). */
    int ok = 1;
    for (int i = 0; i < 24 && ok; i++) {
        float h = -3.0f + 0.37f * i, turn = 2.0f - 0.29f * i, heading = h + turn;
        skyrim_motion_vector f = skyrim_motion_in_headset(70 * sinf(h), 70 * cosf(h), 0, turn, 1.0f / 70);
        skyrim_motion_vector r = skyrim_motion_in_headset(70 * cosf(h), -70 * sinf(h), 0, turn, 1.0f / 70);
        skyrim_motion_vector u = skyrim_motion_in_headset(0, 0, 70, turn, 1.0f / 70);
        ok = fabsf(f.x - sinf(heading)) < 1e-4f && fabsf(f.y) < 1e-6f && fabsf(f.z + cosf(heading)) < 1e-4f
          && fabsf(r.x - cosf(heading)) < 1e-4f && fabsf(r.z - sinf(heading)) < 1e-4f
          && u.x == 0 && fabsf(u.y - 1) < 1e-6f && u.z == 0;
    }
    CHECK(ok, "a metre forward, right and up in the game is a metre forward, right and up in the headset");

    printf("%d failed\n", g_failed);
    return g_failed;
}
