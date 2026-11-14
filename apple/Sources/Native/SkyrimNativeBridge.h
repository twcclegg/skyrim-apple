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
    /* How far the frame was shifted for temporal upscaling (skyrim_camera_jitter), in its own pixels right and
     * down; the camera above is without the shift. has_jitter = 0 and 0, 0 for a frame that wasn't. */
    float jitter[2];
    int has_jitter;
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
/* runtime/d3d9_bridge.cpp, for temporal upscaling (docs/handoff/temporal-upscale.md); both from any thread.
 * Shifts the world camera by a different fraction of a pixel each frame, over a sequence of `phases` positions;
 * 0 stops it. Takes effect a frame or two later; each frame carries its own shift (jitter above). */
void skyrim_camera_jitter(int phases);
/* Added to the mip level the game's textures are sampled at: log2(render size / shown size), so -1 when the
 * picture is shown at twice the size it was rendered at. 0 = as the game sets it. */
void skyrim_texture_bias(float levels);
#endif

#endif
