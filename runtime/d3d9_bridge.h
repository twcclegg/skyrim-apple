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

/* LB_PASSES (d3d9_bridge.cpp): draws and primitives per kind of pass, logged at each Present. 0 off, 1 the
 * per-frame "pass" line, 2 also "passdbg" lines. The hooks are called only while it is on. */
extern int g_d3d_passes;
void d3d_passes_draw(uint32_t prims);          // every Draw*Primitive*, with its primitive count
void d3d_passes_target(IDirect3DDevice9 *d);   // after the render target 0 or the depth-stencil surface changed
void d3d_passes_clear(uint32_t flags);         // before a Clear, with its D3DCLEAR_* flags
bool hud_screen_in_pass(void);                 // drawing a menu into the second screen's image (hud_screen.cpp)

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
