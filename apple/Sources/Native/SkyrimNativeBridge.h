// The native Skyrim runtime's C interface (ipad/native_bridge.mm). Only physical iPad builds
// made by ipad/build.py link it; Swift uses it under SKYRIM_NATIVE_RUNTIME.
#ifndef SkyrimNativeBridge_h
#define SkyrimNativeBridge_h
#include <stddef.h>
#include <TargetConditionals.h>

int skyrim_native_runtime_linked(void);
const char *skyrim_native_identity(void);
int skyrim_native_state(void);
int skyrim_native_validate(char *error, size_t error_size);
int skyrim_native_run(const char *save_root, char *error, size_t error_size);
void skyrim_native_set_paused(int paused);
void skyrim_native_request_quit(void);
#if defined(__APPLE__) && TARGET_OS_VISION
/* SDL visionOS overlay (ipad/visionos/sdl): the UIWindowScene SDL's game window must attach to.
 * Pass Unmanaged.passUnretained(scene).toOpaque(); held weakly. Main thread. */
void skyrim_sdl_set_game_scene(void *window_scene);
/* 1 while immersive play runs: SDL's game window turns nearly invisible but keeps taking the trackpad. */
void skyrim_sdl_set_window_ghost(int on);
/* The game window's SwiftUI pointer surface (LB_GAME_SCENE): hover x from -1 to 1, and leaving it. Main thread. */
void skyrim_xr_hover(float x);
void skyrim_xr_hover_left(void);
/* A pinch or trackpad click seen as a spatial event on the immersive layer: 1 down, 0 up. Ignored when GCMouse
 * delivered the same press (runtime/platform_sdl.c). */
void skyrim_spatial_click(int down);

/* ipad/immersive_bridge.mm: the game's frames for the immersive compositor. */
#include <stdint.h>
typedef struct skyrim_immersive_frame {
    void *texture;           /* id<MTLTexture>, unretained; valid while shown */
    void *retired_state;     /* with retired_slot: give back through skyrim_immersive_release once no GPU work reads it */
    uint32_t retired_slot;
    int retired;
    int is_new;
    int needs_srgb_decode;
    uint32_t width;
    uint32_t height;
    void *depth_texture;     /* id<MTLTexture> (R32Float, D3D depth 0 near to 1 far) or NULL; same lifetime */
    /* The game camera that rendered the frame (NiCamera, Skyrim units, Z up): rotation 3x3 row-major whose
     * columns are forward, up, right; position; frustum tangents left, right, top, bottom; near; far. */
    float camera[18];
    int has_camera;
} skyrim_immersive_frame;
void skyrim_immersive_start(void);
void skyrim_immersive_stop(void);
int skyrim_immersive_acquire(skyrim_immersive_frame *out);
void skyrim_immersive_release(void *state, uint32_t slot);
/* runtime/platform_sdl.c head steering: the head's yaw (clockwise) and pitch (up) in radians relative to its
 * starting direction, from the compositor thread; on = 0 stops steering. */
void skyrim_head_set(float yaw, float pitch, int on);
/* The game heading (radians, clockwise from north) that head yaw 0 maps to; *valid = 0 until steering anchors. */
float skyrim_head_offset(int *valid);
/* runtime/motion_path.h (LB_IMMERSIVE_MOTION=1): for a frame whose game camera was at (east, north, up), how far
 * the camera has moved since (game units, game axes) and the steering offset, at the time the display shows what
 * is being drawn (shown_at, seconds on CACurrentMediaTime's clock). Returns 0 when off or nothing is known. */
#ifndef SKYRIM_MOTION_DEFINED
#define SKYRIM_MOTION_DEFINED
typedef struct skyrim_motion {
    float moved[3];
    float offset;
    int has_moved;
    int has_offset;
} skyrim_motion;
typedef struct skyrim_motion_vector { float x, y, z; } skyrim_motion_vector;
#endif
int skyrim_immersive_motion(double shown_at, float east, float north, float up, skyrim_motion *out);
/* A movement in game axes as the headset sees it (x right, y up, z back), in metres. turn = the headset heading
 * that game north maps to (the head's starting heading minus the steering offset), radians clockwise. */
skyrim_motion_vector skyrim_motion_in_headset(float east, float north, float up, float turn, float metres_per_unit);
#endif

#endif
