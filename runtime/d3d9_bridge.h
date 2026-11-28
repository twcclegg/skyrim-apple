// Shared declarations for the guest<->host Direct3D 9 bridge (C++ side).
#pragma once
#include <d3d9.h>
#include <cstdint>
#include <cstring>

extern "C" {
#include "rt.h"
}
#include "d3d9_ifaces.h"

// Guest COM objects: { vtable, host id }, vtables are arrays of thunks.
void com_register_iface(int iface, const char *name, const ShimFn *fns, int n);
void *com_host(uint32_t guest);
uint32_t com_wrap(void *host, int iface);
void com_QueryInterface(CPU *c);
void com_AddRef(CPU *c);
void com_Release(CPU *c);
uint32_t d3d9_wrap(void *host, int iface);
int d3d9_iface_from_iid(const GUID &iid);

template <class T, class R, class... P, class... A>
static inline R bridge_call(T *self, R (STDMETHODCALLTYPE T::*m)(P...), A... a) {
    return (self->*m)(((P)(a))...);
}

extern int g_d3d_trace, g_bisect, g_d3d_draws;  /* g_d3d_draws: draw calls since the last Present */
bool d3d_skip_draw(void);
void d3d_trace(CPU *c, const char *name, int nargs);
void d3d_trace_camera(const char *why);  // LB_D3DTRACE: the game's world camera near/far and FOVs at this call

/* LB_PASSES (d3d9_bridge.cpp): draws and primitives per kind of pass, logged at each Present. 0 off, 1 the
 * per-frame "pass" line, 2 also "passdbg" and "fpdbg" lines. The hooks are called only while g_d3d_passes is set:
 * with LB_PASSES, or without logs when the first-person flag is wanted (LB_FP=1, LB_FP_SKIP, LB_STEREO_EYE_UNITS,
 * LB_SKIP_CLASS, LB_DEPTH_COPY), or with LB_PASS_STATS (its own lines only).
 * LB_PASSES_GPU=1 (with LB_PASSES) adds "passgpu" lines with GPU ms per class from timestamp queries. */
extern int g_d3d_passes;
// every Draw*Primitive*, with its primitive count and, for DrawPrimitiveUP / DrawIndexedPrimitiveUP, the vertex
// stride it passes (-1 for the others)
void d3d_passes_draw(uint32_t prims, int up_stride);
void d3d_passes_target(IDirect3DDevice9 *d);   // after the render target 0 or the depth-stencil surface changed
void d3d_passes_clear(uint32_t flags);         // before a Clear, with its D3DCLEAR_* flags
void d3d_passes_projection(const float *m);    // SetTransform(D3DTS_PROJECTION), the guest's matrix
bool hud_screen_in_pass(void);                 // drawing a menu into the second screen's image (hud_screen.cpp)
/* First-person draws (arms, weapon, spell hand and its effect; d3d9_bridge.cpp): whether the draw being made is one,
 * and of which kind. C callers: skyrim_first_person_pass() returns the kind, skyrim_first_person_listen(fn) has fn
 * called with the new kind on each change (on the device thread, nullptr to stop). */
enum FpKind {
    FP_NONE,     // not a first-person draw
    FP_COLOUR,   // the late pass, lit, into the scene target (A16B16G16R16F, before tone mapping)
    FP_DEPTH,    // the late pass, depth, into the scene's R32F depth target
    FP_OTHER,    // the late pass into another back-buffer-sized target (Flames: one A8R8G8B8 draw)
    FP_PREPASS,  // depth only, first in the main pass: hides the world behind the hands
    FP_MASK,     // into the smaller (960x540 at 720p) pass, depth then red only (sun-shadow mask, inferred)
    FP_KINDS
};
bool d3d_first_person(void);
extern "C" int skyrim_first_person_pass(void);
extern "C" void skyrim_first_person_listen(void (*fn)(int kind));

/* First-person depth (d3d9_bridge.cpp): the first-person meshes' depth is written in the world's encoding. The
 * hooks are called only while g_d3d_fp is set, which it is unless LB_FP_DEPTH_SCALE=0. */
extern int g_d3d_fp;
/* LB_FP_FLAME_DEPTH (live hybrid tier A, d3d9_bridge.cpp): before each first-person effect draw (blended, into the
 * scene colour target) the same draw into the scene depth target, stamping the hand's distance where it is bright. */
extern int g_fp_flame;
void d3d_fp_flame_draw(IDirect3DDevice9 *d, int indexed, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                       uint32_t a5, uint32_t a6);
void d3d_fp_draw(void);                     // every Draw*Primitive*, before the draw
void d3d_fp_target(IDirect3DDevice9 *d);    // after the render target 0 or the depth-stencil surface changed
void d3d_fp_clear(uint32_t flags);          // before a Clear, with its D3DCLEAR_* flags
void d3d_fp_projection(const float *m);     // SetTransform(D3DTS_PROJECTION), the guest's matrix
bool hud_screen_in_pass(void);              // drawing a menu into the second screen's image (hud_screen.cpp)

extern "C" {
void *platform_window(void);
void platform_present_hook(void);
void platform_present_begin(void);
void platform_present_end(void);
/* time the 60 fps pacing slept after this Present, for the benchmark's per-frame work time */
void platform_present_slept(double ms);
/* draw calls the game made for the frame being presented, for the benchmark's per-frame log */
void platform_present_draws(int draws);
int platform_frame_count(void);
}
