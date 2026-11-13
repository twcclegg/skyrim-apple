/* Skyrim Apple port: the game camera's path between rendered frames. See motion_path.h. */
#define _GNU_SOURCE
#include "motion_path.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOTION_GAP 0.25  /* seconds without a sample after which the path restarts */

void motion_path_cut(MotionPath *p) { p->n = 0; }

int motion_path_push(MotionPath *p, double t, const float v[3], float max_rate) {
    int cut = 0;
    if (p->n > 0) {
        int last = p->n - 1;
        double dt = t - p->t[last];
        if (dt <= 0) {  /* the same moment again: keep the newer value */
            memcpy(p->v[last], v, sizeof p->v[last]);
            return 0;
        }
        float dx = v[0] - p->v[last][0], dy = v[1] - p->v[last][1], dz = v[2] - p->v[last][2];
        if (dt > MOTION_GAP || sqrtf(dx * dx + dy * dy + dz * dz) > max_rate * dt) {
            p->n = 0;
            cut = 1;
        } else {
            p->interval = p->interval > 0 ? p->interval + 0.05 * (dt - p->interval) : dt;
        }
    }
    if (p->n == MOTION_KEEP) {
        memmove(p->t, p->t + 1, sizeof p->t - sizeof p->t[0]);
        memmove(p->v, p->v + 1, sizeof p->v - sizeof p->v[0]);
        p->n--;
    }
    p->t[p->n] = t;
    memcpy(p->v[p->n], v, sizeof p->v[p->n]);
    p->n++;
    return cut;
}

static void between(const MotionPath *p, int a, int b, double f, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = (float)(p->v[a][k] + (double)(p->v[b][k] - p->v[a][k]) * f);
}

int motion_path_at(const MotionPath *p, double t, double max_ahead, float out[3]) {
    if (p->n == 0) return MOTION_NONE;
    int last = p->n - 1;
    if (p->n == 1 || t <= p->t[0]) {
        memcpy(out, p->v[p->n == 1 ? last : 0], sizeof p->v[0]);
        return MOTION_HELD;
    }
    if (t >= p->t[last]) {
        double ahead = t - p->t[last];
        int held = ahead > max_ahead;
        if (held) ahead = max_ahead;
        between(p, last - 1, last, 1.0 + ahead / (p->t[last] - p->t[last - 1]), out);
        return held ? MOTION_HELD : MOTION_AHEAD;
    }
    int i = last - 1;
    while (i > 0 && p->t[i] > t) i--;
    between(p, i, i + 1, (t - p->t[i]) / (p->t[i + 1] - p->t[i]), out);
    return MOTION_BETWEEN;
}

double motion_now(void) {
    struct timespec ts;
#if defined(__APPLE__)
    clock_gettime(CLOCK_UPTIME_RAW, &ts);  /* mach_absolute_time: what CACurrentMediaTime and the compositor use */
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static MotionPath g_camera, g_offset;
static double g_camera_time;
static int g_config;            /* 0 not read yet, 1 on, -1 off */
static double g_lag = -1;       /* seconds; negative = one smoothed game frame */
static double g_ahead = 0.020;
static double g_lead;           /* smoothed time from drawing to showing, seconds */
static int g_lead_known, g_clock_warned;
static const float g_max_speed = 4000;  /* game units per second (57 m/s); faster is a teleport */
static const float g_max_turn = 8;      /* radians per second; faster is a re-anchor, not a turn */
static const float g_max_moved = 1000;  /* game units (14 m): a frame further than this from the path isn't on it */
static unsigned g_states[4], g_cuts, g_far;
static double g_moved_sum, g_logged;

static void forget(void) {
    motion_path_cut(&g_camera);
    motion_path_cut(&g_offset);
    g_camera.interval = g_offset.interval = 0;
    g_camera_time = 0;
    g_lead_known = 0;
    memset(g_states, 0, sizeof g_states);
    g_cuts = g_far = 0;
    g_moved_sum = g_logged = 0;
}

void motion_configure(int on, double lag_seconds, double ahead_seconds) {
    pthread_mutex_lock(&g_lock);
    g_config = on ? 1 : -1;
    g_lag = lag_seconds;
    g_ahead = ahead_seconds < 0 ? 0 : ahead_seconds;
    forget();
    pthread_mutex_unlock(&g_lock);
}

/* Read on the game thread, with the lock held: the launch environment is applied before the game starts, but
 * after the compositor does. */
static int configured(void) {
    if (!g_config) {
        const char *on = getenv("LB_IMMERSIVE_MOTION"), *lag = getenv("LB_IMMERSIVE_MOTION_LAG_MS"),
                   *ahead = getenv("LB_IMMERSIVE_MOTION_AHEAD_MS");
        g_config = on && atoi(on) > 0 ? 1 : -1;
        if (lag && *lag) g_lag = atof(lag) / 1000.0;
        if (ahead && *ahead) g_ahead = fmax(0.0, atof(ahead)) / 1000.0;
        if (g_config > 0) {
            if (g_lag >= 0) fprintf(stderr, "immersive: camera motion on, read %.0f ms back", g_lag * 1000);
            else fprintf(stderr, "immersive: camera motion on, read one game frame back");
            fprintf(stderr, ", continued at most %.0f ms\n", g_ahead * 1000);
        }
    }
    return g_config > 0;
}

void motion_camera(double t, const float pos[3]) {
    pthread_mutex_lock(&g_lock);
    if (configured()) {
        if (pos) {
            g_cuts += motion_path_push(&g_camera, t, pos, g_max_speed);
            g_camera_time = t;
        } else {
            motion_path_cut(&g_camera);
            g_camera_time = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

double motion_camera_time(void) {
    pthread_mutex_lock(&g_lock);
    double t = g_camera_time;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void motion_offset(double t, float offset, int valid) {
    pthread_mutex_lock(&g_lock);
    if (configured()) {
        if (valid && t > 0) {
            float v[3] = {offset, 0, 0};
            if (g_offset.n > 0) {  /* the nearest turn from the last value, so the path doesn't jump at +-pi */
                float last = g_offset.v[g_offset.n - 1][0], d = offset - last;
                v[0] = last + d - 6.2831853f * roundf(d / 6.2831853f);
            }
            motion_path_push(&g_offset, t, v, g_max_turn);
        } else {
            motion_path_cut(&g_offset);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

int motion_query_at(double now, double shown_at, float east, float north, float up, skyrim_motion *out) {
    unsigned states[4] = {0}, cuts = 0, far = 0;
    double lag = 0, moved_sum = 0;
    int report = 0, warn = 0;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_lock);
    if (g_config > 0) {
        /* The display shows what is drawn now a little later, and how much later wobbles less than the moment
         * drawing starts does. Reading the path at the display's time, moved back by the smoothed gap, steps it
         * by exactly one display frame each time while staying at the present on average. */
        double at = now;
        if (shown_at > 0) {
            double lead = shown_at - now;
            if (lead > -0.05 && lead < 0.25) {
                g_lead = g_lead_known && fabs(lead - g_lead) < 0.03 ? g_lead + 0.05 * (lead - g_lead) : lead;
                g_lead_known = 1;
                at = shown_at - g_lead;
            } else if (!g_clock_warned) {
                g_clock_warned = warn = 1;
            }
        }
        lag = g_lag >= 0 ? g_lag : g_camera.interval;
        float p[3], o[3];
        int state = motion_path_at(&g_camera, at - lag, g_ahead, p);
        if (state != MOTION_NONE) {
            float m[3] = {p[0] - east, p[1] - north, p[2] - up};
            float length = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            if (length <= g_max_moved) {
                memcpy(out->moved, m, sizeof m);
                out->has_moved = 1;
                g_moved_sum += length;
            } else {
                g_far++;  /* the frame is from before a cut: show it where it is */
            }
        }
        if (motion_path_at(&g_offset, at - lag, g_ahead, o) != MOTION_NONE) {
            out->offset = o[0];
            out->has_offset = 1;
        }
        g_states[state]++;
        if (now - g_logged >= 5) {
            report = g_logged > 0;
            g_logged = now;
            memcpy(states, g_states, sizeof states);
            cuts = g_cuts;
            far = g_far;
            moved_sum = g_moved_sum;
            memset(g_states, 0, sizeof g_states);
            g_cuts = g_far = 0;
            g_moved_sum = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (warn)
        fprintf(stderr, "immersive: camera motion: the display time (%.3f) isn't on the runtime's clock (%.3f); "
                "using the time of drawing\n", shown_at, now);
    unsigned known = states[MOTION_BETWEEN] + states[MOTION_AHEAD] + states[MOTION_HELD];
    if (report && known)
        fprintf(stderr, "immersive: camera motion read %.0f ms back: %u%% between frames, %u%% continued, %u%% held; "
                "mean shift %.1f units, %u cuts, %u frames off the path\n", lag * 1000,
                states[MOTION_BETWEEN] * 100 / known, states[MOTION_AHEAD] * 100 / known,
                states[MOTION_HELD] * 100 / known, moved_sum / known, cuts, far);
    return out->has_moved || out->has_offset;
}

int skyrim_immersive_motion(double shown_at, float east, float north, float up, skyrim_motion *out) {
    return motion_query_at(motion_now(), shown_at, east, north, up, out);
}

/* The compositor draws a frame whose game camera had heading h (clockwise from north) at headset heading
 * h + turn, where headset heading a looks along (sin a, 0, -cos a). So north goes to (sin turn, 0, -cos turn),
 * east to (cos turn, 0, sin turn) and up to (0, 1, 0). */
skyrim_motion_vector skyrim_motion_in_headset(float east, float north, float up, float turn, float metres_per_unit) {
    float s = sinf(turn), c = cosf(turn);
    skyrim_motion_vector v = {(east * c + north * s) * metres_per_unit, up * metres_per_unit,
                              (east * s - north * c) * metres_per_unit};
    return v;
}
