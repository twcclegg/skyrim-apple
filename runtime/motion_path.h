/* Skyrim Apple port: where the game camera is between the frames it renders, for the immersive compositor
 * (apple/Sources/App/ImmersiveScreen.swift, docs/handoff/camera-motion.md).
 *
 * The compositor redraws at the display's rate and the game renders slower. Head movement is covered because each
 * frame is drawn where its camera pointed; the player's own movement (walking, strafing, turning with the stick)
 * was not: the world held still for a game frame and then jumped. The runtime keeps the camera's recent positions
 * and the head-steering offset with their times, and the compositor asks where they are at the moment it is
 * drawing for and shifts the frame it is showing by the difference.
 *
 * By default the path is read one game frame behind real time, so the answer lies between two positions the game
 * really produced: nothing overshoots when the player stops and the view never goes where the game didn't. If the
 * game is late the path is continued in a straight line for a short, capped time and then held.
 *
 * LB_IMMERSIVE_MOTION=1 turns it on. LB_IMMERSIVE_MOTION_LAG_MS: how far behind real time to read the path
 * (default: one smoothed game frame; 0 reads it at the present moment, which continues the path ahead of the
 * newest frame most of the time). LB_IMMERSIVE_MOTION_AHEAD_MS: the longest the path is continued past the newest
 * frame (default 20). */
#ifndef MOTION_PATH_H
#define MOTION_PATH_H
#ifdef __cplusplus
extern "C" {
#endif

#define MOTION_KEEP 8
/* Samples of a value over time, oldest first. */
typedef struct MotionPath {
    double t[MOTION_KEEP];
    float v[MOTION_KEEP][3];
    int n;
    double interval;  /* smoothed time between samples, seconds; 0 until two have arrived */
} MotionPath;

enum { MOTION_NONE, MOTION_BETWEEN, MOTION_AHEAD, MOTION_HELD };

void motion_path_cut(MotionPath *p);
/* Adds a sample. A change faster than max_rate (units per second) or a gap longer than a quarter second is a cut
 * (a teleport, a loading screen): the path restarts at the sample. Returns 1 if it cut. */
int motion_path_push(MotionPath *p, double t, const float v[3], float max_rate);
/* The value at time t. MOTION_BETWEEN: between two samples. MOTION_AHEAD: continued past the newest sample at the
 * last rate. MOTION_HELD: the continuation reached max_ahead seconds, or there is only one sample, or t is older
 * than everything kept. MOTION_NONE: no samples, out untouched. */
int motion_path_at(const MotionPath *p, double t, double max_ahead, float out[3]);

/* Seconds on the clock the path uses. On Apple systems it is the clock of CACurrentMediaTime and of the
 * compositor's presentation times. */
double motion_now(void);
/* Game thread, once per presented frame: the world camera's position (game units), or NULL when the frame has no
 * world camera (menus, loading). */
void motion_camera(double t, const float pos[3]);
/* The time given with the newest camera position, or 0. */
double motion_camera_time(void);
/* Game thread: the head-steering offset (radians, runtime/platform_sdl.c head_steer) that went with the camera
 * presented at time t; valid = 0 when steering isn't anchored. */
void motion_offset(double t, float offset, int valid);
/* 1 on, 0 off; turning it on or off forgets the paths. For tests: the game reads LB_IMMERSIVE_MOTION. */
void motion_configure(int on, double lag_seconds, double ahead_seconds);

#ifndef SKYRIM_MOTION_DEFINED
#define SKYRIM_MOTION_DEFINED
typedef struct skyrim_motion {
    float moved[3];  /* how far the camera has moved since the frame, game units, game axes (east, north, up) */
    float offset;    /* the head-steering offset now, radians (not wrapped to a turn) */
    int has_moved;
    int has_offset;
} skyrim_motion;
typedef struct skyrim_motion_vector { float x, y, z; } skyrim_motion_vector;
#endif
/* Any thread: for a frame whose camera was at (east, north, up), how far the camera has moved since and what the
 * steering offset is, at the time the display will show what is being drawn (shown_at, on motion_now's clock; 0
 * or a time that isn't plausibly on that clock means now). Returns 0, with nothing set, when the feature is off
 * or nothing is known. */
int skyrim_immersive_motion(double shown_at, float east, float north, float up, skyrim_motion *out);
/* The same with the current time given (tests). */
int motion_query_at(double now, double shown_at, float east, float north, float up, skyrim_motion *out);
/* A movement in game axes as the headset sees it (x right, y up, z back), in metres. turn = the headset heading
 * that game north maps to: the head's starting heading minus the steering offset, radians clockwise. */
skyrim_motion_vector skyrim_motion_in_headset(float east, float north, float up, float turn, float metres_per_unit);

#ifdef __cplusplus
}
#endif
#endif
