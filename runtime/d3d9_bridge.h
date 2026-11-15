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

extern int g_d3d_trace, g_bisect;
bool d3d_skip_draw(void);
void d3d_trace(CPU *c, const char *name, int nargs);

/* First-person depth (d3d9_bridge.cpp): the first-person meshes' depth is written in the world's encoding. The
 * hooks are called only while g_d3d_fp is set, which it is unless LB_FP_DEPTH_SCALE=0. */
extern int g_d3d_fp;
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
}
