// Second screen HUD ("Skyrim Dual"): on a phone with a second display most
// of the HUD and the favourites go to that screen.
//
// The Java side (SkyrimActivity, dual build) shows a Presentation with a
// SurfaceView on the other display and hands its Surface to
// nativeHudSurface(). Without one, everything stays on the main screen.
//
// Both menus render through IMenu::Render (0x847000, vtable slot 6:
// view->Display()); their vtable slots point at hud_render instead.
//
// The HUD movie is drawn twice per frame. On the main screen only the
// enemy health bar, crosshair, sneak eye and floating quest markers are
// visible; on the second screen everything else. The player's health,
// magicka and stamina bars are stacked vertically there and held at their
// fully shown frame, so they never fade out. Visibility and positions are set
// through the movie's own GFxMovieView (SetVariable/GetVariable/Invoke).
//
// The favourites menu is drawn only to the second screen, into its own image
// that stays there after the menu closes, so the favourites list remains
// visible (as of the last time it was open).
//
// A second-screen pass swaps the render target the game had bound for ours
// and scales viewports, scissor rects and clear rects by an integer factor,
// so the menus are drawn sharper than the game's back buffer. At Present the
// two images are laid out on the second display's dxvk swap chain (see dxvk's
// wsi_native_surface.h).
#include "d3d9_bridge.h"
#include <pthread.h>
#include <initializer_list>
#include <SDL2/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

extern "C" void dxvk_native_surface(void *hwnd, void *window, uint32_t width, uint32_t height);

namespace {
const uint32_t IMENU_RENDER = 0x847000;
const uint32_t VT_HUD = 0x10E537C, VT_FAVORITES = 0x10E4E18;
void *const HUD_HWND = (void *)(uintptr_t)0x48554431;  // "HUD1", only a key for dxvk

// GFxMovieView (IMenu + 8) vtable offsets, as the game calls them.
const uint32_t GFX_SETVARIABLE = 0x40;  // (name, const GFxValue *, flags)
const uint32_t GFX_GETVARIABLE = 0x44;  // (GFxValue *out, name) -> bool
const uint32_t GFX_INVOKE = 0x5C;       // (name, GFxValue *result, GFxValue *args, n)
// GFxValue: {object interface, type, data (8 bytes)}; types 2 bool, 3 number.

// HUD movie parts (children of HUDMovieBaseInstance, from hudmenu.gfx).
const char *const k_main_parts[] = {"EnemyHealth_mc", "Crosshair", "StealthMeterInstance",
                                    "FloatingQuestMarkerInstance"};
const char *const k_second_parts[] = {
    "TutorialLockInstance", "LocationLockBase", "CompassShoutMeterHolder", "SubtitleTextHolder",
    "RolloverNameInstance", "RolloverInfoInstance", "GrayBarInstance", "ActivateButton_tf",
    "WeightTranslated", "ValueTranslated", "FavorBackButtonBase", "Health", "Magica", "Stamina",
    "BottomLeftLockInstance", "BottomRightLockInstance", "ArrowInfoInstance", "MessagesBlock"};
const int N_MAIN = sizeof k_main_parts / sizeof *k_main_parts;
const int N_SECOND = sizeof k_second_parts / sizeof *k_second_parts;
// Player bars, stacked at the bottom left of the movie (HUDMovieBaseInstance
// coordinates; the stage is 1280x720 with the base at 616.6,475.4). Each
// bar's left edge goes to x = -544.6, rows 30 apart. Bounds of the three
// meter clips: Health -142..143, Magica 46..339, Stamina (mirrored) -339..-46,
// all about y -40..-16.
struct Bar { const char *name; double x, y; };
const Bar k_bars[] = {{"Health", -402.6, 164.7}, {"Magica", -590.6, 194.8}, {"Stamina", -205.6, 224.6}};
const double METER_SHOWN_FRAME = 40;  // HUDMenu.METER_PAUSE_FRAME: fully faded in

// The second display's surface, from the Java thread.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
void *g_window;                 // ANativeWindow *, owned here
uint32_t g_win_w, g_win_h;
int g_window_gen;               // bumped on every change
int g_seen_gen;                 // the generation the render thread is using

// Render thread state (the game's render calls come from one thread).
IDirect3DDevice9 *g_dev;
IDirect3DSwapChain9 *g_chain;
struct Target { IDirect3DTexture9 *tex; IDirect3DSurface9 *surf; bool valid; };
Target g_hud, g_fav;            // second-screen images
IDirect3DSurface9 *g_ds;        // shared depth/stencil (masks)
uint32_t g_rt_w, g_rt_h, g_k = 1;   // target size, scale over the back buffer
uint32_t g_bb_w, g_bb_h;
bool g_dirty;                   // second screen needs a new frame
bool g_hud_moved;               // the HUD went to the second screen this frame

// The second-screen pass in progress.
bool g_pass, g_on_rt;
pthread_t g_pass_thread;
IDirect3DSurface9 *g_rt;                   // target of the pass
IDirect3DSurface9 *g_from_rt, *g_from_ds;  // what the game had bound
IDirect3DSurface9 *g_from_bb;              // the back buffer, also redirected
D3DVIEWPORT9 g_guest_vp;
RECT g_guest_sc;

// Guest memory for GFx calls.
uint32_t g_gv;                  // 4 GFxValues
uint32_t g_str_vis[N_MAIN + N_SECOND], g_str_x[3], g_str_y[3], g_str_alpha[3], g_str_goto[3];

bool in_pass() { return g_pass && pthread_equal(pthread_self(), g_pass_thread); }

void release_target(Target &t) {
    if (t.surf) { t.surf->Release(); t.surf = nullptr; }
    if (t.tex) { t.tex->Release(); t.tex = nullptr; }
    t.valid = false;
}
void release_targets() {
    release_target(g_hud);
    release_target(g_fav);
    if (g_ds) { g_ds->Release(); g_ds = nullptr; }
    g_rt_w = g_rt_h = 0;
}
void release_chain() {
    if (g_chain) { g_chain->Release(); g_chain = nullptr; }
    dxvk_native_surface(HUD_HWND, nullptr, 0, 0);
}

// Picks up a new or lost surface. Called from Present on the render thread.
void sync_window(IDirect3DDevice9 *d) {
    pthread_mutex_lock(&g_lock);
    if (g_seen_gen != g_window_gen) {
        release_chain();
        g_seen_gen = g_window_gen;
        if (g_window && g_win_w && g_win_h) {
            dxvk_native_surface(HUD_HWND, g_window, g_win_w, g_win_h);
            D3DPRESENT_PARAMETERS pp = {};
            pp.BackBufferWidth = g_win_w;
            pp.BackBufferHeight = g_win_h;
            pp.BackBufferFormat = D3DFMT_X8R8G8B8;
            pp.BackBufferCount = 1;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = (HWND)HUD_HWND;
            pp.Windowed = TRUE;
            // never wait for the second display's vsync on the game thread
            pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            HRESULT hr = d->CreateAdditionalSwapChain(&pp, &g_chain);
            LOG("hud screen: %ux%u swap chain -> %08x", g_win_w, g_win_h, (unsigned)hr);
            if (FAILED(hr)) { g_chain = nullptr; dxvk_native_surface(HUD_HWND, nullptr, 0, 0); }
            g_dirty = true;
        }
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

bool make_target(IDirect3DDevice9 *d, Target &t) {
    if (FAILED(d->CreateTexture(g_rt_w, g_rt_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &t.tex, nullptr)) ||
        FAILED(t.tex->GetSurfaceLevel(0, &t.surf)))
        return false;
    t.valid = false;
    return true;
}

bool ensure_targets(IDirect3DDevice9 *d, IDirect3DSurface9 *from) {
    D3DSURFACE_DESC desc;
    if (!from || FAILED(from->GetDesc(&desc))) return false;
    g_bb_w = desc.Width; g_bb_h = desc.Height;
    uint32_t kw = g_win_w / (g_bb_w ? g_bb_w : 1), kh = g_win_h / (g_bb_h ? g_bb_h : 1);
    uint32_t k = kw > kh ? kw : kh;  // the images are cropped/laid out, so the larger
    k = k < 1 ? 1 : k > 4 ? 4 : k;
    if (g_hud.surf && g_rt_w == g_bb_w * k && g_rt_h == g_bb_h * k) return true;
    release_targets();
    g_k = k;
    g_rt_w = g_bb_w * k; g_rt_h = g_bb_h * k;
    if (!make_target(d, g_hud) || !make_target(d, g_fav) ||
        FAILED(d->CreateDepthStencilSurface(g_rt_w, g_rt_h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_ds,
                                            nullptr))) {
        LOG("hud screen: cannot create a %ux%u target", g_rt_w, g_rt_h);
        release_targets();
        return false;
    }
    LOG("hud screen: menus drawn at %ux%u (%ux the %ux%u back buffer)", g_rt_w, g_rt_h, k, g_bb_w, g_bb_h);
    return true;
}

void scale_vp(D3DVIEWPORT9 *v) { v->X *= g_k; v->Y *= g_k; v->Width *= g_k; v->Height *= g_k; }
void scale_rect(RECT *r) { r->left *= g_k; r->top *= g_k; r->right *= g_k; r->bottom *= g_k; }

void bind_rt(IDirect3DDevice9 *d) {
    d->SetRenderTarget(0, g_rt);  // also resets viewport and scissor to the whole target
    g_on_rt = true;
    g_guest_vp = {0, 0, g_bb_w, g_bb_h, 0, 1};
    g_guest_sc = {0, 0, (LONG)g_bb_w, (LONG)g_bb_h};
}

// ---- GFx calls on the menu's movie (thiscall through its vtable)
uint32_t guest_str(const char *s) {
    uint32_t g = gmalloc((uint32_t)strlen(s) + 1);
    memcpy(MEM + g, s, strlen(s) + 1);
    return g;
}
void gfx_strings() {
    if (g_gv) return;
    g_gv = gmalloc(64);
    char b[128];
    for (int i = 0; i < N_MAIN + N_SECOND; i++) {
        snprintf(b, sizeof b, "HUDMovieBaseInstance.%s._visible", i < N_MAIN ? k_main_parts[i] : k_second_parts[i - N_MAIN]);
        g_str_vis[i] = guest_str(b);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(b, sizeof b, "HUDMovieBaseInstance.%s._x", k_bars[i].name); g_str_x[i] = guest_str(b);
        snprintf(b, sizeof b, "HUDMovieBaseInstance.%s._y", k_bars[i].name); g_str_y[i] = guest_str(b);
        snprintf(b, sizeof b, "HUDMovieBaseInstance.%s._alpha", k_bars[i].name); g_str_alpha[i] = guest_str(b);
        snprintf(b, sizeof b, "HUDMovieBaseInstance.%s.gotoAndStop", k_bars[i].name); g_str_goto[i] = guest_str(b);
    }
}
uint32_t gfx_call(CPU *c, uint32_t view, uint32_t off, int n, const uint32_t *args) {
    c->r[1] = view;
    return rt_invoke(c, M32(M32(view) + off), n, args);
}
void gv_bool(uint32_t v, bool b) { M32(v) = 0; M32(v + 4) = 2; M32(v + 8) = b; M32(v + 12) = 0; }
void gv_num(uint32_t v, double x) { M32(v) = 0; M32(v + 4) = 3; memcpy(MEM + v + 8, &x, 8); }
void set_bool(CPU *c, uint32_t view, uint32_t name, bool b) {
    gv_bool(g_gv, b);
    uint32_t a[3] = {name, g_gv, 0};
    gfx_call(c, view, GFX_SETVARIABLE, 3, a);
}
void set_num(CPU *c, uint32_t view, uint32_t name, double x) {
    gv_num(g_gv, x);
    uint32_t a[3] = {name, g_gv, 0};
    gfx_call(c, view, GFX_SETVARIABLE, 3, a);
}
// -1 when the variable is missing or not a boolean
int get_bool(CPU *c, uint32_t view, uint32_t name) {
    uint32_t v = g_gv + 16;
    M32(v) = 0; M32(v + 4) = 0;
    uint32_t a[2] = {v, name};
    if (!(gfx_call(c, view, GFX_GETVARIABLE, 2, a) & 0xFF)) return -1;
    return (M32(v + 4) & 0x8F) == 2 ? (M8(v + 8) != 0) : -1;
}
void invoke_num(CPU *c, uint32_t view, uint32_t name, double x) {
    uint32_t arg = g_gv + 32;
    gv_num(arg, x);
    uint32_t a[4] = {name, 0, arg, 1};
    gfx_call(c, view, GFX_INVOKE, 4, a);
}

// ---- a pass into one of our targets
void pass_render(CPU *c, uint32_t menu, Target &t) {
    IDirect3DDevice9 *d = g_dev;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 vp;
    RECT sc;
    d->GetViewport(&vp);
    d->GetScissorRect(&sc);
    g_rt = t.surf;
    g_from_rt = rt;
    g_from_ds = ds;
    g_from_bb = nullptr;
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_from_bb);
    bind_rt(d);
    d->SetDepthStencilSurface(g_ds);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
    // the menu continues with the viewport and scissor the game had set
    g_guest_vp = vp;
    D3DVIEWPORT9 svp = vp;
    scale_vp(&svp);
    d->SetViewport(&svp);
    g_guest_sc = sc;
    RECT ssc = sc;
    scale_rect(&ssc);
    d->SetScissorRect(&ssc);
    g_pass_thread = pthread_self();
    g_pass = true;
    c->r[1] = menu;
    rt_invoke(c, IMENU_RENDER, 0, nullptr);
    g_pass = false;
    g_on_rt = false;
    d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(ds);
    d->SetViewport(&vp);
    d->SetScissorRect(&sc);
    if (g_d3d_fp) d3d_fp_target(d);  // the game's targets are back, set past the bridge
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (g_from_bb) g_from_bb->Release();
    g_from_rt = g_from_ds = g_from_bb = nullptr;
    g_rt = nullptr;
    t.valid = true;
    g_dirty = true;
    if (g_d3d_passes) d3d_passes_target(d);  // the game's targets are back, set past the bridge
}

void render_hud(CPU *c, uint32_t menu) {
    uint32_t view = M32(menu + 8);
    if (!view) { rt_invoke(c, IMENU_RENDER, 0, nullptr); return; }
    gfx_strings();
    // player bars: stacked, and held fully shown (they live on the second screen only)
    for (int i = 0; i < 3; i++) {
        invoke_num(c, view, g_str_goto[i], METER_SHOWN_FRAME);
        set_num(c, view, g_str_x[i], k_bars[i].x);
        set_num(c, view, g_str_y[i], k_bars[i].y);
        set_num(c, view, g_str_alpha[i], 100);
    }
    int vis[N_MAIN + N_SECOND];
    for (int i = 0; i < N_MAIN + N_SECOND; i++) vis[i] = get_bool(c, view, g_str_vis[i]);
    // main screen: only the parts that belong over the game
    for (int i = N_MAIN; i < N_MAIN + N_SECOND; i++) if (vis[i] > 0) set_bool(c, view, g_str_vis[i], false);
    c->r[1] = menu;
    rt_invoke(c, IMENU_RENDER, 0, nullptr);
    // second screen: the rest; the player bars always
    for (int i = 0; i < N_MAIN; i++) if (vis[i] > 0) set_bool(c, view, g_str_vis[i], false);
    for (int i = N_MAIN; i < N_MAIN + N_SECOND; i++) {
        const char *n = k_second_parts[i - N_MAIN];
        bool bar = !strcmp(n, "Health") || !strcmp(n, "Magica") || !strcmp(n, "Stamina");
        if (vis[i] > 0 || (bar && vis[i] == 0)) set_bool(c, view, g_str_vis[i], true);
    }
    pass_render(c, menu, g_hud);
    // put the game's own visibility back
    for (int i = 0; i < N_MAIN + N_SECOND; i++)
        if (vis[i] >= 0) set_bool(c, view, g_str_vis[i], vis[i] != 0);
    g_hud_moved = true;
}

// Menu render hook (thiscall, this = the menu).
void hud_render(CPU *c) {
    uint32_t menu = c->r[1];
    IDirect3DDevice9 *d = g_dev;
    IDirect3DSurface9 *rt = nullptr;
    bool ok = d && g_chain && SUCCEEDED(d->GetRenderTarget(0, &rt)) && ensure_targets(d, rt);
    if (rt) rt->Release();
    if (!ok) {
        c->r[1] = menu;
        rt_invoke(c, IMENU_RENDER, 0, nullptr);
        RET(0, 0);
    }
    if (M32(menu) == VT_HUD) render_hud(c, menu);
    else pass_render(c, menu, g_fav);
    RET(0, 0);
}

// Second screen layout: the HUD image and the favourites list (the left part
// of the favourites menu) side by side or one above the other, whichever
// gives the HUD more room.
const float FAV_CROP = 0.42f;  // share of the menu's width that holds the list
void layout(RECT *hud, RECT *fav, RECT *fav_src) {
    float W = (float)g_win_w, H = (float)g_win_h, a = (float)g_rt_w / g_rt_h, fa = a * FAV_CROP;
    // stacked: HUD on top at full width, list below; side by side: list left
    float hs_w = W, hs_h = W / a;
    if (hs_h > H * 0.75f) { hs_h = H * 0.75f; hs_w = hs_h * a; }
    float rem_h = H - hs_h;
    float s_w = W / (a + fa), s_h = s_w;  // side by side: common height h, widths a*h and fa*h
    if (s_h > H) s_h = H;
    float side_hud_w = s_h * a;
    if (side_hud_w > hs_w) {
        float x0 = (W - s_h * (a + fa)) / 2, y0 = (H - s_h) / 2;
        *fav = {(LONG)x0, (LONG)y0, (LONG)(x0 + s_h * fa), (LONG)(y0 + s_h)};
        *hud = {fav->right, (LONG)y0, (LONG)(fav->right + side_hud_w), (LONG)(y0 + s_h)};
    } else {
        *hud = {(LONG)((W - hs_w) / 2), 0, (LONG)((W + hs_w) / 2), (LONG)hs_h};
        float fh = rem_h, fw = fh * fa;
        if (fw > W) { fw = W; fh = fw / fa; }
        *fav = {(LONG)((W - fw) / 2), (LONG)hs_h, (LONG)((W + fw) / 2), (LONG)(hs_h + fh)};
    }
    *fav_src = {0, 0, (LONG)(g_rt_w * FAV_CROP), (LONG)g_rt_h};
}
} // namespace

bool hud_screen_in_pass(void) { return in_pass(); }

// ---------------------------------------------------------------- bridge
// Hand-written device methods (see tools/gen_d3d9_bridge.py): plain
// pass-through except during a menu pass on our target.
// Skyrim renders the scene's depth a second time into a back-buffer-sized R32F colour target (638 of a
// Riverwood frame's 2,013 draws), which unlike the D24S8 depth buffer can be read back and sampled. The last
// one bound is kept here, not referenced: use it within the frame it was bound in.
static IDirect3DSurface9 *g_scene_depth;
bool hud_screen_in_pass(void) { return in_pass(); }
IDirect3DSurface9 *d3d_scene_depth_target(void) { return g_scene_depth; }
// Called after each Present: a frame that never binds the depth target (menus, loading) reports none.
void d3d_scene_depth_reset(void) { g_scene_depth = nullptr; }
// LB_D3DTRACE: these calls define the frame's passes, so they say what the surface is, not only its handle.
extern "C" int platform_frame_count(void);
static void trace_surface(const char *call, uint32_t index, IDirect3DSurface9 *s) {
    D3DSURFACE_DESC desc = {};
    if (s) s->GetDesc(&desc);
    char fourcc[5] = {(char)desc.Format, (char)(desc.Format >> 8), (char)(desc.Format >> 16), (char)(desc.Format >> 24), 0};
    fprintf(stderr, "D3D %d IDirect3DDevice9::%s %u %p %ux%u format %u%s%s usage %x\n", platform_frame_count(), call, index,
            (void *)s, desc.Width, desc.Height, (unsigned)desc.Format, desc.Format > 0xFFFF ? " " : "",
            desc.Format > 0xFFFF ? fourcc : "", (unsigned)desc.Usage);
}
void B_IDirect3DDevice9_SetRenderTarget(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *s = (IDirect3DSurface9 *)com_host(ARG(2));
    if (g_d3d_trace) trace_surface("SetRenderTarget", ARG(1), s);
    if (s && ARG(1) == 0) {  // remember the scene's depth target (see d3d_scene_depth_target)
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(s->GetDesc(&desc)) && desc.Format == D3DFMT_R32F && desc.Width >= 640) g_scene_depth = s;
    }
    if (in_pass() && ARG(1) == 0) {
        if (s && (s == g_from_rt || s == g_from_bb || s == g_rt)) { bind_rt(d); RET(0, 3); }
        g_on_rt = false;  // the menu draws into a target of its own (filters)
    }
    HRESULT hr = d->SetRenderTarget(ARG(1), s);
    if (g_d3d_fp && ARG(1) == 0) d3d_fp_target(d);
    if (g_d3d_passes && ARG(1) == 0) d3d_passes_target(d);
    RET((uint32_t)hr, 3);
}
void B_IDirect3DDevice9_SetDepthStencilSurface(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *s = (IDirect3DSurface9 *)com_host(ARG(1));
    if (g_d3d_trace) trace_surface("SetDepthStencilSurface", 0, s);
    if (in_pass() && s && s == g_from_ds) s = g_ds;
    HRESULT hr = d->SetDepthStencilSurface(s);
    if (g_d3d_fp) d3d_fp_target(d);
    if (g_d3d_passes) d3d_passes_target(d);
    RET((uint32_t)hr, 2);
}
void B_IDirect3DDevice9_Clear(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    uint32_t n = ARG(1);
    const D3DRECT *rects = (const D3DRECT *)GPTR(ARG(2));
    if (g_d3d_trace) {
        fprintf(stderr, "D3D %d IDirect3DDevice9::Clear flags %x z %g\n", platform_frame_count(), ARG(3), ARGF(5));
        d3d_trace_camera("Clear");
    }
    if (g_d3d_passes) d3d_passes_clear(ARG(3));
    if (g_d3d_fp) d3d_fp_clear(ARG(3));
    if (in_pass() && g_on_rt && n && rects) {
        D3DRECT r[16];
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++)
            r[i] = {rects[i].x1 * (LONG)g_k, rects[i].y1 * (LONG)g_k, rects[i].x2 * (LONG)g_k, rects[i].y2 * (LONG)g_k};
        RET((uint32_t)d->Clear(n, r, ARG(3), ARG(4), ARGF(5), ARG(6)), 7);
    }
    RET((uint32_t)d->Clear(n, rects, ARG(3), ARG(4), ARGF(5), ARG(6)), 7);
}
void B_IDirect3DDevice9_SetViewport(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *v = (const D3DVIEWPORT9 *)GPTR(ARG(1));
    if (g_d3d_trace && v)
        fprintf(stderr, "D3D %d IDirect3DDevice9::SetViewport %u %u %u %u z %g %g\n", platform_frame_count(),
                (unsigned)v->X, (unsigned)v->Y, (unsigned)v->Width, (unsigned)v->Height, v->MinZ, v->MaxZ);
    if (in_pass() && g_on_rt && v) {
        g_guest_vp = *v;
        D3DVIEWPORT9 s = *v;
        scale_vp(&s);
        RET((uint32_t)d->SetViewport(&s), 2);
    }
    RET((uint32_t)d->SetViewport(v), 2);
}
void B_IDirect3DDevice9_GetViewport(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *v = (D3DVIEWPORT9 *)GPTR(ARG(1));
    if (in_pass() && g_on_rt && v) { *v = g_guest_vp; RET(0, 2); }
    RET((uint32_t)d->GetViewport(v), 2);
}
void B_IDirect3DDevice9_SetScissorRect(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *r = (const RECT *)GPTR(ARG(1));
    if (in_pass() && g_on_rt && r) {
        g_guest_sc = *r;
        RECT s = *r;
        scale_rect(&s);
        RET((uint32_t)d->SetScissorRect(&s), 2);
    }
    RET((uint32_t)d->SetScissorRect(r), 2);
}
void B_IDirect3DDevice9_GetScissorRect(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *r = (RECT *)GPTR(ARG(1));
    if (in_pass() && g_on_rt && r) { *r = g_guest_sc; RET(0, 2); }
    RET((uint32_t)d->GetScissorRect(r), 2);
}

// ---------------------------------------------------------------- hooks
extern "C" void hud_screen_setup_guest(void) {
#ifdef GAME_SKYRIM
    uint32_t t = rt_thunk(hud_render, "hud_render");
    for (uint32_t vt : {VT_HUD, VT_FAVORITES}) {
        if (M32(vt + 6 * 4) != IMENU_RENDER) { LOG("hud screen: unexpected vtable at %08x", vt); continue; }
        M32(vt + 6 * 4) = t;
    }
#endif
}

void hud_screen_device(IDirect3DDevice9 *d) { g_dev = d; }

// Before Reset: everything in D3DPOOL_DEFAULT must go.
void hud_screen_reset(void) {
    release_targets();
    if (g_chain) {
        release_chain();
        pthread_mutex_lock(&g_lock);
        g_seen_gen = -1;  // make a new one at the next Present
        pthread_mutex_unlock(&g_lock);
    }
}

// The crosshair stays on the main screen now (drawn by the HUD movie).
extern "C" int hud_screen_crosshair(void) { return 0; }

// Called just before the main Present.
void hud_screen_present(IDirect3DDevice9 *d) {
    g_dev = d;
    sync_window(d);
    if (g_chain && g_dirty && g_hud.surf) {
        IDirect3DSurface9 *bb = nullptr;
        if (SUCCEEDED(g_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            d->ColorFill(bb, nullptr, 0xFF000000);
            RECT hud, fav, fav_src;
            layout(&hud, &fav, &fav_src);
            if (g_hud.valid) d->StretchRect(g_hud.surf, nullptr, bb, &hud, D3DTEXF_LINEAR);
            if (g_fav.valid && fav.right > fav.left && fav.bottom > fav.top)
                d->StretchRect(g_fav.surf, &fav_src, bb, &fav, D3DTEXF_LINEAR);
            bb->Release();
            g_chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
        }
        g_dirty = false;
    }
    g_hud_moved = false;
}

// ---------------------------------------------------------------- Java
#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_com_tj_skyrim_SkyrimActivity_nativeHudSurface(JNIEnv *env, jclass,
                                                                                 jobject surface) {
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    pthread_mutex_lock(&g_lock);
    void *old = g_window;
    g_window = w;
    g_win_w = w ? (uint32_t)ANativeWindow_getWidth(w) : 0;
    g_win_h = w ? (uint32_t)ANativeWindow_getHeight(w) : 0;
    int gen = ++g_window_gen;
    // The old surface goes away when this returns: wait (briefly) until the
    // render thread has dropped its swap chain. Before the game runs there is
    // no swap chain to drop.
    bool dropped = true;
    if (old && g_dev) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 1;
        while (g_seen_gen != gen)
            if (pthread_cond_timedwait(&g_cond, &g_lock, &dl)) break;
        dropped = g_seen_gen == gen;
    }
    pthread_mutex_unlock(&g_lock);
    // a swap chain may still use it (game paused): leak it rather than crash
    if (old && dropped) ANativeWindow_release((ANativeWindow *)old);
    LOG("hud screen: surface %s (%ux%u)", w ? "ready" : "gone", g_win_w, g_win_h);
}
#endif
