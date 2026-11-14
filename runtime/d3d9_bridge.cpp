// Guest<->host Direct3D 9 bridge: COM object registry and the methods
// that need hand-written marshalling (structures with pointers, locks,
// presentation). Everything else is generated into d3d9_bridge_gen.cpp.
#include <unistd.h>
#include <dirent.h>
#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <ctype.h>
#include <functional>
#include <vector>
void shadow_flush(void *obj);
void shadow_forget(void *obj);
#include "d3d9_bridge.h"
#include "motion_path.h"
void bc_forget(void *obj);
bool bc_lock(void *tex, uint32_t sub, uint32_t w, uint32_t h, const RECT *r, DWORD flags, uint32_t out,
             const std::function<HRESULT(D3DLOCKED_RECT *, const RECT *)> &lock, HRESULT *hr);
bool bc_unlock(void *tex, uint32_t sub);
bool bc_surface_target(void *surf, void **tex, uint32_t *sub);
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <map>
#include <algorithm>
#include <string>

// ---------------------------------------------------------------- registry
namespace {
struct IfaceInfo { const char *name = nullptr; uint32_t vtbl = 0; };
IfaceInfo g_ifaces[64];
std::recursive_mutex g_lock;
std::unordered_map<void *, uint32_t> g_host_to_guest;
// Guest COM id -> host object. com_host reads it on every D3D9 call (about 5% of the main thread went to its
// lock), so reads take none: the table grows in chunks that never move and its count only rises. Writers hold
// g_lock as before.
struct ObjTable {
    static constexpr uint32_t kChunk = 4096, kChunks = 4096;
    std::atomic<std::atomic<void *> *> chunks[kChunks] = {};
    std::atomic<uint32_t> count{0};
    ObjTable() { push_back(nullptr); }  // id 0 is never an object
    uint32_t size() const { return count.load(std::memory_order_acquire); }
    std::atomic<void *> &operator[](uint32_t id) {
        return chunks[id / kChunk].load(std::memory_order_acquire)[id % kChunk];
    }
    void push_back(void *host) {
        uint32_t id = count.load(std::memory_order_relaxed);
        if (id / kChunk >= kChunks) rt_fatal("out of guest COM object ids");
        if (!chunks[id / kChunk].load(std::memory_order_relaxed))
            chunks[id / kChunk].store(new std::atomic<void *>[kChunk](), std::memory_order_release);
        (*this)[id].store(host, std::memory_order_release);
        count.store(id + 1, std::memory_order_release);  // the entry is visible before the count covers it
    }
} g_objs;
std::vector<uint32_t> g_free_ids;
}

void com_register_iface(int iface, const char *name, const ShimFn *fns, int n) {
    uint32_t vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s#%d", name, i);
        M32(vt + 4 * i) = rt_thunk(fns[i], strdup(buf));
    }
    g_ifaces[iface].name = name;
    g_ifaces[iface].vtbl = vt;
}

void *com_host(uint32_t g) {
    if (!g) return nullptr;
    uint32_t id = M32(g + 4);
    if (id >= g_objs.size()) rt_fatal("bad guest COM object %08x", g);
    return g_objs[id].load(std::memory_order_acquire);
}

uint32_t com_wrap(void *host, int iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_host_to_guest.find(host);
    // A texture level's surface shares its container's refcount, so its
    // wrapper is never forgotten; when the host address is reused by an
    // object of another type, the stale mapping must not be returned.
    if (it != g_host_to_guest.end()) {
        if ((int)M32(it->second + 8) == iface) return it->second;
        g_objs[M32(it->second + 4)] = nullptr;
        g_free_ids.push_back(M32(it->second + 4));
        M32(it->second) = 0xDEAD0000u;
        g_host_to_guest.erase(it);
    }
    uint32_t id;
    if (!g_free_ids.empty()) { id = g_free_ids.back(); g_free_ids.pop_back(); g_objs[id] = host; }
    else { id = (uint32_t)g_objs.size(); g_objs.push_back(host); }
    uint32_t g = gmalloc(16);
    M32(g) = g_ifaces[iface].vtbl;
    M32(g + 4) = id;
    M32(g + 8) = (uint32_t)iface;
    M32(g + 12) = 0;
    g_host_to_guest[host] = g;
    return g;
}

static void com_forget(uint32_t g) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    uint32_t id = M32(g + 4);
    void *h = g_objs[id];
    shadow_forget(h);
    bc_forget(h);
    g_host_to_guest.erase(h);
    g_objs[id] = nullptr;
    g_free_ids.push_back(id);
    M32(g) = 0xDEAD0000u;
    gfree(g);
}

void com_AddRef(CPU *c) {
    auto *u = (IUnknown *)com_host(ARG(0));
    RET(u->AddRef(), 1);
}

void com_Release(CPU *c) {
    uint32_t g = ARG(0);
    auto *u = (IUnknown *)com_host(g);
    ULONG r = u->Release();
    if (r == 0) com_forget(g);
    RET(r, 1);
}

void com_QueryInterface(CPU *c) {
    auto *u = (IUnknown *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = u->QueryInterface(iid, &out);
    if (ARG(2)) M32(ARG(2)) = 0;
    if (SUCCEEDED(hr) && out) {
        int iface = d3d9_iface_from_iid(iid);
        if (iface < 0) iface = (int)M32(ARG(0) + 8); // IUnknown: same object
        M32(ARG(2)) = d3d9_wrap(out, iface);
    }
    RET(hr, 3);
}

int d3d9_iface_from_iid(const GUID &iid) {
#define I(T) if (iid == __uuidof(T)) return IFACE_##T;
    I(IDirect3D9) I(IDirect3DDevice9) I(IDirect3DSwapChain9) I(IDirect3DTexture9) I(IDirect3DCubeTexture9)
    I(IDirect3DVolumeTexture9) I(IDirect3DVolume9) I(IDirect3DSurface9) I(IDirect3DVertexBuffer9)
    I(IDirect3DIndexBuffer9) I(IDirect3DVertexDeclaration9) I(IDirect3DVertexShader9) I(IDirect3DPixelShader9)
    I(IDirect3DStateBlock9) I(IDirect3DQuery9)
#undef I
    if (iid == __uuidof(IDirect3DBaseTexture9)) return IFACE_IDirect3DBaseTexture9;
    return -1;
}

uint32_t d3d9_wrap(void *host, int iface) {
    if (!host) return 0;
    if (iface == IFACE_IDirect3DBaseTexture9) {
        switch (((IDirect3DBaseTexture9 *)host)->GetType()) {
        case D3DRTYPE_CUBETEXTURE: iface = IFACE_IDirect3DCubeTexture9; break;
        case D3DRTYPE_VOLUMETEXTURE: iface = IFACE_IDirect3DVolumeTexture9; break;
        default: iface = IFACE_IDirect3DTexture9; break;
        }
    }
    return com_wrap(host, iface);
}

// ---------------------------------------------------------------- structs
extern "C" int touch_available(void);

static void pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *pp) {
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = M32(g + 0);
    pp->BackBufferHeight = M32(g + 4);
    pp->BackBufferFormat = (D3DFORMAT)M32(g + 8);
    pp->BackBufferCount = M32(g + 12);
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)M32(g + 16);
    pp->MultiSampleQuality = M32(g + 20);
    pp->SwapEffect = (D3DSWAPEFFECT)M32(g + 24);
    pp->hDeviceWindow = (HWND)platform_window();
    pp->Windowed = TRUE; // the platform layer owns the (fullscreen) window
    pp->EnableAutoDepthStencil = M32(g + 36);
    pp->AutoDepthStencilFormat = (D3DFORMAT)M32(g + 40);
    pp->Flags = M32(g + 44);
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = M32(g + 52);
    // The VSync menu item controls the touch overlay instead (touch.cpp).
    // Phones always present with vsync; desktop test runs never wait for it.
    if (touch_available())
#ifdef __ANDROID__
        pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
#else
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
#endif
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

static void pp_to_guest(const D3DPRESENT_PARAMETERS *pp, uint32_t g) {
    M32(g + 0) = pp->BackBufferWidth;
    M32(g + 4) = pp->BackBufferHeight;
    M32(g + 8) = pp->BackBufferFormat;
    M32(g + 12) = pp->BackBufferCount;
}

// ---------------------------------------------------------------- IDirect3D9
void B_IDirect3D9_GetAdapterMonitor(CPU *c) { RET(0x00010001u, 2); }

// The game picks its shaders and default quality by GPU vendor. On a vendor
// it does not know (Qualcomm on phones) it compiles shaders at runtime with
// D3DCompiler; as NVIDIA/Intel/AMD it creates ~1700 prebuilt shaders up front,
// which takes minutes on a phone. So the real adapter is reported by default
// (0.1.5 behaviour). save/gpu_vendor.txt or LB_GPU_VENDOR: keep (default),
// intel, nvidia, amd.
static const char *gpu_vendor_choice() {
    static char v[16];
    if (v[0]) return v;
    snprintf(v, sizeof v, "keep");
    const char *e = getenv("LB_GPU_VENDOR");
    if (e && *e) snprintf(v, sizeof v, "%s", e);
    else {
        char path[1100];
        snprintf(path, sizeof path, "%s/gpu_vendor.txt", g_save_dir);
        if (FILE *f = fopen(path, "r")) {
            char w[16] = "";
            if (fscanf(f, "%15s", w) == 1) snprintf(v, sizeof v, "%s", w);
            fclose(f);
        }
    }
    for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
    return v;
}

void B_IDirect3D9_GetAdapterIdentifier(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    if (g_d3d_trace) d3d_trace(c, "IDirect3D9::GetAdapterIdentifier", 3);
    auto *id = (D3DADAPTER_IDENTIFIER9 *)GPTR(ARG(3));
    HRESULT hr = d3d->GetAdapterIdentifier(ARG(1), ARG(2), id);
    const char *v = gpu_vendor_choice();
    if (SUCCEEDED(hr) && id && strcmp(v, "keep")) {
        uint32_t ven = 0x8086, dev = 0x5912;                     // Intel HD 630
        if (!strcmp(v, "nvidia")) ven = 0x10DE, dev = 0x1C82;   // GTX 1050 Ti
        else if (!strcmp(v, "amd")) ven = 0x1002, dev = 0x67DF; // RX 480
        LOG("GPU vendor %04x device %04x reported as %s %04x:%04x", (unsigned)id->VendorId,
            (unsigned)id->DeviceId, v, ven, dev);
        id->VendorId = ven;
        id->DeviceId = dev;
    }
    RET((uint32_t)hr, 4);
}

// Display modes: a fixed list of heights at the aspect ratio chosen in the
// launcher (<save>/display.cfg, "an ad"; "0 0" = the screen's own). The
// launcher offers the same list as "Resolution" and writes the chosen one to
// SkyrimPrefs.ini (the game only accepts a size that is in this list). The
// chosen mode becomes the back buffer, which dxvk scales up to the window,
// letterboxed when its aspect differs from the screen's (see view_rect).
static const uint32_t k_mode_heights[] = {1080, 900, 720, 648, 576, 540, 480, 432, 360, 288, 270, 216, 180, 144, 108};
static int g_aspect_n = -1, g_aspect_d;
static void mode_aspect(int ww, int wh, int64_t *an, int64_t *ad) {
    if (g_aspect_n < 0) {
        g_aspect_n = g_aspect_d = 0;
        char path[1024];
        snprintf(path, sizeof path, "%s/display.cfg", g_save_dir);
        if (FILE *f = fopen(path, "r")) {
            if (fscanf(f, "%d %d", &g_aspect_n, &g_aspect_d) != 2 || g_aspect_n <= 0 || g_aspect_d <= 0)
                g_aspect_n = g_aspect_d = 0;
            fclose(f);
        }
        LOG("display aspect %d:%d", g_aspect_n, g_aspect_d);
    }
    *an = g_aspect_n ? g_aspect_n : ww;
    *ad = g_aspect_n ? g_aspect_d : wh;
}
static uint32_t mode_count(int *ww, int *wh, int64_t *an, int64_t *ad, uint32_t *top) {
    *ww = 1280; *wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), ww, wh);
    mode_aspect(*ww, *wh, an, ad);
    int64_t t = (int64_t)*ww * *ad / *an;
    *top = (uint32_t)(t < *wh ? t : *wh);
    /* An explicit LB_WINDOW is the largest mode, at its own aspect, whatever the window is: on visionOS the window
     * is the system's, so LB_WINDOW=1280x1024 was not in the list and the game refused to start ("Your display
     * doesn't support the selected resolution", the simulator, 17 Nov). The smaller modes follow at that aspect. */
    static int lw = -1, lh = -1;
    if (lw < 0) {
        lw = lh = 0;
        if (const char *e = getenv("LB_WINDOW")) {
            if (sscanf(e, "%dx%d", &lw, &lh) != 2 || lw < 16 || lh < 16) lw = lh = 0;
        }
    }
    if (lw) { *an = lw; *ad = lh; *top = (uint32_t)lh; }
    uint32_t n = 1;  // the largest size that fits the window
    for (uint32_t h : k_mode_heights) if (h < *top) n++;
    return n;
}
static uint32_t mode_count() { int ww, wh; int64_t an, ad; uint32_t top; return mode_count(&ww, &wh, &an, &ad, &top); }
extern "C" void bridge_scaled_mode(uint32_t i, uint32_t *w, uint32_t *h) {
    int ww, wh;
    int64_t an, ad;
    uint32_t top, k = 0;
    mode_count(&ww, &wh, &an, &ad, &top);
    *h = top;
    *w = (uint32_t)(top * an / ad) & ~1u;
    if (i == 0) return;
    for (uint32_t mh : k_mode_heights) {
        if (mh >= top) continue;
        if (++k == i) { *h = mh; *w = (uint32_t)(mh * an / ad) & ~1u; return; }
    }
}

// The back buffer size (from CreateDevice/Reset) and the part of the window
// it is shown in: as large as fits at its own aspect, centred.
static uint32_t g_bb_w, g_bb_h;
static RECT view_rect() {
    int ww = 1280, wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), &ww, &wh);
    RECT r = {0, 0, ww, wh};
    if (!g_bb_w || !g_bb_h) return r;
    int64_t w = (int64_t)wh * g_bb_w / g_bb_h, h = wh;
    if (w > ww) { w = ww; h = (int64_t)ww * g_bb_h / g_bb_w; }
    // within a pixel or two of the window: just stretch
    if (ww - w <= 2 && wh - h <= 2) return r;
    r.left = (LONG)((ww - w) / 2);
    r.top = (LONG)((wh - h) / 2);
    r.right = r.left + (LONG)w;
    r.bottom = r.top + (LONG)h;
    return r;
}
// The same rectangle in 0..1 window coordinates, for the touch overlay.
extern "C" void bridge_view_rect(float *x0, float *y0, float *x1, float *y1) {
    int ww = 1280, wh = 720;
    SDL_GetWindowSize((SDL_Window *)platform_window(), &ww, &wh);
    RECT r = view_rect();
    *x0 = (float)r.left / ww; *x1 = (float)r.right / ww;
    *y0 = (float)r.top / wh; *y1 = (float)r.bottom / wh;
}

void B_IDirect3D9_GetAdapterModeCount(CPU *c) { RET(mode_count(), 3); }
void B_IDirect3D9_EnumAdapterModes(CPU *c) {
    /* (this, adapter, format, mode, D3DDISPLAYMODE *) */
    uint32_t i = ARG(3), p = ARG(4);
    if (i >= mode_count()) RET(D3DERR_INVALIDCALL, 5);
    uint32_t w, h;
    bridge_scaled_mode(i, &w, &h);
    M32(p) = w;
    M32(p + 4) = h;
    M32(p + 8) = 60;
    M32(p + 12) = ARG(2) ? ARG(2) : D3DFMT_X8R8G8B8;
    RET(0, 5);
}

// The game sizes its first-run default config from the desktop mode, so
// never let this fail: fall back to the platform window's size.
void B_IDirect3D9_GetAdapterDisplayMode(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    D3DDISPLAYMODE m = {};
    HRESULT hr = d3d->GetAdapterDisplayMode(ARG(1), &m);
    if (FAILED(hr) || !m.Width || !m.Height) {
        int w = 1280, h = 720;
        SDL_GetWindowSize((SDL_Window *)platform_window(), &w, &h);
        m.Width = (UINT)w;
        m.Height = (UINT)h;
        m.RefreshRate = 60;
        m.Format = D3DFMT_X8R8G8B8;
        LOG("GetAdapterDisplayMode failed (%08x); using %ux%u", (unsigned)hr, m.Width, m.Height);
    }
    if (!m.Format) m.Format = D3DFMT_X8R8G8B8;
    uint32_t p = ARG(2);
    M32(p) = m.Width;
    M32(p + 4) = m.Height;
    M32(p + 8) = m.RefreshRate;
    M32(p + 12) = m.Format;
    RET(0, 3);
}

static void fp_init(IDirect3DDevice9 *d);  // first-person depth, below
static void passes_init(IDirect3DDevice9 *d);  // LB_PASSES, below

void B_IDirect3D9_CreateDevice(CPU *c) {
    auto *d3d = (IDirect3D9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(5), &pp);
    LOG("CreateDevice %ux%u fmt=%d ds=%d/%d windowed(guest)=%u flags=%x interval=%x",
        pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.EnableAutoDepthStencil,
        pp.AutoDepthStencilFormat, M32(ARG(5) + 32), ARG(4), pp.PresentationInterval);
    IDirect3DDevice9 *dev = nullptr;
    DWORD flags = ARG(4) | D3DCREATE_MULTITHREADED;
    rt_host_affinity_begin();
    HRESULT hr = d3d->CreateDevice(ARG(1), (D3DDEVTYPE)ARG(2), (HWND)platform_window(), flags, &pp, &dev);
    rt_host_affinity_end();
    pp_to_guest(&pp, ARG(5));
    if (SUCCEEDED(hr)) { g_bb_w = pp.BackBufferWidth; g_bb_h = pp.BackBufferHeight; }
    if (SUCCEEDED(hr)) { extern void hud_screen_device(IDirect3DDevice9 *); hud_screen_device(dev); }
    if (SUCCEEDED(hr)) fp_init(dev);
    if (SUCCEEDED(hr)) passes_init(dev);
    if (ARG(6)) M32(ARG(6)) = SUCCEEDED(hr) ? d3d9_wrap(dev, IFACE_IDirect3DDevice9) : 0;
    LOG("CreateDevice -> %08x", (uint32_t)hr);
    RET(hr, 7);
}

// ---------------------------------------------------------------- device
void B_IDirect3DDevice9_GetCreationParameters(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = d->GetCreationParameters(&p);
    uint32_t g = ARG(1);
    M32(g) = p.AdapterOrdinal;
    M32(g + 4) = p.DeviceType;
    M32(g + 8) = 0x00010010u;
    M32(g + 12) = p.BehaviorFlags;
    RET(hr, 2);
}

void B_IDirect3DDevice9_CreateAdditionalSwapChain(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(1), &pp);
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT hr = d->CreateAdditionalSwapChain(&pp, &sc);
    pp_to_guest(&pp, ARG(1));
    if (ARG(2)) M32(ARG(2)) = d3d9_wrap(sc, IFACE_IDirect3DSwapChain9);
    RET(hr, 3);
}

void B_IDirect3DDevice9_Reset(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_from_guest(ARG(1), &pp);
    { extern void hud_screen_reset(void); hud_screen_reset(); }
    rt_host_affinity_begin();
    HRESULT hr = d->Reset(&pp);
    rt_host_affinity_end();
    pp_to_guest(&pp, ARG(1));
    if (SUCCEEDED(hr)) { g_bb_w = pp.BackBufferWidth; g_bb_h = pp.BackBufferHeight; }
    if (SUCCEEDED(hr) && g_d3d_fp) d3d_fp_target(d);  // Reset binds the back buffer again
    if (SUCCEEDED(hr) && g_d3d_passes) d3d_passes_target(d);  // Reset binds the back buffer again
    LOG("Reset %ux%u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, (uint32_t)hr);
    RET(hr, 2);
}

// LB_D3DTRACE=from:to[,from:to...] logs every generated bridge call made during those
// frames (counted by Present) to stderr.
int g_d3d_trace;
static int g_frame, g_trace_from = -1;

static void trace_camera_changed(void);
void d3d_trace(CPU *c, const char *name, int nargs) {
    trace_camera_changed();
    if (strstr(name, "Draw")) fprintf(stderr, "D3D %d #%d %s", g_frame, g_d3d_draws, name);
    else fprintf(stderr, "D3D %d %s", g_frame, name);
    for (int i = 1; i <= nargs; i++) fprintf(stderr, " %x", ARG(i));
    if (strstr(name, "ConstantF") && ARG(2))
        for (uint32_t i = 0; i < ARG(3) * 4 && i < 64; i++)
            fprintf(stderr, "%s%g", i % 4 ? " " : " | ", *(float *)GPTR(ARG(2) + 4 * i));
    if (strstr(name, "SetTransform") && ARG(2))
        for (uint32_t i = 0; i < 16; i++)
            fprintf(stderr, "%s%g", i % 4 ? " " : " | ", *(float *)GPTR(ARG(2) + 4 * i));
    fprintf(stderr, "\n");
}

// LB_BISECT=F: on frame F+2k, skip the k-th draw call of the frame and save
// a screenshot after presenting it (finds which draw produces an artefact).
int g_bisect, g_d3d_draws;
static int g_last_class = -1;      // the class of the draw being made (d3d_passes_draw), for LB_SKIP_CLASS
/* LB_DEPTH_COPY=1: the game redraws the whole scene into an R32F colour target only to have its depth as a texture
 * (the "depth" pass class, a third of a frame's draws). Its values, P33 (1 - near / z), are exactly the hardware depth
 * the main pass already wrote, so instead: at the depth pass's first draw, copy the main pass's depth-stencil into
 * that target (DXVK, every stereo layer), and skip the pass's draws. The first-person depth draws stay. */
static int g_depth_copy = -1;
static IDirect3DSurface9 *g_main_ds;   // the main pass's depth-stencil this frame (held)
static IDirect3DDevice9 *g_pass_device;
static bool g_depth_copied;
extern "C" long dxvk_copy_depth_to_float(void *device, void *depthStencil, void *floatTarget);
extern "C" void dxvk_stereo_set_view_ui_offsets(const float view0[4], const float view1[4]);
/* Live stereo: the interface (pass class ui: menus, HUD) at LB_STEREO_HUD_DEPTH metres (default 2) instead of at
 * infinity. g_ui_k is each view's clip offset for a point at that depth, set each frame by stereo_after_present. */
static float g_ui_k[2][2];  // [view][x, y]
static bool g_ui_shift_on;
static uint32_t g_skip_class_mask;  // LB_SKIP_CLASS=depth,shadow,...: skip every draw of those classes (timing only)
static int g_bisect_from = -1, g_draw_no;

// LB_SKIP_DRAWS=F:A-B[,F:A-B...]: on frame F (the frame being drawn, as in LB_SHOT and LB_D3DTRACE: the number its
// Present will have) skip draws A to B, counted from 0 at the frame's first draw (the #n of LB_D3DTRACE's lines).
// With LB_SHOT=F the picture shows what those draws contributed. Experiments only.
struct SkipRange { int frame, from, to; };
static std::vector<SkipRange> g_skip_ranges;
static bool g_skip_ranges_read;
static void skip_ranges_read(void) {
    g_skip_ranges_read = true;
    for (const char *e = getenv("LB_SKIP_DRAWS"); e && *e;) {
        SkipRange r = {};
        int n = sscanf(e, "%d:%d-%d", &r.frame, &r.from, &r.to);
        if (n == 2) r.to = r.from;
        if (n >= 2) g_skip_ranges.push_back(r);
        e = strchr(e, ',');
        if (e) e++;
    }
    if (!g_skip_ranges.empty()) LOG("LB_SKIP_DRAWS: %zu ranges", g_skip_ranges.size());
}
static bool skip_ranges_hit(void) {
    const int frame = g_frame + 1, draw = g_d3d_draws - 1;  // g_d3d_draws already counts this draw
    for (const SkipRange &r : g_skip_ranges)
        if (r.frame == frame && draw >= r.from && draw <= r.to) return true;
    return false;
}
static bool fp_skip_now(void);  // LB_FP_SKIP, below

static bool depth_copy_draw(void);
bool d3d_skip_draw(void) {
    if (g_depth_copy == 1 && depth_copy_draw()) return true;
    if (g_skip_class_mask && g_last_class >= 0 && (g_skip_class_mask >> g_last_class & 1)) return true;
    if (!g_skip_ranges.empty() && skip_ranges_hit()) return true;
    if (fp_skip_now()) return true;
    int k = g_frame - g_bisect_from;
    return k >= 0 && !(k & 1) && g_draw_no++ == k / 2;
}

static void bisect_present(void) {
    if (g_bisect_from < 0) {
        const char *e = getenv("LB_BISECT");
        g_bisect_from = e ? atoi(e) : 1 << 30;
    }
    int k = g_frame - 1 - g_bisect_from;
    if (k >= 0 && !(k & 1) && g_bisect) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "import -window root %s/skip_%03d.png",
                 getenv("LB_BISECT_DIR") ? getenv("LB_BISECT_DIR") : ".", k / 2);
#if defined(__APPLE__)
#if !TARGET_OS_IPHONE
        if (system(cmd)) {}
#else
        LOG("LB_BISECT screenshot export is unavailable on iOS; rendering continues");
#endif
#else
        if (system(cmd)) {}
#endif
    }
    g_draw_no = 0;
    if (!g_skip_ranges_read) skip_ranges_read();
    static const bool fp_skip = getenv("LB_FP_SKIP") || getenv("LB_STEREO_HYBRID");  // both skip draws
    g_bisect = (g_frame >= g_bisect_from && g_frame < g_bisect_from + 160) || !g_skip_ranges.empty() || fp_skip
            || g_skip_class_mask || g_depth_copy == 1;
}

// LB_PASSES=1 logs, at each Present, the frame's draw calls and primitives by the kind of pass they belong to, so
// we can tell how much of a frame a second eye would have to draw again (twcclegg/skyrim-apple#23):
//   pass <frame> shadow=<draws>/<prims> refl=.. main=.. depth=.. fp=.. mask=.. fx=.. post=.. ui=.. other=.. rt=<changes>
// <frame> is the one in the same frame's "ft" line (LB_FRAMETIMES), the classes add up to its draw count, and rt
// counts changes of render target 0. LB_PASSES=2 also logs each run of draws of one class into one target, in the
// LB_D3DTRACE frames (without it, the first frame), to check the classes against a real frame:
//   passdbg <frame> <class> <w>x<h> fmt=<D3DFORMAT> ds=<depth-stencil bound> draws=<n> prims=<n>
// and, every frame, a line on the first-person pass (d3d_first_person):
//   fpdbg <frame> runs=<n> mask=<draws> prepass=<draws> colour=<draws> other=<draws> depth=<draws>
//         draws=#<first>-#<last> near=<first-person near plane> [UNEXPECTED]
// Off, it costs one branch per draw call, target change and Clear. g_d3d_passes turns the hooks on: also without
// LB_PASSES when the first-person flag is wanted (LB_FP=1, LB_FP_SKIP, or LB_STEREO_EYE_UNITS set), then with no logs.
int g_d3d_passes;
static int g_passes_log;  // LB_PASSES: 0 no lines, 1 pass lines, 2 also passdbg and fpdbg lines
enum PassClass {
    PASS_SHADOW, PASS_REFL, PASS_MAIN, PASS_DEPTH, PASS_FP, PASS_MASK, PASS_FX, PASS_POST, PASS_UI, PASS_OTHER, PASS_COUNT
};
static const char *const k_pass_names[PASS_COUNT] = {"shadow", "refl", "main", "depth", "fp",
                                                     "mask",   "fx",   "post", "ui",    "other"};
struct PassTotals { int draws; uint32_t prims; };  // per class and frame (a GPU time per class would go here too)
struct PassTarget { void *rt; uint32_t w, h, fmt; bool ds, bb; };  // what draws go to now (rt: identity only)
struct PassFrame {
    PassTotals cls[PASS_COUNT];
    int rt_changes;
    bool seen_main, seen_depth, seen_bb;  // a main / depth / back buffer draw happened this frame
    // First-person detection (d3d_first_person, below)
    bool fp_late;                         // in the late pass: after its Clear of depth only, until its projection ends
    bool fp_late_confirmed;               // ... and its projection was seen to be the first-person one
    bool fp_proj;                         // the projection set last is the first-person one
    bool fp;                              // fp_late || fp_proj, and a target that allows it
    bool fp_last_draw;                    // the frame's previous draw was a first-person one
    int fp_runs, fp_kind_draws[FP_KINDS]; // runs of consecutive first-person draws; draws by kind (index = kind)
    int fp_first, fp_last;                // frame draw numbers (#n of LB_D3DTRACE) of the first and last of them
};
struct PassRun { int cls; int draws; uint32_t prims; PassTarget t; };  // LB_PASSES=2: draws since the last line
static PassTarget g_pass_target;
static PassFrame g_pass_frame;
static PassRun g_pass_run = {-1, 0, 0, {}};
static bool g_passdbg;

/* First-person draws (twcclegg/skyrim-apple#23, stage 1): the arms, the weapon, the spell hand and the hand's effect,
 * drawn with the game's first-person camera, whose projection has its own near plane (5; the world's is 15). Traced in
 * Riverwood (TESV 1.9.32.0.8, low preset, 1280x720) with empty hands, a sword, and a sword plus Flames, a frame draws
 * the first-person meshes in three places, each time after a SetTransform(D3DTS_PROJECTION) with that near plane:
 *   mask     into the 960x540 pass (the "refl" class), first depth only, then writing red only: the hands in what
 *            looks like the screen-space sun-shadow mask (10 draws with a sword: 5 + 5)
 *   prepass  first thing in the main pass on the scene target, colour writes off: the hands' depth, so the world is not
 *            drawn behind them (5 draws). Skipping the late pass leaves flat silhouettes in the clear colour there.
 *   late     after the world's effects and the sun's first occlusion query, still before tone mapping:
 *              Clear(ZBUFFER only, z 1)   on the scene target; its colour (the world) stays
 *              colour  the meshes with lighting, then the hand's effect (Flames: 3 more additive draws)
 *              other   with Flames, one draw into a cleared A8R8G8B8 target of back-buffer size (the fire's)
 *              Clear(ZBUFFER only) on the scene's R32F depth target, which keeps the world's depth (not cleared)
 *              depth   the opaque meshes again (5), writing their depth over the world's
 *            then the scene target with the world's projection again (the sun's second query) and the post chain.
 * The flag is on during the late pass (from its Clear of depth only into a back-buffer-sized target, once the main
 * pass has drawn, until a projection with another near plane or a target that is not back-buffer-sized), and
 * whenever the projection set last has the near plane learnt from the late pass and not the world camera's. So the
 * mask and prepass draws are caught only once a late pass was seen (from the second first-person frame on).
 * Third person has no such Clear and no such projection. The loading screen's model has a Clear of depth only but the
 * world's near plane, and is not counted. The game keeps drawing the first-person pass under menus that show the
 * world (the Tab menu), so the flag is on there too.
 * The flag is set and cleared on the thread making the device calls, between draws, so code on the bridge's draw path
 * (DXVK per-draw state, a stereo shift) can read it for the draw being made. It needs the pass hooks: LB_PASSES,
 * LB_FP=1, LB_FP_SKIP or LB_STEREO_EYE_UNITS; otherwise it is always off. */
static float g_fp_near;                // the first-person near plane, learnt from a late pass (0: not yet)
static void (*g_fp_listener)(int kind);
static void stereo_fp_changed(int kind);  // stereo captures with the eye offset on first-person draws only, below
static bool stereo_skip_fp(void);         // stereo captures: a frame drawn with no first-person draws, below
static void stereo_late_eyes(void);       // eye views, LB_EYE_LATE: the eyes turned toward the head, below

// The kind of the first-person draw being made, from the target bound (FP_NONE outside first-person draws).
static int fp_kind(void) {
    const PassFrame &f = g_pass_frame;
    if (!f.fp) return FP_NONE;
    const PassTarget &t = g_pass_target;
    if (t.w != g_bb_w || t.h != g_bb_h) return FP_MASK;
    if (t.fmt == D3DFMT_R32F) return FP_DEPTH;
    if (t.fmt != D3DFMT_A16B16G16R16F) return FP_OTHER;
    return f.fp_late ? FP_COLOUR : FP_PREPASS;
}
static void fp_update(void) {
    PassFrame &f = g_pass_frame;
    const PassTarget &t = g_pass_target;
    const int before = fp_kind();
    f.fp = (f.fp_late || f.fp_proj) && t.rt && !t.bb && !hud_screen_in_pass();
    const int now = fp_kind();
    if (now != before) {
        stereo_fp_changed(now);
        if (g_fp_listener) g_fp_listener(now);
    }
}
bool d3d_first_person(void) { return g_pass_frame.fp; }
extern "C" int skyrim_first_person_pass(void) { return fp_kind(); }
extern "C" void skyrim_first_person_listen(void (*fn)(int kind)) { g_fp_listener = fn; }

// The class of a draw, from the targets bound and what the frame drew before. Sizes are relative to the back
// buffer (CreateDevice/Reset), never absolute. First match wins. "Seen" means it matches the Riverwood frame
// traced with LB_D3DTRACE (docs/perf/LOG.md, 2026-11-10); everything else is unverified, check it with LB_PASSES=2.
//   ui      a menu drawn into the second screen's image (hud_screen.cpp, Android dual screen only), or a draw into
//           the back buffer after the first since it was bound: the HUD and menus over the final image (seen: 17
//           draws after the final image in Riverwood)
//   post    the first draw into the back buffer since it was bound, if it is a quad (<= 2 primitives): the final
//           image (seen in Riverwood; the game also draws one quad into the back buffer earlier in the frame)
//   fp      a first-person draw (d3d_first_person, above; seen in Riverwood with empty hands, a sword and Flames),
//           whatever the target: the late pass, and its prepass and mask draws otherwise counted as main and refl
//   depth   an R32F colour target of back-buffer size: the scene's depth drawn again (seen, 638 draws)
//   main    an A16B16G16R16F target of back-buffer size with a depth-stencil bound, before the depth pass (size and
//           format seen, 620 draws; the depth-stencil and "before" unverified)
//   fx      the same target after the depth pass (unverified)
//   shadow  a depth-stencil bound, target not of back-buffer aspect: the 512x1024 shadow map, also the two 512x512
//           targets of the traced frame (unverified)
//   refl    a depth-stencil bound, back-buffer aspect but smaller: the reflection (960x540 at 1280x720 seen; that
//           it has a depth-stencil and the shadow mask has none is unverified)
//   post    no depth-stencil and <= 2 primitives: full-screen quads, the luminance chain and filters (unverified)
//   mask    no depth-stencil, back-buffer aspect but smaller: the deferred sun-shadow mask (unverified)
//   fx      any other draw into a back-buffer-sized target after the depth pass (unverified)
//   other   the rest
static int pass_class(uint32_t prims) {
    const PassTarget &t = g_pass_target;
    PassFrame &f = g_pass_frame;
    bool bb_size = g_bb_w && t.w == g_bb_w && t.h == g_bb_h;
    int64_t skew = (int64_t)t.w * g_bb_h - (int64_t)t.h * g_bb_w;  // within a pixel of height: back-buffer aspect
    bool bb_aspect = g_bb_w && t.w && (skew < 0 ? -skew : skew) <= (int64_t)g_bb_w;
    bool smaller = t.w < g_bb_w;
    if (hud_screen_in_pass()) return PASS_UI;
    if (t.bb) {
        bool first = !f.seen_bb;
        f.seen_bb = true;
        return first && prims <= 2 ? PASS_POST : PASS_UI;
    }
    if (bb_size && t.fmt == D3DFMT_R32F) {
        f.seen_depth = true;
        return f.fp ? PASS_FP : PASS_DEPTH;
    }
    if (bb_size && t.fmt == D3DFMT_A16B16G16R16F && t.ds) {
        if (f.seen_depth) return f.fp ? PASS_FP : PASS_FX;
        f.seen_main = true;
        return f.fp ? PASS_FP : PASS_MAIN;
    }
    if (f.fp) return PASS_FP;
    if (t.ds && !bb_aspect) return PASS_SHADOW;
    if (t.ds && smaller) return PASS_REFL;
    if (!t.ds && prims <= 2) return PASS_POST;
    if (!t.ds && bb_aspect && smaller) return PASS_MASK;
    if (bb_size && f.seen_depth) return PASS_FX;
    return PASS_OTHER;
}

static void passdbg_flush(void) {
    const PassRun &r = g_pass_run;
    if (r.cls >= 0 && r.draws)
        LOG("passdbg %d %s %ux%u fmt=%u ds=%d draws=%d prims=%u", platform_frame_count(), k_pass_names[r.cls], r.t.w,
            r.t.h, r.t.fmt, r.t.ds ? 1 : 0, r.draws, r.prims);
    g_pass_run = {-1, 0, 0, {}};
}

// LB_PASSES_GPU=1 (with LB_PASSES) adds GPU milliseconds per class from Direct3D 9 timestamp queries, which DXVK
// turns into vkCmdWriteTimestamp (MoltenVK: Metal counter sample buffers). A timestamp ("mark") is issued at the
// first change of target or draw of a frame, at each change of render target 0 or depth-stencil, at each change of
// class between draws, and at Present; the time from one mark to the next goes to the class of the first draw after
// the first mark (a span with no draw goes to "other"). The queries are read without flushing a few frames later,
// and each frame gets one line once its last timestamp is in:
//   passgpu <frame> shadow=<ms> refl=.. main=.. depth=.. fp=.. mask=.. fx=.. post=.. ui=.. other=.. total=<ms>
//           marks=<n> zero=<n>
// <frame> is the one of the frame's "pass" line (tools/bench/bench.py merges the two), total is the sum of the
// classes (first mark to last), marks the timestamps of the frame (past GPU_MARKS the last span takes the rest of
// the frame), zero counts spans that read exactly 0 (the GPU resolves timestamps at encoder boundaries: a class sharing an encoder with the next
// reads 0 and the next absorbs it). GPU idle inside a frame (waiting for the CPU to submit) is counted in the span
// it falls in; the Present blit after the last mark is not counted. LB_PASSES_GPU=2 also logs every span of one
// frame in every 300 ("passgpudbg <frame> <i> <class> <ms>") to see the granularity.
static int g_passes_gpu;
static IDirect3DDevice9 *g_pass_dev;  // the real device (CreateDevice), not AddRef'd
static double g_gpu_tick_ms;          // ms per timestamp tick
enum { GPU_MARKS = 384, GPU_RING = 8, GPU_UNRESOLVED = -1, GPU_END = -2 };
struct GpuFrame {
    IDirect3DQuery9 *q[GPU_MARKS];
    int8_t cls[GPU_MARKS];  // class of the span that starts at mark i
    int marks, frame;
    bool pending;           // issued, waiting to be read
};
static GpuFrame g_gpu_ring[GPU_RING];
static int g_gpu_cur, g_gpu_drops;  // ring slot being recorded, frames dropped unread

static void gpu_mark(int cls) {
    GpuFrame &g = g_gpu_ring[g_gpu_cur];
    if (g.pending) return;  // the ring is full of unread frames: this one goes unmeasured
    if (g.marks >= GPU_MARKS) return;
    IDirect3DQuery9 *&q = g.q[g.marks];
    if (!q) {
        const HRESULT hr = g_pass_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q);
        if (FAILED(hr) || !q) {  // no timestamps after all: stop, rather than try again at every draw
            LOG("LB_PASSES_GPU: CreateQuery(TIMESTAMP) failed (%08x); GPU time per class off", (uint32_t)hr);
            q = nullptr;
            g_passes_gpu = 0;
            return;
        }
    }
    q->Issue(D3DISSUE_END);
    g.cls[g.marks++] = (int8_t)cls;
}

// A draw of class k: the open span takes it, or a new span starts if it was another class.
static void gpu_draw(int k) {
    GpuFrame &g = g_gpu_ring[g_gpu_cur];
    if (!g.marks) { gpu_mark(k); return; }
    int8_t &last = g.cls[g.marks - 1];
    if (last == GPU_UNRESOLVED) last = (int8_t)k;
    else if (last != k) gpu_mark(k);
}

// The target changed: a new span whose class is that of its first draw (one mark for several changes in a row).
static void gpu_target(void) {
    GpuFrame &g = g_gpu_ring[g_gpu_cur];
    if (!g.marks || g.cls[g.marks - 1] != GPU_UNRESOLVED) gpu_mark(GPU_UNRESOLVED);
}

// Reads every frame whose timestamps are all in, oldest first; a frame still out when the ring comes round to it
// again is dropped.
static void gpu_collect(void) {
    for (int n = 1; n <= GPU_RING; n++) {
        GpuFrame &g = g_gpu_ring[(g_gpu_cur + n) % GPU_RING];  // oldest first
        if (!g.pending) continue;
        UINT64 last = 0;
        if (g.q[g.marks - 1]->GetData(&last, sizeof last, 0) != S_OK) return;  // later frames are not in either
        double ms[PASS_COUNT] = {}, total = 0;
        int zero = 0;
        UINT64 prev = 0;
        static int dbg_count;
        bool dbg = g_passes_gpu == 2 && dbg_count++ % 300 == 0;
        for (int i = 0; i < g.marks; i++) {
            UINT64 t = 0;
            if (g.q[i]->GetData(&t, sizeof t, 0) != S_OK) t = prev;
            if (i) {
                double d = t >= prev ? (double)(t - prev) * g_gpu_tick_ms : 0;
                int c = g.cls[i - 1] >= 0 ? g.cls[i - 1] : PASS_OTHER;
                ms[c] += d;
                total += d;
                zero += t == prev;
                if (dbg) LOG("passgpudbg %d %d %s %.4f", g.frame, i - 1, k_pass_names[c], d);
            }
            prev = t;
        }
        char line[512];
        int len = snprintf(line, sizeof line, "passgpu %d", g.frame);
        for (int i = 0; i < PASS_COUNT && len < (int)sizeof line; i++)
            len += snprintf(line + len, sizeof line - len, " %s=%.3f", k_pass_names[i], ms[i]);
        LOG("%s total=%.3f marks=%d zero=%d", line, total, g.marks, zero);
        g.pending = false;
        g.marks = 0;
    }
}

// At Present, before the real one: the frame's last mark, then the next slot.
static void gpu_present(int frame) {
    GpuFrame &g = g_gpu_ring[g_gpu_cur];
    if (g.marks && !g.pending) {
        if (g.marks >= GPU_MARKS) g.marks--;  // the end mark replaces the last span
        gpu_mark(GPU_END);
        g.frame = frame;
        g.pending = g.marks > 1;
        if (!g.pending) g.marks = 0;
    }
    gpu_collect();
    g_gpu_cur = (g_gpu_cur + 1) % GPU_RING;
    GpuFrame &next = g_gpu_ring[g_gpu_cur];
    if (next.pending) {  // still unread after GPU_RING frames: give up on it
        next.pending = false;
        next.marks = 0;
        if (g_gpu_drops++ < 10 || g_gpu_drops % 100 == 0) LOG("passgpu: frame %d dropped unread (%d so far)", next.frame, g_gpu_drops);
    }
}

static void gpu_init(IDirect3DDevice9 *d) {
    const char *e = getenv("LB_PASSES_GPU");
    g_passes_gpu = e ? atoi(e) : 0;
    if (!g_passes_gpu) return;
    g_pass_dev = d;
    IDirect3DQuery9 *fq = nullptr;
    UINT64 freq = 0;
    if (SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq))) {
        fq->Issue(D3DISSUE_END);
        for (int i = 0; i < 100 && fq->GetData(&freq, sizeof freq, D3DGETDATA_FLUSH) == S_FALSE; i++) usleep(1000);
        fq->Release();
    }
    if (!freq) {
        LOG("LB_PASSES_GPU: no timestamp frequency; GPU time per class off");
        g_passes_gpu = 0;
        return;
    }
    g_gpu_tick_ms = 1000.0 / (double)freq;
    LOG("LB_PASSES_GPU=%d: GPU ms per pass class from timestamp queries (%llu ticks per second)", g_passes_gpu,
        (unsigned long long)freq);
}

void d3d_passes_draw(uint32_t prims) {
    int k = pass_class(prims);
    g_last_class = k;
    if ((g_ui_k[0][0] != 0 || g_ui_k[1][0] != 0 || g_ui_k[0][1] != 0 || g_ui_k[1][1] != 0) && (k == PASS_UI) != g_ui_shift_on) {
        g_ui_shift_on = k == PASS_UI;
        const float on0[4] = {g_ui_shift_on ? g_ui_k[0][0] : 0.0f, g_ui_shift_on ? g_ui_k[0][1] : 0.0f, 0, 0},
                    on1[4] = {g_ui_shift_on ? g_ui_k[1][0] : 0.0f, g_ui_shift_on ? g_ui_k[1][1] : 0.0f, 0, 0};
        dxvk_stereo_set_view_ui_offsets(on0, on1);
    }
    if (k == PASS_MAIN && g_depth_copy == 1 && !g_main_ds && g_pass_device) g_pass_device->GetDepthStencilSurface(&g_main_ds);
    if (k == PASS_MAIN) stereo_late_eyes();
    if (g_passes_gpu) gpu_draw(k);
    PassFrame &f = g_pass_frame;
    f.cls[k].draws++;
    f.cls[k].prims += prims;
    if (f.fp) {
        if (!f.fp_runs) f.fp_first = g_d3d_draws - 1;  // g_d3d_draws already counts this draw
        if (!f.fp_last_draw) f.fp_runs++;
        f.fp_last = g_d3d_draws - 1;
        f.fp_kind_draws[fp_kind()]++;
    }
    f.fp_last_draw = f.fp;
    if (!g_passdbg) return;
    PassRun &r = g_pass_run;
    if (k != r.cls || g_pass_target.rt != r.t.rt || g_pass_target.ds != r.t.ds) {
        passdbg_flush();
        r.cls = k;
        r.t = g_pass_target;
    }
    r.draws++;
    r.prims += prims;
}

void d3d_passes_target(IDirect3DDevice9 *d) {
    g_pass_device = d;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *bb = nullptr;
    D3DSURFACE_DESC desc = {};
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (rt) rt->GetDesc(&desc);
    if ((void *)rt != g_pass_target.rt) g_pass_frame.rt_changes++;
    // Each time the back buffer is bound again its first quad is the final image (post), not the interface: the
    // game also draws a quad into the back buffer earlier in the frame, as a scratch target (Riverwood, frame 2700
    // traced with LB_PASSES=2: a 1-draw run long before the final image and the HUD).
    if ((void *)rt != g_pass_target.rt && rt && rt == bb) g_pass_frame.seen_bb = false;
    if (g_passes_gpu && ((void *)rt != g_pass_target.rt || (ds != nullptr) != g_pass_target.ds)) gpu_target();
    g_pass_target = {rt, desc.Width, desc.Height, (uint32_t)desc.Format, ds != nullptr, rt && rt == bb};
    const PassTarget &t = g_pass_target;
    // the late first-person pass goes on into other back-buffer-sized targets, and ends at any other
    if (g_pass_frame.fp_late && (!rt || t.bb || t.w != g_bb_w || t.h != g_bb_h)) g_pass_frame.fp_late = false;
    fp_update();
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (bb) bb->Release();
}

// A Clear of depth without colour into a back-buffer-sized target, once the main pass has drawn: the late
// first-person pass may start (d3d_first_person). Its projection confirms it (d3d_passes_projection).
void d3d_passes_clear(uint32_t flags) {
    if (!(flags & D3DCLEAR_ZBUFFER) || (flags & D3DCLEAR_TARGET)) return;
    const PassTarget &t = g_pass_target;
    PassFrame &f = g_pass_frame;
    if (!t.rt || t.bb || !g_bb_w || t.w != g_bb_w || t.h != g_bb_h || !f.seen_main || f.fp_late) return;
    f.fp_late = true;
    f.fp_late_confirmed = false;
    fp_update();
}

// The world camera's near plane (NiCamera W, see game_camera_read below), or 0.
static float world_near(void) {
    uint32_t root = M32(0x1B2E8BC), w = root >= 0x10000 ? M32(root + 0xB8) : 0;
    float v = 0;
    if (w >= 0x10000) memcpy(&v, MEM + w + 0xF8, 4);
    return v;
}

// SetTransform(D3DTS_PROJECTION), m the guest's row-major matrix (row vectors): perspective has _34 = 1, and its near
// plane is -_43 / _33. In the late pass the first projection confirms the pass (a near plane other than the world's)
// and teaches the first-person near plane; a later one with another near plane ends it. Outside it, the projection
// is a first-person one when its near plane is the one learnt.
void d3d_passes_projection(const float *m) {
    PassFrame &f = g_pass_frame;
    const float near_plane = m[11] == 1.0f && m[10] != 0.0f ? -m[14] / m[10] : 0.0f;  // 0: not perspective
    const float world = world_near();
    auto same = [](float a, float b) { return a > 0 && b > 0 && fabsf(a - b) <= 1e-3f * b; };
    if (f.fp_late && !f.fp_late_confirmed) {
        if (near_plane <= 0 || same(near_plane, world)) {
            f.fp_late = false;  // not the first-person camera (the loading screen's model has the world's)
        } else {
            f.fp_late_confirmed = true;
            if (!same(near_plane, g_fp_near)) {
                LOG("first-person near plane %g (world %g)", near_plane, world);
                g_fp_near = near_plane;
            }
        }
    } else if (f.fp_late && !same(near_plane, g_fp_near)) {
        f.fp_late = false;
    }
    f.fp_proj = same(near_plane, g_fp_near) && !same(near_plane, world);
    fp_update();
}

// After CreateDevice: reads the switches once and learns the targets the device starts with.
static void flame_init(void);
static void passes_init(IDirect3DDevice9 *d) {
    static bool read;
    if (!read) {
        read = true;
        flame_init();
        const char *e = getenv("LB_PASSES"), *fp = getenv("LB_FP"), *eye = getenv("LB_STEREO_EYE_UNITS");
        g_passes_log = e ? atoi(e) : 0;
        // On by default with single-pass stereo (where the pass would be drawn twice); LB_DEPTH_COPY=0/1 decides.
        const char *dc = getenv("LB_DEPTH_COPY"), *mv = getenv("LB_STEREO_MULTIVIEW");
        g_depth_copy = dc ? (atoi(dc) ? 1 : 0) : (mv && !strcmp(mv, "1") ? 1 : 0);
        if (g_depth_copy) LOG("LB_DEPTH_COPY: the R32F depth pass is a copy of the main pass's depth");
        if (const char *sc = getenv("LB_SKIP_CLASS")) {  // timing experiments: the picture is wrong by design
            for (int c = 0; c < PASS_COUNT; c++)
                if (strstr(sc, k_pass_names[c])) g_skip_class_mask |= 1u << c;
            LOG("LB_SKIP_CLASS=%s: skipping pass classes mask %x", sc, g_skip_class_mask);
        }
        const bool fp_wanted = (fp && atoi(fp)) || getenv("LB_FP_SKIP") || (eye && atof(eye) != 0) || g_fp_flame
                            || g_skip_class_mask || g_depth_copy == 1;
        g_d3d_passes = g_passes_log || fp_wanted;
        // without an LB_D3DTRACE range, LB_PASSES=2 looks at the first frame
        g_passdbg = g_passes_log == 2 && !getenv("LB_D3DTRACE");
        if (g_passes_log) LOG("LB_PASSES=%d: draws per pass class logged at each Present", g_passes_log);
        else if (fp_wanted) LOG("first-person pass detection on (LB_FP, LB_FP_SKIP or LB_STEREO_EYE_UNITS)");
        if (g_d3d_passes) gpu_init(d);
    }
    if (!g_d3d_passes) return;
    d3d_passes_target(d);
    g_pass_frame.rt_changes = 0;
}

// At Present: the frame's line, then a new frame. LB_PASSES=2 follows the LB_D3DTRACE frames (g_d3d_trace is
// already set for the next one).
// LB_DEPTH_COPY at a depth-pass draw: copy once per frame, then skip. False (draw it) if the copy can't be made.
static bool depth_copy_draw(void) {
    if (g_last_class != PASS_DEPTH) return false;
    if (g_depth_copied) return true;
    IDirect3DSurface9 *rt = nullptr;
    if (!g_pass_device || !g_main_ds || FAILED(g_pass_device->GetRenderTarget(0, &rt)) || !rt) return false;
    const long hr = dxvk_copy_depth_to_float(g_pass_device, g_main_ds, rt);
    rt->Release();
    static int logged;
    if (hr != 0 && logged++ < 3) LOG("LB_DEPTH_COPY: copy failed (hr %08lx); the pass is drawn", (unsigned long)hr);
    if (hr != 0) return false;
    g_depth_copied = true;
    return true;
}

static void passes_present(void) {
    g_depth_copied = false;
    if (g_ui_shift_on) {
        static const float zero[4] = {0, 0, 0, 0};
        dxvk_stereo_set_view_ui_offsets(zero, zero);
        g_ui_shift_on = false;
    }
    if (g_main_ds) { g_main_ds->Release(); g_main_ds = nullptr; }
    if (!g_d3d_passes) return;
    g_pass_frame.fp_late = g_pass_frame.fp_proj = false;
    fp_update();
    if (!g_passes_log) { g_pass_frame = PassFrame(); return; }
    passdbg_flush();
    const PassFrame &f = g_pass_frame;
    if (g_passes_log == 2) {
        // Seen in first person: 3 runs (mask, prepass, late), colour >= depth > 0, prepass = depth. Anything with
        // first-person draws but no colour draws, or more than 3 runs, is flagged.
        const int *k = f.fp_kind_draws;
        const bool odd = f.fp_runs && (!k[FP_COLOUR] || k[FP_DEPTH] > k[FP_COLOUR] || f.fp_runs > 3);
        LOG("fpdbg %d runs=%d mask=%d prepass=%d colour=%d other=%d depth=%d draws=#%d-#%d near=%g%s",
            platform_frame_count(), f.fp_runs, k[FP_MASK], k[FP_PREPASS], k[FP_COLOUR], k[FP_OTHER], k[FP_DEPTH],
            f.fp_runs ? f.fp_first : -1, f.fp_runs ? f.fp_last : -1, g_fp_near, odd ? " UNEXPECTED" : "");
    }
    char line[512];
    int n = snprintf(line, sizeof line, "pass %d", platform_frame_count());
    for (int i = 0; i < PASS_COUNT && n < (int)sizeof line; i++)
        n += snprintf(line + n, sizeof line - n, " %s=%d/%u", k_pass_names[i], g_pass_frame.cls[i].draws,
                      g_pass_frame.cls[i].prims);
    LOG("%s rt=%d", line, g_pass_frame.rt_changes);
    if (g_passes_gpu) gpu_present(platform_frame_count());
    g_pass_frame = PassFrame();
    g_passdbg = g_passes_log == 2 && g_d3d_trace;
}

// LB_FP_SKIP=MODE (every frame) or MODE@F[,MODE@F...] (only frames F, numbered as LB_SHOT numbers them): skip
// first-person draws. MODE is all, late (colour, other and depth: the visible pass and its depth), colour (colour and
// other), depth, prepass or mask; several joined by +, e.g. prepass+mask. Experiments: with LB_SHOT the picture
// shows what those draws contribute.
static bool fp_skip_now(void) {
    static int mode = -1;               // every frame: bit per FP_* kind
    static std::map<int, int> frames;   // or per frame
    if (mode < 0) {
        mode = 0;
        auto parse = [](const char *m, const char *end) {
            int bits = 0;
            while (m < end) {
                const char *plus = (const char *)memchr(m, '+', end - m);
                const std::string w(m, plus ? plus : end);
                bits |= w == "all" ? ~0 : w == "late" ? (1 << FP_COLOUR | 1 << FP_OTHER | 1 << FP_DEPTH)
                        : w == "colour" ? (1 << FP_COLOUR | 1 << FP_OTHER) : w == "depth" ? 1 << FP_DEPTH
                        : w == "prepass" ? 1 << FP_PREPASS : w == "mask" ? 1 << FP_MASK : 0;
                m = plus ? plus + 1 : end;
            }
            return bits;
        };
        const char *e = getenv("LB_FP_SKIP");
        for (const char *item = e; item && *item;) {
            const char *comma = strchr(item, ','), *end = comma ? comma : item + strlen(item);
            const char *at = (const char *)memchr(item, '@', end - item);
            if (at) frames[atoi(at + 1)] = parse(item, at);
            else mode = parse(item, end);
            item = comma ? comma + 1 : nullptr;
        }
        if (e) LOG("LB_FP_SKIP: every frame %x, %zu frames listed", mode, frames.size());
    }
    const int k = fp_kind();
    if (k == FP_NONE) return false;
    if (stereo_skip_fp()) return true;
    auto it = frames.find(g_frame + 1);
    return ((it != frames.end() ? it->second : mode) >> k) & 1;
}

// ---------------------------------------------------------------- first-person depth
/* First-person draws (twcclegg/skyrim-apple#23): the arms, the weapon and the spell hand are drawn with the game's
 * first-person camera, whose projection has its own near plane (5; the world's is 15). Traced in Riverwood (TESV
 * 1.9.32.0.8, low preset, 1280x720) with empty hands, a sword, and a sword plus Flames, a frame draws them three
 * times, each after a SetTransform(D3DTS_PROJECTION) with that near plane: into the 960x540 pass (depth, then red
 * only), as a depth-only prepass first in the main pass, and in a late pass after the world's effects, still before
 * tone mapping:
 *   Clear(ZBUFFER only, z 1)  on the scene target (A16B16G16R16F); its colour (the world) stays
 *   the lit meshes, then the hand's effect (Flames: a few additive draws and one into an A8R8G8B8 target)
 *   Clear(ZBUFFER only)       on the scene's R32F depth target, which keeps the world's depth (not cleared)
 *   the opaque meshes again (five draws with a sword), writing their depth over the world's: the FP_DEPTH draws
 * then the world's projection again and the post chain.
 * Only the FP_DEPTH draws matter here. The detector is on during the late pass (from its Clear of depth only into
 * a back-buffer-sized target, once the main pass has drawn, until a projection with another near plane or a target
 * that is not back-buffer-sized) and whenever the projection set last has the near plane learnt from a late pass and
 * not the world camera's. The late pass is confirmed, and the first-person near plane learnt, by the first projection
 * after its Clear: a near plane other than the world's (the loading screen's model has a Clear of depth only but the
 * world's near plane, and is left out). Third person has no such Clear and no such projection. The hooks run on the
 * thread making the device calls, between draws; they are on unless LB_FP_DEPTH_SCALE=0. */
int g_d3d_fp;
struct FpTarget { void *rt; uint32_t w, h, fmt; bool ds, bb; };  // render target 0 now (rt: identity only)
struct FpFrame {
    bool seen_main, seen_depth;  // a draw into the main scene target / the R32F depth target happened this frame
    bool late;                   // in the late pass: after its Clear of depth only, until its projection ends
    bool late_confirmed;         // ... and its projection was seen to be the first-person one
    bool proj;                   // the projection set last is the first-person one
    bool fp;                     // late || proj, and a target that allows it
};
static FpTarget g_fp_target;
static FpFrame g_fp_frame;
static IDirect3DDevice9 *g_fp_device;  // the device the hooks last saw
static float g_fpd_near;                // the first-person near plane, learnt from a late pass (0: not yet)
static float g_proj_p33;               // _33 of the projection set last (0: not perspective)
static float g_world_p33;              // _33 of the last projection with the world camera's near plane (0: none yet)
static bool g_fp_depth_scale;          // LB_FP_DEPTH_SCALE: on unless 0
static std::vector<int> g_fp_depth_scale_off;  // LB_FP_DEPTH_SCALE=off@F,...: frames drawn without it
static void fp_depth_scale_draw(void);
static void fp_depth_scale_restore(void);

// The draw being made is a first-person one into the scene's R32F depth target (FP_DEPTH).
static bool fp_depth_draw_now(void) {
    const FpTarget &t = g_fp_target;
    return g_fp_frame.fp && t.w == g_bb_w && t.h == g_bb_h && t.fmt == D3DFMT_R32F;
}
static void fpd_update(void) {
    FpFrame &f = g_fp_frame;
    const FpTarget &t = g_fp_target;
    f.fp = (f.late || f.proj) && t.rt && !t.bb && !hud_screen_in_pass();
    // FP_DEPTH draws over (whichever call ended them, the target already being the new one): the game's shader back
    if (!fp_depth_draw_now()) fp_depth_scale_restore();
}

// Every Draw*Primitive*, before the draw.
void d3d_fp_draw(void) {
    const FpTarget &t = g_fp_target;
    FpFrame &f = g_fp_frame;
    if (hud_screen_in_pass() || t.bb) return;
    const bool bb_size = g_bb_w && t.w == g_bb_w && t.h == g_bb_h;
    if (bb_size && t.fmt == D3DFMT_R32F) f.seen_depth = true;
    else if (bb_size && t.fmt == D3DFMT_A16B16G16R16F && t.ds && !f.seen_depth) f.seen_main = true;
    if (g_fp_depth_scale && fp_depth_draw_now()) fp_depth_scale_draw();
}

// After render target 0 or the depth-stencil surface changed (also after CreateDevice, Reset, the HUD screen pass).
void d3d_fp_target(IDirect3DDevice9 *d) {
    g_fp_device = d;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *bb = nullptr;
    D3DSURFACE_DESC desc = {};
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (rt) rt->GetDesc(&desc);
    g_fp_target = {rt, desc.Width, desc.Height, (uint32_t)desc.Format, ds != nullptr, rt && rt == bb};
    const FpTarget &t = g_fp_target;
    // the late pass goes on into other back-buffer-sized targets, and ends at any other
    if (g_fp_frame.late && (!rt || t.bb || t.w != g_bb_w || t.h != g_bb_h)) g_fp_frame.late = false;
    fpd_update();
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (bb) bb->Release();
}

// Before a Clear: one of depth without colour into a back-buffer-sized target, once the main pass has drawn, may
// start the late pass. Its projection confirms it (d3d_fp_projection).
void d3d_fp_clear(uint32_t flags) {
    if (!(flags & D3DCLEAR_ZBUFFER) || (flags & D3DCLEAR_TARGET)) return;
    const FpTarget &t = g_fp_target;
    FpFrame &f = g_fp_frame;
    if (!t.rt || t.bb || !g_bb_w || t.w != g_bb_w || t.h != g_bb_h || !f.seen_main || f.late) return;
    f.late = true;
    f.late_confirmed = false;
    fpd_update();
}

// The world camera's near plane (NiCamera W, see game_camera_read below), or 0.
static float fpd_world_near(void) {
    uint32_t root = M32(0x1B2E8BC), w = root >= 0x10000 ? M32(root + 0xB8) : 0;
    float v = 0;
    if (w >= 0x10000) memcpy(&v, MEM + w + 0xF8, 4);
    return v;
}

// SetTransform(D3DTS_PROJECTION), m the guest's row-major matrix (row vectors): perspective has _34 = 1, and its near
// plane is -_43 / _33. In the late pass the first projection confirms the pass (a near plane other than the world's)
// and teaches the first-person near plane; a later one with another near plane ends it. Outside it, the projection
// is a first-person one when its near plane is the one learnt.
void d3d_fp_projection(const float *m) {
    FpFrame &f = g_fp_frame;
    const float near_plane = m[11] == 1.0f && m[10] != 0.0f ? -m[14] / m[10] : 0.0f;  // 0: not perspective
    const float world = fpd_world_near();
    auto same = [](float a, float b) { return a > 0 && b > 0 && fabsf(a - b) <= 1e-3f * b; };
    g_proj_p33 = near_plane > 0 ? m[10] : 0.0f;
    if (same(near_plane, world)) g_world_p33 = m[10];
    if (f.late && !f.late_confirmed) {
        if (near_plane <= 0 || same(near_plane, world)) {
            f.late = false;  // not the first-person camera (the loading screen's model has the world's)
        } else {
            f.late_confirmed = true;
            if (!same(near_plane, g_fpd_near)) {
                LOG("first-person near plane %g (world %g)", near_plane, world);
                g_fpd_near = near_plane;
            }
        }
    } else if (f.late && !same(near_plane, g_fpd_near)) {
        f.late = false;
    }
    f.proj = same(near_plane, g_fpd_near) && !same(near_plane, world);
    fpd_update();
}

/* First-person depth in the world's encoding. The scene's R32F depth target holds what the game's depth pixel
 * shaders write: clip z / clip w through the projection in use, d = P33 (1 - near / z) with P33 = far / (far - near)
 * (~1.00001). The world's draws use the world camera's near plane (15), the FP_DEPTH draws the first-person camera's
 * (5), so a consumer decoding with one near plane (the immersive compositor, ImmersiveScreen.swift, and the offline
 * tools) puts hands and weapons three times too far. The fix rewrites the value as it is written: for those draws
 * only, the pixel shader bound is swapped for a copy of itself whose colour output x is passed through
 *     d' = A d + B,   A = r P33w / P33fp,   B = P33w (1 - r),   r = world near / first-person near
 * which is exactly P33w (1 - near_world / z): the world's encoding of the same view depth z. Both near planes and
 * P33 come from the projections the game sets (d3d_fp_projection), never fixed numbers. d' < 0 for z below the
 * world's near plane (down to 1 - r at the first-person near plane); R32F keeps it and decoders give
 * z = near_world / (1 - d') as for every other pixel.
 *
 * Only the colour output changes: the vertex shader, clip z/w and so the z-buffer, the depth test between the
 * first-person meshes and clipping stay exactly as the game drew them. Seen in Riverwood and Whiterun (iron sword,
 * Flames, empty hands): the depth draws' pixel shaders are 7-instruction ps_3_0 programs, `rcp r0.x, v0.y;
 * mul oC0.x, r0.x, v0.x; mov oC0.yzw, 1` with an alpha-tested variant (texld, texkill); the same shaders draw the
 * prepass and the shadow-mask pass, which are left alone. The copy is made once per (shader, A, B) by rewriting the
 * bytecode (fp_depth_patch): every write to oC0 goes to a free temporary, and the end gets `mov oC0.yzw, rT` and
 * `mad oC0.x, rT.x, cK.x, cK.y` with A and B in a `def` of a free constant, so no shader constant the game sets is
 * touched. A shader the rewrite does not understand is left as it is (logged once). The game shadows its own shader
 * state (it skips SetPixelShader for the shader it thinks is bound), so the original is bound again as soon as the
 * draws stop being FP_DEPTH (fpd_update), before any other draw. The game's only later reader of the target, the
 * depth-of-field composite, blurs nothing as close as the hands in either encoding.
 *
 * Cost: the hooks run on every draw, target change, Clear and projection (a few compares each); the swap is one
 * GetPixelShader, a search of a few cached shaders and one SetPixelShader per FP_DEPTH draw (five a frame), and one
 * SetPixelShader to put the original back. On by default: every consumer of the depth target then sees one
 * encoding. LB_FP_DEPTH_SCALE=0 turns it off (and the hooks with it); LB_FP_DEPTH_SCALE=off@F[,F...] turns it off
 * only on frames F (numbered as LB_SHOT numbers them), to compare the picture with and without it in one run. */
struct FpDepthPatched {
    IDirect3DPixelShader9 *original;  // held, so its address is not reused while it keys the cache
    IDirect3DPixelShader9 *patched;   // null: the rewrite failed, draw with the original
    float a, b;
};
static std::vector<FpDepthPatched> g_fp_depth_cache;
static IDirect3DPixelShader9 *g_fp_depth_bound_original, *g_fp_depth_bound_patched;  // the swap in force

static void fp_depth_scale_forget(void) {
    for (FpDepthPatched &p : g_fp_depth_cache) {
        if (p.original) p.original->Release();
        if (p.patched) p.patched->Release();
    }
    g_fp_depth_cache.clear();
    g_fp_depth_bound_original = g_fp_depth_bound_patched = nullptr;
}

/* A copy of the ps_2_0+ bytecode `in` (DWORD tokens up to and including the end token) whose colour output 0's x
 * component is a * x + b. Empty if the shader is not one this can rewrite safely: shader model 1, no write to oC0,
 * relative addressing of constants, subroutines (the outputs would be written after main's ret), or no free temporary
 * or constant register. */
static std::vector<uint32_t> fp_depth_patch(const uint32_t *in, size_t count, float a, float b) {
    std::vector<uint32_t> none;
    if (count < 2 || (in[0] >> 16) != 0xFFFF || ((in[0] >> 8) & 0xFF) < 2) return none;
    auto reg_type = [](uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); };
    enum { D3DSPR_TEMP_ = 0, D3DSPR_CONST_ = 2, D3DSPR_COLOROUT_ = 8 };
    const uint32_t op_dcl = 31, op_def = 81, op_defi = 48, op_defb = 47, op_ret = 28, op_label = 30;
    int max_temp = -1, max_const = -1;
    uint32_t written = 0;  // write mask union of the oC0 writes
    size_t end = 0, first_code = 0;
    std::vector<size_t> oc0_dst;  // token indices of oC0 destinations
    for (size_t i = 1; i < count;) {
        const uint32_t tok = in[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFF) { end = i; break; }
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }  // comment (CTAB...)
        if (!first_code) first_code = i;
        const size_t len = (tok >> 24) & 0xF;
        if (i + 1 + len > count) return none;
        const uint32_t *arg = in + i + 1;
        if (op == op_ret || op == op_label) return none;  // the end is not main's end
        if (op == op_def || op == op_defi || op == op_defb) {
            if (len && reg_type(arg[0]) == D3DSPR_CONST_ && op == op_def) max_const = std::max(max_const, (int)(arg[0] & 0x7FF));
        } else if (op == op_dcl) {
            // usage token, then the declared register (inputs, samplers): nothing to track
        } else {
            for (size_t j = 0; j < len; j++) {
                const uint32_t t = arg[j], type = reg_type(t), num = t & 0x7FF;
                if (type == D3DSPR_TEMP_) max_temp = std::max(max_temp, (int)num);
                if (type == D3DSPR_CONST_) {
                    max_const = std::max(max_const, (int)num);
                    if (t & (1u << 13)) return none;  // c[a0 + n]: which constants are free is unknown
                }
                if (j == 0 && type == D3DSPR_COLOROUT_ && num == 0) {
                    oc0_dst.push_back(i + 1);
                    written |= (t >> 16) & 0xF;
                }
            }
        }
        i += 1 + len;
    }
    if (!end || oc0_dst.empty() || !(written & 1) || !first_code) return none;
    const uint32_t temp = (uint32_t)(max_temp + 1), konst = (uint32_t)(max_const + 1);
    if (temp >= 12 || konst >= 32) return none;  // stay within every ps_2_0+ limit
    std::vector<uint32_t> out(in, in + end);
    for (size_t at : oc0_dst) out[at] = (out[at] & ~(0x7u << 28) & ~(0x3u << 11) & ~0x7FFu) | temp;  // -> rT
    const uint32_t param = 0x80000000u, oc0 = param | (1u << 11), rt = param | temp, c = param | (2u << 28) | konst;
    uint32_t fa, fb, zero = 0;
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    const uint32_t def[] = {op_def | (5u << 24), c | (0xFu << 16), fa, fb, zero, zero};
    out.insert(out.begin() + first_code, def, def + 6);
    if (written & 0xE) {  // mov oC0.(written but x), rT
        const uint32_t mov[] = {1u | (2u << 24), oc0 | ((written & 0xE) << 16), rt | (0xE4u << 16)};
        out.insert(out.end(), mov, mov + 3);
    }
    // mad oC0.x, rT.x, cK.x, cK.y
    const uint32_t mad[] = {4u | (4u << 24), oc0 | (1u << 16), rt | (0x00u << 16), c | (0x00u << 16), c | (0x55u << 16)};
    out.insert(out.end(), mad, mad + 5);
    out.push_back(0x0000FFFF);
    return out;
}

// Before an FP_DEPTH draw: bind the rewritten copy of the game's pixel shader (fp_depth_patch).
static void fp_depth_scale_draw(void) {
    IDirect3DDevice9 *d = g_fp_device;
    const float world = fpd_world_near();
    if (!d || !g_fp_frame.proj || g_fpd_near <= 0 || world <= g_fpd_near * 1.0001f || g_proj_p33 <= 0) return;
    if (!g_fp_depth_scale_off.empty() &&
        std::find(g_fp_depth_scale_off.begin(), g_fp_depth_scale_off.end(), g_frame + 1) != g_fp_depth_scale_off.end())
        return;
    const float r = world / g_fpd_near, p33w = g_world_p33 > 0 ? g_world_p33 : g_proj_p33;
    const float a = r * p33w / g_proj_p33, b = p33w * (1.0f - r);
    IDirect3DPixelShader9 *ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
    ps->Release();  // the device still holds it; the cache takes its own reference
    if (ps == g_fp_depth_bound_patched) return;  // still bound from the previous draw (the game skipped a set)
    FpDepthPatched *hit = nullptr;
    for (FpDepthPatched &p : g_fp_depth_cache)
        if (p.original == ps && p.a == a && p.b == b) { hit = &p; break; }
    if (!hit) {
        FpDepthPatched p = {ps, nullptr, a, b};
        ps->AddRef();
        UINT size = 0;
        std::vector<uint32_t> code;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
            code.resize(size / 4);
            if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
        }
        const std::vector<uint32_t> patched = code.empty() ? code : fp_depth_patch(code.data(), code.size(), a, b);
        if (patched.empty() || FAILED(d->CreatePixelShader((const DWORD *)patched.data(), &p.patched))) p.patched = nullptr;
        LOG("first-person depth: pixel shader %p (%u bytes) %s, d' = %.6g d %+.6g (near %g -> %g)", (void *)ps, size,
            p.patched ? "rewritten" : "NOT rewritten, its depth stays in the first-person encoding", a, b, g_fpd_near,
            world);
        if (g_fp_depth_cache.size() > 256) fp_depth_scale_forget();  // near planes changing all the time: start over
        g_fp_depth_cache.push_back(p);
        hit = &g_fp_depth_cache.back();
    }
    if (!hit->patched) return;
    d->SetPixelShader(hit->patched);
    g_fp_depth_bound_original = ps;
    g_fp_depth_bound_patched = hit->patched;
}

// The FP_DEPTH draws are over: give the game back the pixel shader it thinks is bound.
static void fp_depth_scale_restore(void) {
    if (!g_fp_depth_bound_patched) return;
    IDirect3DDevice9 *d = g_fp_device;
    IDirect3DPixelShader9 *ps = nullptr;
    if (d && SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
        if (ps == g_fp_depth_bound_patched) d->SetPixelShader(g_fp_depth_bound_original);
        ps->Release();
    }
    g_fp_depth_bound_original = g_fp_depth_bound_patched = nullptr;
}

/* LB_FP_FLAME_DEPTH=1 or =<units> (off by default): live first-person hybrid, tier A (plans: live-hybrid.md). The
 * hand's effects (Flames: a few billboards blended into the scene colour target in the late first-person pass) write
 * no depth, so the immersive screen's depth reprojection puts them on whatever is behind them, usually the ground.
 * Before each such draw, the same draw is made into the scene's R32F depth target (d3d_scene_depth_target) with a
 * rewritten copy of its pixel shader (flame_patch): colour = the depth of a point <units> in front of the camera in
 * the world's encoding (default 34, the measured distance of the hands), alpha = the original's alpha times its
 * brightest channel, alpha test at 0.25, no blending, depth test as the game set it (so the sword in front still
 * hides the flame) and no depth write. The later first-person depth draws then write the hands over it as before.
 * All state touched is put back before the game's own draw. */
IDirect3DSurface9 *d3d_scene_depth_target(void);  // hud_screen.cpp
int g_fp_flame;
static float g_flame_units;
struct FlamePatched { IDirect3DPixelShader9 *original, *patched; float depth; };
static std::vector<FlamePatched> g_flame_cache;
static int g_flame_draws, g_flame_logged_frame = -1;

static void flame_init(void) {
    static bool read;
    if (read) return;
    read = true;
    const char *e = getenv("LB_FP_FLAME_DEPTH");
    if (!e || !*e || !strcmp(e, "0")) return;
    g_flame_units = atof(e) > 1.0 ? (float)atof(e) : 34.0f;
    g_fp_flame = 1;
    LOG("first-person flame depth stamp on: %g units", g_flame_units);
}

static void flame_forget(void) {
    for (FlamePatched &p : g_flame_cache) {
        if (p.original) p.original->Release();
        if (p.patched) p.patched->Release();
    }
    g_flame_cache.clear();
}

// A copy of ps_2_0+ bytecode whose colour output 0 is (depth, depth, depth, alpha * max(r, g, b)) of the original's
// output. Empty if not rewritable (as fp_depth_patch).
static std::vector<uint32_t> flame_patch(const uint32_t *in, size_t count, float depth) {
    std::vector<uint32_t> none;
    if (count < 2 || (in[0] >> 16) != 0xFFFF || ((in[0] >> 8) & 0xFF) < 2) return none;
    auto reg_type = [](uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); };
    enum { D3DSPR_TEMP_ = 0, D3DSPR_CONST_ = 2, D3DSPR_COLOROUT_ = 8 };
    const uint32_t op_dcl = 31, op_def = 81, op_defi = 48, op_defb = 47, op_ret = 28, op_label = 30;
    int max_temp = -1, max_const = -1;
    size_t end = 0, first_code = 0;
    std::vector<size_t> oc0_dst;
    for (size_t i = 1; i < count;) {
        const uint32_t tok = in[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFF) { end = i; break; }
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        if (!first_code) first_code = i;
        const size_t len = (tok >> 24) & 0xF;
        if (i + 1 + len > count) return none;
        const uint32_t *arg = in + i + 1;
        if (op == op_ret || op == op_label) return none;
        if (op == op_def || op == op_defi || op == op_defb) {
            if (len && reg_type(arg[0]) == D3DSPR_CONST_ && op == op_def) max_const = std::max(max_const, (int)(arg[0] & 0x7FF));
        } else if (op != op_dcl) {
            for (size_t j = 0; j < len; j++) {
                const uint32_t t = arg[j], type = reg_type(t), num = t & 0x7FF;
                if (type == D3DSPR_TEMP_) max_temp = std::max(max_temp, (int)num);
                if (type == D3DSPR_CONST_) {
                    max_const = std::max(max_const, (int)num);
                    if (t & (1u << 13)) return none;
                }
                if (j == 0 && type == D3DSPR_COLOROUT_ && num == 0) oc0_dst.push_back(i + 1);
            }
        }
        i += 1 + len;
    }
    if (!end || oc0_dst.empty() || !first_code) return none;
    const uint32_t temp = (uint32_t)(max_temp + 1), temp2 = temp + 1, konst = (uint32_t)(max_const + 1);
    if (temp2 >= 12 || konst >= 32) return none;
    std::vector<uint32_t> out(in, in + end);
    for (size_t at : oc0_dst) out[at] = (out[at] & ~(0x7u << 28) & ~(0x3u << 11) & ~0x7FFu) | temp;  // -> rT
    const uint32_t param = 0x80000000u, oc0 = param | (1u << 11), rt = param | temp, ru = param | temp2,
                   c = param | (2u << 28) | konst;
    uint32_t fd, zero = 0;
    memcpy(&fd, &depth, 4);
    const uint32_t def[] = {81u | (5u << 24), c | (0xFu << 16), fd, zero, zero, zero};
    out.insert(out.begin() + first_code, def, def + 6);
    const uint32_t tail[] = {
        11u | (3u << 24), ru | (1u << 16), rt | (0x00u << 16), rt | (0x55u << 16),  // max rU.x, rT.x, rT.y
        11u | (3u << 24), ru | (1u << 16), ru | (0x00u << 16), rt | (0xAAu << 16),  // max rU.x, rU.x, rT.z
        5u | (3u << 24), oc0 | (8u << 16), ru | (0x00u << 16), rt | (0xFFu << 16),   // mul oC0.w, rU.x, rT.w
        1u | (2u << 24), oc0 | (7u << 16), c | (0x00u << 16),                       // mov oC0.xyz, cK.x
    };
    out.insert(out.end(), tail, tail + sizeof tail / sizeof tail[0]);
    out.push_back(0x0000FFFF);
    return out;
}

void d3d_fp_flame_draw(IDirect3DDevice9 *d, int indexed, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                       uint32_t a5, uint32_t a6) {
    if (!d || fp_kind() != FP_COLOUR) return;
    IDirect3DSurface9 *depth_rt = d3d_scene_depth_target();
    const float world = fpd_world_near(), p33w = g_world_p33;
    if (!depth_rt || world <= 0 || p33w <= 0 || g_flame_units <= world) return;
    DWORD blend = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend) return;  // the hands and the sword: opaque, their depth comes from the first-person depth draws
    const float depth = p33w * (1.0f - world / g_flame_units);
    IDirect3DPixelShader9 *ps = nullptr;
    if (FAILED(d->GetPixelShader(&ps)) || !ps) return;
    ps->Release();
    FlamePatched *hit = nullptr;
    for (FlamePatched &p : g_flame_cache)
        if (p.original == ps && p.depth == depth) { hit = &p; break; }
    if (!hit) {
        FlamePatched p = {ps, nullptr, depth};
        ps->AddRef();
        UINT size = 0;
        std::vector<uint32_t> code;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
            code.resize(size / 4);
            if (FAILED(ps->GetFunction(code.data(), &size))) code.clear();
        }
        const std::vector<uint32_t> patched = code.empty() ? code : flame_patch(code.data(), code.size(), depth);
        if (patched.empty() || FAILED(d->CreatePixelShader((const DWORD *)patched.data(), &p.patched))) p.patched = nullptr;
        LOG("first-person flame depth: pixel shader %p (%u bytes) %s, depth %.6g (%g units)", (void *)ps, size,
            p.patched ? "rewritten" : "NOT rewritten, no stamp", depth, g_flame_units);
        if (g_flame_cache.size() > 256) flame_forget();
        g_flame_cache.push_back(p);
        hit = &g_flame_cache.back();
    }
    if (!hit->patched) return;
    IDirect3DSurface9 *rt0 = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return;
    D3DVIEWPORT9 vp;
    d->GetViewport(&vp);
    static const D3DRENDERSTATETYPE k_states[] = {D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
                                                  D3DRS_ALPHAFUNC, D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE,
                                                  D3DRS_SRGBWRITEENABLE};
    DWORD saved[sizeof k_states / sizeof k_states[0]];
    for (size_t i = 0; i < sizeof k_states / sizeof k_states[0]; i++) d->GetRenderState(k_states[i], &saved[i]);
    d->SetRenderTarget(0, depth_rt);
    d->SetViewport(&vp);
    d->SetPixelShader(hit->patched);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    d->SetRenderState(D3DRS_ALPHAREF, 64);
    d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (indexed) d->DrawIndexedPrimitive((D3DPRIMITIVETYPE)a1, (INT)a2, a3, a4, a5, a6);
    else d->DrawPrimitive((D3DPRIMITIVETYPE)a1, a2, a3);
    d->SetRenderTarget(0, rt0);
    rt0->Release();
    d->SetViewport(&vp);
    d->SetPixelShader(ps);
    for (size_t i = 0; i < sizeof k_states / sizeof k_states[0]; i++) d->SetRenderState(k_states[i], saved[i]);
    g_flame_draws++;
    if (g_flame_logged_frame < 0 || g_frame - g_flame_logged_frame >= 600) {
        g_flame_logged_frame = g_frame;
        LOG("first-person flame depth: %d stamps so far (frame %d)", g_flame_draws, g_frame);
    }
}

// After CreateDevice: reads the switch once and learns the targets the device starts with.
static void fp_init(IDirect3DDevice9 *d) {
    static bool read;
    if (!read) {
        read = true;
        const char *e = getenv("LB_FP_DEPTH_SCALE");
        g_fp_depth_scale = !e || strcmp(e, "0") != 0;
        if (e && !strncmp(e, "off@", 4))
            for (const char *p = e + 4; p && *p;) {
                g_fp_depth_scale_off.push_back(atoi(p));
                p = strchr(p, ',');
                if (p) p++;
            }
        g_d3d_fp = g_fp_depth_scale;
        LOG("first-person depth in the world's encoding: %s%s", g_fp_depth_scale ? "on" : "off (LB_FP_DEPTH_SCALE=0)",
            g_fp_depth_scale_off.empty() ? "" : " (off on the LB_FP_DEPTH_SCALE=off@ frames)");
    }
    fp_depth_scale_forget();  // a new device: shaders made for the old one are gone
    flame_forget();
    if (!g_d3d_fp) return;
    d3d_fp_target(d);
}

// At Present: a new frame.
static void fp_present(void) {
    if (!g_d3d_fp) return;
    g_fp_frame.late = g_fp_frame.proj = false;
    fpd_update();
    g_fp_frame = FpFrame();
}

// LB_SHOT=F[,F...]: write the backbuffer about to be presented on those
// frames to <save>/shot-<frame>.bmp. Off unless set; for headless verification
// where no screen capture permission exists.
// With LB_SHOT, the scene's depth target of the same frame goes to <save>/depth-<frame>.f32: width and height
// as two uint32, then rows of float32 exactly as the game wrote them. Input for trying depth-based stereo off
// the headset (docs/perf/IMMERSIVE_OPTIONS.md).
IDirect3DSurface9 *d3d_scene_depth_target(void);
void d3d_scene_depth_reset(void);
#if defined(__APPLE__)
extern "C" void dxvk_present_tap_set_depth(void *surface);  // DXVK present tap (dxvk_present_tap.h)
extern "C" long dxvk_stereo_get_render_target_data_layer(void *, void *, void *, unsigned);  // dxvk_stereo.h
extern "C" int dxvk_stereo_surface_layers(void *);
#endif

#if defined(__APPLE__)
/* Whether a menu is up (TESV 1.9.32.0.8): the menu manager singleton's menu stack count (+0x9C; 1 is the HUD) and
 * its count of open menus that pause the game (+0xC8). With a menu up the immersive screen gets no depth and
 * shows the frame flat, so menus and their text aren't bent by the scene behind them. Changes are logged with
 * both counts, since only the main menu, console and pause menu were checked. */
static bool menu_open(void) {
    uint32_t mm = M32(0x12E3548);
    if (mm < 0x10000) return false;
    uint32_t open_menus = M32(mm + 0x9C), pause_menus = M32(mm + 0xC8);
    bool open = pause_menus != 0 || open_menus > 1;
    static uint64_t last = ~0ull;
    uint64_t now = ((uint64_t)open_menus << 32) | pause_menus;
    if (now != last) {
        last = now;
        fprintf(stderr, "menus: %u open, %u pausing -> screen %s\n", open_menus, pause_menus, open ? "flat" : "depth");
    }
    return open;
}
extern "C" int skyrim_menu_open(void) { return menu_open() ? 1 : 0; }
#endif

// tag: the file name after "depth-" / "shot-" (the frame number, plus the eye for stereo captures).
static void shot_depth(IDirect3DDevice9 *d, const char *tag) {
    IDirect3DSurface9 *depth = d3d_scene_depth_target(), *copy = nullptr;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    if (!depth) { LOG("LB_SHOT frame %d: no scene depth target seen", g_frame); return; }
    HRESULT hr = depth->GetDesc(&desc);
    if (SUCCEEDED(hr)) hr = d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
    if (SUCCEEDED(hr)) hr = d->GetRenderTargetData(depth, copy);
    if (SUCCEEDED(hr)) hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        char path[1100];
        snprintf(path, sizeof path, "%s/depth-%s.f32", g_save_dir, tag);
        FILE *out = fopen(path, "wb");
        if (out) {
            uint32_t size[2] = {desc.Width, desc.Height};
            fwrite(size, 4, 2, out);
            for (uint32_t y = 0; y < desc.Height; y++) fwrite((const uint8_t *)locked.pBits + (size_t)y * locked.Pitch, 4, desc.Width, out);
            fclose(out);
        }
        copy->UnlockRect();
        LOG("LB_SHOT frame %d depth %ux%u -> %s", g_frame, desc.Width, desc.Height, out ? path : "(open failed)");
#if defined(__APPLE__)
        // Single-pass stereo (LB_STEREO_MULTIVIEW): the second eye's depth, from layer 1, beside it.
        if (dxvk_stereo_surface_layers(depth) == 2 && SUCCEEDED((HRESULT)dxvk_stereo_get_render_target_data_layer(d, depth, copy, 1))
         && SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            snprintf(path, sizeof path, "%s/depth-%s-layer1.f32", g_save_dir, tag);
            FILE *out1 = fopen(path, "wb");
            if (out1) {
                uint32_t size[2] = {desc.Width, desc.Height};
                fwrite(size, 4, 2, out1);
                for (uint32_t y = 0; y < desc.Height; y++) fwrite((const uint8_t *)locked.pBits + (size_t)y * locked.Pitch, 4, desc.Width, out1);
                fclose(out1);
            }
            copy->UnlockRect();
            LOG("LB_SHOT frame %d depth layer 1 -> %s", g_frame, out1 ? path : "(open failed)");
        }
#endif
    } else {
        LOG("LB_SHOT frame %d depth failed: hr %08x", g_frame, (unsigned)hr);
    }
    if (copy) copy->Release();
}

static void shot_write(IDirect3DDevice9 *d, const char *tag, bool depth);

// Whether LB_SHOT names this frame.
static bool shot_frame_wanted(int frame) {
    static int frames[16], count = -1;
    if (count < 0) {
        count = 0;
        const char *e = getenv("LB_SHOT");
        while (e && *e && count < 16) {
            frames[count++] = atoi(e);
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    for (int i = 0; i < count; i++) if (frames[i] == frame) return true;
    return false;
}

// LB_STEREO_ALTERNATE with LB_STEREO_EYE_UNITS set (Apple only): the stereo section below then names LB_SHOT's
// files by eye, so the plain shot is skipped.
static bool stereo_alternate_env(void) {
#if defined(__APPLE__)
    const char *a = getenv("LB_STEREO_ALTERNATE"), *d = getenv("LB_STEREO_EYE_UNITS");
    return a && atoi(a) != 0 && d && atof(d) != 0;
#else
    return false;
#endif
}

static void shot_before_present(IDirect3DDevice9 *d) {
    if (stereo_alternate_env() || !shot_frame_wanted(g_frame)) return;
    char tag[32];
    snprintf(tag, sizeof tag, "%05d", g_frame);
    shot_write(d, tag, true);
}

// The back buffer about to be presented to <save>/shot-<tag>.bmp, and with depth the scene's depth target to
// <save>/depth-<tag>.f32.
static void shot_write_layer(IDirect3DDevice9 *d, const char *tag, bool depth, unsigned layer);
static void shot_write(IDirect3DDevice9 *d, const char *tag, bool depth) {
    shot_write_layer(d, tag, depth, 0);
#if defined(__APPLE__)
    // Stereo back buffer (LB_STEREO_PRESENT): eye 2's picture as shot-<tag>-layer1.bmp
    IDirect3DSurface9 *back = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        const bool two = dxvk_stereo_surface_layers(back) == 2;
        back->Release();
        if (two) shot_write_layer(d, tag, false, 1);
    }
#endif
}
static void shot_write_layer(IDirect3DDevice9 *d, const char *tag, bool depth, unsigned layer) {
    if (depth) shot_depth(d, tag);
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    HRESULT hr = d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (SUCCEEDED(hr)) hr = back->GetDesc(&desc);
    if (SUCCEEDED(hr) && desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
#if defined(__APPLE__)
    if (SUCCEEDED(hr)) hr = layer ? (HRESULT)dxvk_stereo_get_render_target_data_layer(d, back, copy, layer) : d->GetRenderTargetData(back, copy);
#else
    if (SUCCEEDED(hr)) hr = d->GetRenderTargetData(back, copy);
#endif
    if (SUCCEEDED(hr)) hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        char path[1100];
        if (layer) snprintf(path, sizeof path, "%s/shot-%s-layer%u.bmp", g_save_dir, tag, layer);
        else snprintf(path, sizeof path, "%s/shot-%s.bmp", g_save_dir, tag);
        FILE *out = fopen(path, "wb");
        uint32_t w = desc.Width, h = desc.Height, row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        uint8_t header[54] = {'B', 'M'};
        auto put = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
        put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h);
        header[26] = 1; header[28] = 24; put(34, row * h);
        if (out) {
            fwrite(header, 1, 54, out);
            std::vector<uint8_t> line(row, 0);
            for (uint32_t y = h; y-- > 0;) {  // BMP rows run bottom-up
                const uint8_t *src = (const uint8_t *)locked.pBits + (size_t)y * locked.Pitch;
                for (uint32_t x = 0; x < w; x++) memcpy(&line[x * 3], src + x * 4, 3);  // BGRX -> BGR
                fwrite(line.data(), 1, row, out);
            }
            fclose(out);
        }
        copy->UnlockRect();
        LOG("LB_SHOT frame %d %ux%u -> %s", g_frame, w, h, out ? path : "(open failed)");
    } else {
        LOG("LB_SHOT frame %d failed: hr %08x", g_frame, (unsigned)hr);
    }
    if (copy) copy->Release();
    if (back) back->Release();
}

#if defined(__APPLE__)
// Skyrim's physics and scripts assume at most 60 fps. DXVK's d3d9.maxFrameRate
// sleeps while holding the D3D9 device lock, which stalled Skyrim's main-thread
// loading behind the loading-screen thread's presents; pace here instead, after
// Present has returned and no DXVK lock is held. LB_MAX_FPS overrides, 0 = off.
//
// Display ticks: in immersive play the visionOS compositor calls skyrim_display_tick() once per display refresh,
// right after taking the newest finished game frame. While ticks arrive and the frame rate asked for is the refresh
// rate divided by a whole number (LB_MAX_FPS=45 at 90 Hz: every second refresh), the game is released on those
// ticks instead of by its own clock. Each frame then starts at the same point of the display's cycle, and one that
// is finished in time is shown for exactly that many refreshes, rather than sometimes one and sometimes three as
// two unsynchronised clocks drift past each other. A frame that runs late is released at once, and counting
// restarts from its tick.
static std::mutex g_tick_lock;
static std::condition_variable g_tick_cv;
static uint64_t g_ticks;
static int64_t g_tick_ns, g_tick_period_ns;

static int64_t pace_now_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

extern "C" void skyrim_display_tick(void) {
    const int64_t now = pace_now_ns();
    {
        std::lock_guard<std::mutex> guard(g_tick_lock);
        const int64_t gap = now - g_tick_ns;
        if (g_tick_ns && gap > 0 && gap < 50000000)
            g_tick_period_ns = g_tick_period_ns ? (g_tick_period_ns * 15 + gap) / 16 : gap;
        g_tick_ns = now;
        g_ticks++;
    }
    g_tick_cv.notify_all();
}

// Refreshes per game frame when pacing on display ticks, else 0 (no recent ticks, or no whole-number ratio).
// N refreshes may be up to 15% longer than the interval asked for (a lower frame rate), but not shorter beyond
// clock noise, so LB_MAX_FPS stays a maximum: 45 at 96 Hz stays on the timer instead of running at 48.
static int ticks_per_frame(int64_t interval_ns, int64_t now) {
    if (!g_tick_period_ns || now - g_tick_ns > 100000000) return 0;
    const double ratio = (double)interval_ns / (double)g_tick_period_ns;
    const long n = std::lround(ratio);
    return n >= 1 && ratio - (double)n < 0.02 * (double)n && (double)n - ratio < 0.15 ? (int)n : 0;
}

static void pace_present(void) {
    static std::mutex lock;
    static int64_t interval_ns = -1, next_ns = 0;
    static uint64_t released_tick;
    static int last_mode = -1;
    int64_t wait_ns;
    {
        std::unique_lock<std::mutex> guard(lock);
        if (interval_ns < 0) {
            const char *e = getenv("LB_MAX_FPS");
#if defined(__APPLE__) && defined(TARGET_OS_VISION) && TARGET_OS_VISION
            // Apple Vision Pro: 45 fps on every second 90 Hz refresh (display ticks); the compositor reprojects
            // each frame to the head pose of both refreshes. The budget per game frame is then 22 ms, room for
            // both eyes. LB_MAX_FPS overrides (60, 90, 0 = off).
            int fps = e ? atoi(e) : 45;
#else
            int fps = e ? atoi(e) : 60;
#endif
            interval_ns = fps > 0 ? 1000000000ll / fps : 0;
            LOG("present pacing: %s %d fps (LB_MAX_FPS)", fps > 0 ? "at most" : "off,", fps > 0 ? fps : 0);
        }
        if (!interval_ns) return;
        const int64_t now = pace_now_ns();
        std::unique_lock<std::mutex> ticks(g_tick_lock);
        const int per = ticks_per_frame(interval_ns, now);
        if (per != last_mode) {
            if (per) LOG("present pacing: on display ticks, every %d refreshes", per);
            else if (last_mode > 0) LOG("present pacing: display ticks stopped, back to the timer");
            last_mode = per;
            released_tick = g_ticks;
        }
        if (per) {
            const uint64_t target = released_tick + (uint64_t)per;
            next_ns = 0;
            guard.unlock();  // only the tick lock is held while waiting
            if (g_ticks < target) {
                const auto before = std::chrono::steady_clock::now();
                // The deadline covers a compositor that stops ticking mid-wait.
                g_tick_cv.wait_for(ticks, std::chrono::nanoseconds((per + 2) * g_tick_period_ns),
                                   [&] { return g_ticks >= target; });
                platform_present_slept(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
            }
            const uint64_t now_tick = g_ticks;
            ticks.unlock();
            guard.lock();
            released_tick = now_tick;
            return;
        }
        ticks.unlock();
        const int64_t slot = next_ns > now ? next_ns : now;  // no catch-up bursts after stalls
        next_ns = slot + interval_ns;
        wait_ns = slot - now;
    }
    if (wait_ns > 0) {
        const auto before = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
        platform_present_slept(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
    }
}
#else
static void pace_present(void) {}
#endif

/* The game's main world camera as it was last set up (TESV 1.9.32.0.8, docs/VISION_PRO_PLAN.md M3): NiCamera W =
 * *(*(0x1B2E8BC)+0xB8); world rotation +0x54 (3x3, row-major), translation +0x78, frustum tangents l r t b +0xE8,
 * near/far +0xF8. Read at Present, i.e. for the frame being shown. */
struct GameCamera { float rot[9], pos[3], frustum[6]; };  /* 18 floats */
static bool game_camera_read(GameCamera *out) {
    uint32_t root = M32(0x1B2E8BC);
    if (root < 0x10000) return false;
    uint32_t w = M32(root + 0xB8);
    if (w < 0x10000) return false;
    memcpy(out->rot, MEM + w + 0x54, sizeof out->rot);
    memcpy(out->pos, MEM + w + 0x78, sizeof out->pos);
    memcpy(out->frustum, MEM + w + 0xE8, sizeof out->frustum);
    return true;
}

/* LB_D3DTRACE: the world camera's frustum (tangents l r t b, near, far) and the PlayerCamera FOVs, at a call that
 * may start a pass (Clear), and whenever they change between two traced calls. */
static float g_trace_cam[8];
void d3d_trace_camera(const char *why) {
    float v[8] = {};
    uint32_t root = M32(0x1B2E8BC), w = root >= 0x10000 ? M32(root + 0xB8) : 0, pc = M32(0x12E7288);
    if (w >= 0x10000) memcpy(v, MEM + w + 0xE8, 24);
    if (pc >= 0x10000) { memcpy(v + 6, MEM + pc + 0xAC, 4); memcpy(v + 7, MEM + pc + 0xB0, 4); }
    memcpy(g_trace_cam, v, sizeof v);
    fprintf(stderr, "D3D %d camera(%s) l %.4f r %.4f t %.4f b %.4f near %.3f far %.0f fov %.1f/%.1f\n", g_frame, why,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}
static void trace_camera_changed(void) {
    float v[8] = {};
    uint32_t root = M32(0x1B2E8BC), w = root >= 0x10000 ? M32(root + 0xB8) : 0, pc = M32(0x12E7288);
    if (w >= 0x10000) memcpy(v, MEM + w + 0xE8, 24);
    if (pc >= 0x10000) { memcpy(v + 6, MEM + pc + 0xAC, 4); memcpy(v + 7, MEM + pc + 0xB0, 4); }
    if (memcmp(v, g_trace_cam, sizeof v)) d3d_trace_camera("changed");
}

/* The camera of the frame being presented, for the immersive compositor (ipad/immersive_bridge.mm reads it from
 * the present tap's callback, which runs inside Present on this thread). */
static GameCamera g_frame_camera;
static bool g_frame_camera_valid;
extern "C" int skyrim_frame_camera(float out[18]) {
    if (!g_frame_camera_valid) return 0;
    memcpy(out, &g_frame_camera, sizeof g_frame_camera);
    return 1;
}

/* The eye of the frame being presented, filled at Present from the shift that frame was actually drawn with:
 * -1 left, +1 right, 0 centre (no shift: stereo off, a centre frame, a menu, no camera). k_out, if given, gets the
 * clip-space shift k (DXVK pos.x -= k) applied to that frame. */
static int g_frame_eye;
static float g_frame_eye_k;
extern "C" int skyrim_frame_eye(float *k_out) {
    if (k_out) *k_out = g_frame_eye_k;
    return g_frame_eye;
}

/* LB_FOV: the game's field of view in degrees at its 4:3 reference (65 by default, about 81 degrees across at
 * 16:9), written to PlayerCamera worldFOV (+0xAC) and firstPersonFOV (+0xB0) as the console's fov command
 * does. Head tracking wants it wider than the headset shows. */
static void game_fov_apply(void) {
    static float fov = -1;
    if (fov < 0) fov = getenv("LB_FOV") ? (float)atof(getenv("LB_FOV")) : 0;
    uint32_t pc = M32(0x12E7288);
    if (fov <= 0 || pc < 0x10000) return;
    memcpy(MEM + pc + 0xAC, &fov, 4);
    memcpy(MEM + pc + 0xB0, &fov, 4);
}

/* LB_CAMLOG=N: log the world camera every N frames (to check the layout above). */
static void camera_log(void) {
    static int every = -1;
    if (every < 0) every = getenv("LB_CAMLOG") ? atoi(getenv("LB_CAMLOG")) : 0;
    if (every <= 0 || g_frame % every) return;
    GameCamera cam;
    if (!game_camera_read(&cam)) { fprintf(stderr, "cam %d: none\n", g_frame); return; }
    const float *r = cam.rot, *f = cam.frustum;
    uint32_t pc = M32(0x12E7288);
    float fov = 0, fov1 = 0;
    if (pc >= 0x10000) { memcpy(&fov, MEM + pc + 0xAC, 4); memcpy(&fov1, MEM + pc + 0xB0, 4); }
    fprintf(stderr, "cam %d: rot [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f] pos %.1f %.1f %.1f "
            "frustum l %.3f r %.3f t %.3f b %.3f near %.2f far %.0f fov %.1f/%.1f\n", g_frame,
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], cam.pos[0], cam.pos[1], cam.pos[2],
            f[0], f[1], f[2], f[3], f[4], f[5], fov, fov1);
}

#if defined(__APPLE__)
/* True two-eye stereo captures (experiment, Mac): render chosen frames from a camera moved sideways, to compare
 * with the compositor's depth reprojection (tools/stereo_capture.py, tools/stereo_compare.py).
 *
 *   LB_STEREO_EYE_UNITS=d   half the eye separation in game units (2.24 = 32 mm at 70 units per metre). Also
 *                           what turns the shader code on inside DXVK (dxvk_stereo.h), so it must be set at launch.
 *   LB_STEREO_FRAMES=list   comma-separated items F, A-B or A-B:S (groups starting at A, A+S, ... up to B; S
 *                           defaults to, and is at least, the group's length). Each base F starts a group of frames,
 *                           each drawn one way and written as <save>/shot-F-<slot>.bmp with stereo-F-<slot>.txt (the
 *                           eye shift and camera used) and, for some, depth-F-<slot>.f32. By default a triplet:
 *                             L  frame F, the left eye     R  F+1, the right eye     C  F+2, the centre (+ depth)
 *   LB_STEREO_PASSES=firstperson
 *                           L and R shift only the first-person draws (d3d_first_person: the mask, prepass and late
 *                           pass, every kind), with no shift for the rest of the frame: hands, weapon and spell
 *                           effect truly per eye over the centre eye's world (twcclegg/skyrim-apple#23 chunk 1.2).
 *                           L and R then write depth too. DXVK reads the same variable: any value but size or all
 *                           keeps its default pass rule (viewports of the back buffer's aspect), which the
 *                           first-person draws meet, the 960x540 mask pass included. The mask is shifted with the
 *                           rest because it is sampled at the hands' screen position when the late pass lights them:
 *                           left at the centre position, the shifted hands would read the world's shadow there.
 *   LB_STEREO_HYBRID=1      each group is six frames of one game moment, for judging the first-person hybrid
 *                           against full true stereo and reprojection (tools/stereo_compare.py --hybrid):
 *                             L, R, C   as above (C with depth)
 *                             FL, FR    the left and right eye on first-person draws only, as with
 *                                       LB_STEREO_PASSES=firstperson (with depth)
 *                             W         the centre with every first-person draw skipped: the world alone (+ depth)
 *                           The game clock is held for the group (as LB_VR180 does: 1 us per frame), so animation,
 *                           effects and eye adaptation should not move between the six frames.
 *   LB_STEREO_HOLD=1        hold the game clock over plain triplets too.
 *
 *   LB_STEREO_ALTERNATE=1   continuous mode for measuring and judging, instead of LB_STEREO_FRAMES (which is then
 *                           ignored): frames render from the left eye when their number is even and from the right
 *                           eye when odd, all the time, with no screenshots. While a menu is open the shift is 0.
 *                           skyrim_frame_eye() reports the eye and shift of the frame being presented. LB_SHOT=F,...
 *                           then writes shot-<base>-<L|R>.bmp, depth-<base>-<L|R>.f32 and stereo-<base>-<L|R>.txt
 *                           per listed frame, base being the even frame of the pair (LB_SHOT=F,F+1 with F even
 *                           gives the L/R pair). tools/stereo_compare.py --shots --base F also needs the centre
 *                           frame: LB_STEREO_CENTRE=F+2 (comma-separated list) renders that frame with no shift as
 *                           eye C (base = frame - 2), so LB_SHOT=F,F+1,F+2 gives a complete triplet.
 *
 * The left eye sits at view-space x = -d. Seen from there a point at view x is at x + d, so its clip x grows by
 * P00*d: it moves right on screen. DXVK applies pos.x -= k, so k = P00 * e with e = -d for the left eye (k < 0,
 * clip x grows) and e = +d for the right eye. P00 = 2 / (tan_right - tan_left) from the world camera's frustum
 * tangents (equal to 1 / tan_right for the game's symmetric frustum). The first-person camera has the same field
 * of view (65, docs/perf/LOG.md 2026-11-14), so the same k is right for its draws.
 *
 * The shift for frame N+1 is set right after frame N's Present returns, so the whole of frame N+1, from its
 * first draw, renders from one eye; on first-person-only frames it is switched on and off as the first-person flag
 * changes (stereo_fp_changed), between draws on the thread making the device calls, which DXVK picks up at the next
 * draw. Without LB_STEREO_EYE_UNITS nothing changes, in DXVK either; with it but no LB_STEREO_FRAMES, the shaders
 * carry the shift with k = 0. */
extern "C" void dxvk_stereo_set_clip_shift(float k);  // DXVK (dxvk_stereo.h)
extern "C" void dxvk_stereo_set_view_clip_matrices(const float view0[16], const float view1[16]);

/* Per-eye clip matrices for live single-pass stereo (stage 1 of rendering into the headset's own eye views).
 * Each eye renders the game's scene through its own camera: M = P_eye * V_eye_from_game * inverse(P_game), in
 * column-vector form on D3D clip space (x right, y up, z 0..w, w = view z), applied by DXVK to every perspective
 * vertex of the main passes. View space is the game camera's (x right, y up, z forward, game units).
 *   - P_game: from the game camera's frustum tangents (l r t b at unit distance), near and far.
 *   - P_eye: from the eye's own tangents with the game's near and far (the eye's view of the same scene).
 *   - V_eye_from_game: the eye's pose in the game camera's view space, inverted.
 * skyrim_stereo_set_eyes gives the eyes (from the headset compositor); LB_STEREO_MATRIX=1 on the Mac uses the
 * game's own frustum and +-LB_STEREO_EYE_UNITS sideways, which must equal the plain offset path exactly. */
struct StereoEyes {
    bool set = false;
    float tangents[2][4];     // l r t b per eye, at unit distance
    float eye_pose[2][16];  // column-major, metres, visionOS head space (x right, y up, z back)
    float units_per_metre = 70.0f;
};
static StereoEyes g_stereo_eyes;
static std::mutex g_stereo_eyes_lock;
/* The eyes the frame being drawn was rendered with (set with its matrices after the previous Present), and those of
 * the frame being presented, which the immersive compositor reads through skyrim_frame_eyes from inside Present. */
static StereoEyes g_eyes_drawing, g_eyes_presented;
static std::atomic<bool> g_eyes_given{false};  // g_stereo_eyes.set, for readers outside the lock
/* The eye frusta (l r t b) and eye poses (column-major, metres, head space) this Present's frame was rendered with;
 * 0 when it was not rendered into eye views. Called from the present tap, inside Present. */
extern "C" int skyrim_frame_eyes(float tangents[8], float eye_pose[32]) {
    std::lock_guard<std::mutex> hold(g_stereo_eyes_lock);
    if (!g_eyes_presented.set) return 0;
    memcpy(tangents, g_eyes_presented.tangents, sizeof g_eyes_presented.tangents);
    memcpy(eye_pose, g_eyes_presented.eye_pose, sizeof g_eyes_presented.eye_pose);
    return 1;
}
extern "C" void skyrim_stereo_set_eyes(const float tangents[8], const float eye_pose[32], float units_per_metre) {
    std::lock_guard<std::mutex> hold(g_stereo_eyes_lock);
    memcpy(g_stereo_eyes.tangents, tangents, sizeof g_stereo_eyes.tangents);
    memcpy(g_stereo_eyes.eye_pose, eye_pose, sizeof g_stereo_eyes.eye_pose);
    if (units_per_metre > 0) g_stereo_eyes.units_per_metre = units_per_metre;
    g_stereo_eyes.set = true;
    g_eyes_given = true;
}
/* The compositor stops giving eyes (tracking lost, the immersive space closed): back to the game's own frustum. */
extern "C" void skyrim_stereo_clear_eyes(void) {
    std::lock_guard<std::mutex> hold(g_stereo_eyes_lock);
    g_stereo_eyes.set = false;
    g_eyes_given = false;
}

// Column-major 4x4 helpers (m[c * 4 + r]).
static void mat_mul(float out[16], const float a[16], const float b[16]) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = v;
        }
    memcpy(out, t, sizeof t);
}
// D3D perspective projection, column-vector form: x_c = (2x - (r+l)z)/(r-l), y_c = (2y - (t+b)z)/(t-b),
// z_c = f/(f-n) z - n f/(f-n), w_c = z.
static void proj_d3d(float m[16], float l, float r, float t, float b, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l);
    m[5] = 2.0f / (t - b);
    m[8] = -(r + l) / (r - l);
    m[9] = -(t + b) / (t - b);
    m[10] = f / (f - n);
    m[11] = 1.0f;
    m[14] = -n * f / (f - n);
}
// Its inverse (exact for the form above).
static void proj_d3d_inverse(float m[16], float l, float r, float t, float b, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    const float a = f / (f - n), c = -n * f / (f - n);
    m[0] = (r - l) / 2.0f;
    m[5] = (t - b) / 2.0f;
    m[12] = (r + l) / 2.0f;   // x = (x_c (r-l)/2 + w (r+l)/2) ...
    m[13] = (t + b) / 2.0f;
    m[14] = 1.0f;             // z = w_c
    m[11] = 1.0f / c;         // w = (z_c - a w_c) / c
    m[15] = -a / c;
}
/* The two eyes' clip matrices for the game camera `cam`. Returns false without eyes. */
static bool stereo_eye_matrices(const GameCamera &cam, float d_units, const StereoEyes &eyes, float out[2][16],
                                float near_override = 0, float far_scale = 1) {
    const float l = cam.frustum[0], r = cam.frustum[1], t = cam.frustum[2], b = cam.frustum[3];
    const float n = near_override > 0 ? near_override : cam.frustum[4], f = cam.frustum[5];
    if (!(r > l) || !(t > b) || !(n > 0) || !(f > n)) return false;
    float inv_game[16];
    proj_d3d_inverse(inv_game, l, r, t, b, n, f);
    for (int e = 0; e < 2; e++) {
        float p_eye[16], view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (eyes.set) {
            const float *tg = eyes.tangents[e];
            proj_d3d(p_eye, tg[0], tg[1], tg[2], tg[3], n, f * far_scale);
            // Eye pose in head space (visionOS: z back) -> game view space (z forward), in game units: flip z.
            const float *h = eyes.eye_pose[e];
            float pose[16];
            for (int c = 0; c < 4; c++)
                for (int rr = 0; rr < 4; rr++) {
                    float v = h[c * 4 + rr];
                    if ((rr == 2) != (c == 2)) v = -v;  // conjugate by diag(1,1,-1,1)
                    pose[c * 4 + rr] = v;
                }
            pose[12] *= eyes.units_per_metre; pose[13] *= eyes.units_per_metre; pose[14] *= eyes.units_per_metre;
            // view = inverse(pose) for a rigid pose: R^T and -R^T p
            for (int c = 0; c < 3; c++)
                for (int rr = 0; rr < 3; rr++) view[c * 4 + rr] = pose[rr * 4 + c];
            for (int rr = 0; rr < 3; rr++)
                view[12 + rr] = -(view[0 * 4 + rr] * pose[12] + view[1 * 4 + rr] * pose[13] + view[2 * 4 + rr] * pose[14]);
        } else {
            proj_d3d(p_eye, l, r, t, b, n, f);
            view[12] = e == 0 ? d_units : -d_units;  // left eye at x = -d: view x' = x + d
        }
        float m[16];
        mat_mul(m, view, inv_game);
        mat_mul(out[e], p_eye, m);
    }
    return true;
}
/* The live matrices for the next frame: the world's, and the first-person draws' (their projection has its own
 * near plane, 5 against the world's 15: with the world's inverse their eye offset would come out a third of what it
 * should be). stereo_fp_changed switches between them as the first-person draws start and end. */
static float g_live_m_world[2][16], g_live_m_fp[2][16];
static bool g_live_m_valid, g_live_m_fp_valid;
static bool stereo_matrix_mode(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LB_STEREO_MATRIX"); on = e && !strcmp(e, "1"); }
    return on == 1 || g_eyes_given;
}
extern "C" void dxvk_stereo_set_clip_offset(float x, float y, float z, float w);
extern "C" int dxvk_stereo_enabled(void);
extern "C" volatile int g_fake_clock_on;  // kernel32.c (rt_game_ns)
extern "C" volatile uint64_t g_fake_clock_ns;
extern "C" uint64_t rt_game_ns(void);

struct StereoSlot {
    const char *tag;  // file name part after the base frame
    char eye;         // 'L', 'R' or 'C'
    bool fp_only;     // the shift on first-person draws only
    bool skip_fp;     // no first-person draws at all
    bool depth;       // also write the scene's depth target
};
static const StereoSlot k_triplet[] = {
    {"L", 'L', false, false, false}, {"R", 'R', false, false, false}, {"C", 'C', false, false, true}};
static const StereoSlot k_triplet_fp[] = {
    {"L", 'L', true, false, true}, {"R", 'R', true, false, true}, {"C", 'C', false, false, true}};
static const StereoSlot k_hybrid[] = {
    {"L", 'L', false, false, false}, {"R", 'R', false, false, false}, {"C", 'C', false, false, true},
    {"FL", 'L', true, false, true},  {"FR", 'R', true, false, true},  {"W", 'C', false, true, true}};

struct StereoState {
    bool init = false, on = false, alternate = false;
    float d = 0;
    std::vector<int> bases;
    const StereoSlot *slots = k_triplet;
    int group = 3;              // frames per group
    bool hold = false;          // hold the game clock over each group
    uint64_t hold_t0 = 0;       // the held clock's start
    bool holding = false;
    float k_applied = 0;        // the shift the frame being presented was drawn with (first-person draws only on
    bool fp_only = false;       //   fp_only frames)
    bool skip_fp = false;       // the frame being drawn skips its first-person draws
    std::vector<int> centres;   // alternate mode: frames rendered unshifted (LB_STEREO_CENTRE)
    int eye_applied = 0;        // -1/0/+1: the eye the frame being presented was drawn from
    GameCamera cam_applied{};   // the camera k_applied was computed from
    bool cam_applied_valid = false;
};
static StereoState g_stereo;
static bool stereo_live_multiview(void);

static void stereo_init(void) {
    StereoState &st = g_stereo;
    st.init = true;
    const char *d = getenv("LB_STEREO_EYE_UNITS"), *f = getenv("LB_STEREO_FRAMES");
    const char *alt = getenv("LB_STEREO_ALTERNATE"), *cen = getenv("LB_STEREO_CENTRE");
    st.alternate = alt && atoi(alt) != 0;
    if (!d || atof(d) == 0) return;
    if (st.alternate) {
        st.d = (float)fabs(atof(d));
        for (const char *e = cen; e && *e;) {
            int a = 0;
            if (sscanf(e, "%d", &a) == 1 && st.centres.size() < 256) st.centres.push_back(a);
            e = strchr(e, ',');
            if (e) e++;
        }
        st.on = true;
        LOG("stereo alternate: left eye on even frames, right on odd, eye separation %.3f units (half %.3f each "
            "side), %zu centre frames, DXVK shader shift %s", 2 * st.d, st.d, st.centres.size(),
            dxvk_stereo_enabled() ? "on" : "OFF (LB_STEREO_EYE_UNITS must be set at launch)");
        return;
    }
    if (!d || atof(d) == 0) return;
    if (!f || !*f) {
        // No capture frames: live single-pass stereo (LB_STEREO_MULTIVIEW=1) shifts every frame's eyes.
        if (stereo_live_multiview()) {
            st.d = (float)fabs(atof(d));
            st.on = true;
            LOG("stereo: live single-pass stereo, d %.3f units", st.d);
        }
        return;
    }
    st.d = (float)fabs(atof(d));
    const char *passes = getenv("LB_STEREO_PASSES"), *hybrid = getenv("LB_STEREO_HYBRID"),
               *hold = getenv("LB_STEREO_HOLD");
    if (hybrid && atoi(hybrid)) {
        st.slots = k_hybrid;
        st.group = 6;
        st.hold = true;
    } else if (passes && !strcmp(passes, "firstperson")) {
        st.slots = k_triplet_fp;
    }
    if (hold && atoi(hold)) st.hold = true;
    for (const char *e = f; e && *e;) {
        int a = 0, b = 0, step = st.group, n = 0;
        if (sscanf(e, "%d-%d:%d%n", &a, &b, &step, &n) == 3 || sscanf(e, "%d-%d%n", &a, &b, &n) == 2) {
            if (step < st.group) step = st.group;  // groups must not overlap
            for (int x = a; x <= b && st.bases.size() < 256; x += step) st.bases.push_back(x);
        } else if (sscanf(e, "%d", &a) == 1) {
            st.bases.push_back(a);
        }
        e = strchr(e, ',');
        if (e) e++;
    }
    st.on = !st.bases.empty();
    LOG("stereo capture: d %.3f units, %zu groups of %d (%s%s), DXVK shader shift %s", st.d, st.bases.size(),
        st.group, st.slots == k_hybrid ? "hybrid: L R C FL FR W" : st.slots == k_triplet_fp ? "first person only"
                                                                                            : "L R C",
        st.hold ? ", game clock held" : "",
        dxvk_stereo_enabled() ? "on" : "OFF (LB_STEREO_EYE_UNITS must be set at launch)");
}

// Alternate mode (LB_STEREO_ALTERNATE): every frame is one of these, each written with depth when LB_SHOT lists it.
static const StereoSlot k_alt_slots[] = {
    {"L", 'L', false, false, true}, {"R", 'R', false, false, true}, {"C", 'C', false, false, true}};

// The slot frame renders in a stereo group, with the group's base frame and the slot's index; null if none.
static const StereoSlot *stereo_slot(int frame, int *base, int *index = nullptr) {
    if (g_stereo.alternate) {
        if (index) *index = 0;
        for (int c : g_stereo.centres)
            if (frame == c) { *base = c - 2; return &k_alt_slots[2]; }
        if (frame & 1) { *base = frame - 1; return &k_alt_slots[1]; }
        *base = frame;
        return &k_alt_slots[0];
    }
    for (int b : g_stereo.bases) {
        if (frame >= b && frame < b + g_stereo.group) {
            *base = b;
            if (index) *index = frame - b;
            return &g_stereo.slots[frame - b];
        }
    }
    return nullptr;
}

static void stereo_write_info(const char *tag, const StereoSlot &slot, int base) {
    char path[1100];
    snprintf(path, sizeof path, "%s/stereo-%s.txt", g_save_dir, tag);
    FILE *out = fopen(path, "w");
    if (!out) return;
    const StereoState &st = g_stereo;
    const char eye = slot.eye;
    const float *f = st.cam_applied.frustum;
    const float p00 = st.cam_applied_valid && f[1] != f[0] ? 2.0f / (f[1] - f[0]) : 0.0f;
    fprintf(out, "frame %d\nbase %d\neye %c\nslot %s\neye_units %.6f\neye_x %.6f\nclip_shift_k %.9g\np00 %.9g\n",
            g_frame, base, eye, slot.tag, st.d, eye == 'L' ? -st.d : eye == 'R' ? st.d : 0.0f, st.k_applied, p00);
    fprintf(out, "passes %s\nfirst_person_only %d\nfirst_person_skipped %d\nfirst_person_near %.9g\nclock_held %d\n"
            "menu_open %d\n", slot.fp_only ? "firstperson" : getenv("LB_STEREO_PASSES") ? getenv("LB_STEREO_PASSES")
                                                                                         : "screen",
            slot.fp_only ? 1 : 0, slot.skip_fp ? 1 : 0, g_fp_near, st.holding ? 1 : 0, menu_open() ? 1 : 0);
    // The camera the shift came from (read at the previous Present) and the camera at this Present.
    auto put_cam = [&](const char *name, const GameCamera &c, bool valid) {
        if (!valid) { fprintf(out, "%s none\n", name); return; }
        fprintf(out, "%s_frustum_lrtb_near_far %.9g %.9g %.9g %.9g %.9g %.9g\n", name, c.frustum[0], c.frustum[1],
                c.frustum[2], c.frustum[3], c.frustum[4], c.frustum[5]);
        fprintf(out, "%s_rot", name);
        for (float v : c.rot) fprintf(out, " %.9g", v);
        fprintf(out, "\n%s_pos %.9g %.9g %.9g\n", name, c.pos[0], c.pos[1], c.pos[2]);
    };
    put_cam("cam_used", st.cam_applied, st.cam_applied_valid);
    GameCamera now;
    const bool now_valid = game_camera_read(&now);
    put_cam("cam_present", now, now_valid);
    fclose(out);
}

// Before Present: capture the frame if it belongs to a group.
static void stereo_before_present(IDirect3DDevice9 *d) {
    if (!g_stereo.init) stereo_init();
    if (!g_stereo.on) return;
    int base = 0;
    const StereoSlot *slot = stereo_slot(g_frame, &base);
    if (!slot) return;
    if (g_stereo.alternate && !shot_frame_wanted(g_frame)) return;
    char tag[32];
    snprintf(tag, sizeof tag, "%05d-%s", base, slot->tag);
    shot_write(d, tag, slot->depth);
    stereo_write_info(tag, *slot, base);
    LOG("stereo capture: frame %d slot %s k %.6g%s -> shot-%s.bmp", g_frame, slot->tag, g_stereo.k_applied,
        slot->fp_only ? " (first person only)" : slot->skip_fp ? " (no first person)" : "", tag);
}

// The first-person flag changed (fp_update): on first-person-only frames the shift follows it.
static void stereo_fp_changed(int kind) {
    const StereoState &st = g_stereo;
    if (st.on && st.fp_only) dxvk_stereo_set_clip_shift(kind != FP_NONE ? st.k_applied : 0.0f);
    // Live per-eye matrices: first-person draws through their own projection's inverse.
    if (g_live_m_valid) {
        const bool fp = kind != FP_NONE && g_live_m_fp_valid;
        dxvk_stereo_set_view_clip_matrices(fp ? g_live_m_fp[0] : g_live_m_world[0], fp ? g_live_m_fp[1] : g_live_m_world[1]);
    }
}
static bool stereo_skip_fp(void) { return g_stereo.skip_fp; }

static bool stereo_live_multiview(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LB_STEREO_MULTIVIEW"); on = e && !strcmp(e, "1"); }
    return on == 1;
}

// After Present: set the eye, and the clock, for the next frame.
/* Eye views, stage 2 of plans/eye-views.md (LB_EYE_LATE=1): the game camera trails the head (head steering is a
 * closed loop on the camera the game last presented, about 10 degrees behind a 27 deg/s sweep on the Mac), and
 * each frame is rendered where its camera points. At the frame's first main-scene draw, when this frame's camera is
 * known, the eyes are turned from the camera toward where the head points now (skyrim_head_now), by at most
 * LB_EYE_LATE_MAX degrees each in heading and pitch (default 2: the compositor's default margin, so the eyes stay
 * inside what the game culls for), and the matrices and the eyes this frame reports are redone with them. The
 * compositor places each eye's picture by the reported pose, so the picture comes out where the head looks and
 * only the remaining difference is corrected there. g_late_done: once per frame. */
static bool g_late_done;
extern "C" int skyrim_head_now(float *heading, float *pitch);  // platform_sdl.c
static void stereo_late_eyes(void) {
    if (g_late_done) return;
    g_late_done = true;
    static float max_rad = -1;
    if (max_rad < 0) {
        const char *e = getenv("LB_EYE_LATE");
        const char *m = getenv("LB_EYE_LATE_MAX");
        max_rad = e && !strcmp(e, "1") ? (m ? (float)atof(m) : 2.0f) * 0.017453293f : 0.0f;
        if (max_rad > 0) LOG("eye views: late eyes, up to %.1f degrees toward the head", max_rad * 57.29578f);
    }
    if (max_rad <= 0 || !g_live_m_valid) return;
    StereoEyes eyes;
    { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); eyes = g_eyes_drawing; }
    float heading, pitch;
    GameCamera cam;
    if (!eyes.set || menu_open() || !game_camera_read(&cam) || !skyrim_head_now(&heading, &pitch)) return;
    // Forward is the rotation's first column; heading clockwise from north (+y), as head steering measures it.
    const float game_yaw = atan2f(cam.rot[0], cam.rot[3]), game_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, cam.rot[6])));
    if (fabsf(game_pitch) > 1.2f) return;  // nearly straight up or down: the heading is meaningless
    float dyaw = heading - game_yaw;
    dyaw -= 6.2831853f * floorf((dyaw + 3.1415927f) / 6.2831853f);
    const float dyaw_c = fmaxf(-max_rad, fminf(max_rad, dyaw));
    const float dp_c = fmaxf(-max_rad, fminf(max_rad, pitch - game_pitch));
    // In the camera's frame (x right, y up, z back, as the eye poses): R = Rx(-pitch_cam) Ry(-dyaw) Rx(pitch_cam + dp).
    auto rx = [](float a, float m[9]) { float c = cosf(a), s = sinf(a); float r[9] = {1, 0, 0, 0, c, s, 0, -s, c}; memcpy(m, r, sizeof r); };
    auto ry = [](float a, float m[9]) { float c = cosf(a), s = sinf(a); float r[9] = {c, 0, -s, 0, 1, 0, s, 0, c}; memcpy(m, r, sizeof r); };
    auto mul3 = [](const float a[9], const float b[9], float o[9]) {  // column-major 3x3
        float t[9];
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) t[c * 3 + r] = a[0 * 3 + r] * b[c * 3 + 0] + a[1 * 3 + r] * b[c * 3 + 1] + a[2 * 3 + r] * b[c * 3 + 2];
        memcpy(o, t, sizeof t);
    };
    float a[9], b[9], c[9], rot[9];
    rx(-game_pitch, a); ry(-dyaw_c, b); rx(game_pitch + dp_c, c);
    mul3(a, b, rot); mul3(rot, c, rot);
    for (int e = 0; e < 2; e++) {
        float *p = eyes.eye_pose[e], q[16];
        for (int col = 0; col < 4; col++) {  // R applied to the pose's rotation columns and its position
            for (int r = 0; r < 3; r++)
                q[col * 4 + r] = rot[0 * 3 + r] * p[col * 4 + 0] + rot[1 * 3 + r] * p[col * 4 + 1] + rot[2 * 3 + r] * p[col * 4 + 2];
            q[col * 4 + 3] = p[col * 4 + 3];
        }
        memcpy(p, q, sizeof q);
    }
    // Turning the eyes takes points on the game's far plane (the sky) past it: one at angle a off the camera's axis,
    // turned by d toward the eye's, ends at depth f cos(a - d) / cos a = f (cos d + tan a sin d), largest at the
    // frustum's corner. The eyes' far plane is that much farther (1% more), or the sky is clipped there.
    const float corner = hypotf(fmaxf(-cam.frustum[0], cam.frustum[1]), fmaxf(cam.frustum[2], -cam.frustum[3]));
    const float turn = hypotf(dyaw_c, dp_c);
    const float far_scale = (cosf(turn) + corner * sinf(turn)) * 1.01f;
    float m[2][16];
    if (!stereo_eye_matrices(cam, g_stereo.d, eyes, m, 0, far_scale)) return;
    memcpy(g_live_m_world, m, sizeof m);
    g_live_m_fp_valid = g_fp_near > 0 && stereo_eye_matrices(cam, g_stereo.d, eyes, g_live_m_fp, g_fp_near, far_scale);
    const bool fp = fp_kind() != FP_NONE && g_live_m_fp_valid;
    dxvk_stereo_set_view_clip_matrices(fp ? g_live_m_fp[0] : m[0], fp ? g_live_m_fp[1] : m[1]);
    { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); g_eyes_drawing = eyes; }
    static int logged;
    if (++logged % 300 == 1)
        LOG("eye views: late eyes: head %.2f/%.2f deg from the camera, applied %.2f/%.2f", dyaw * 57.29578f,
            (pitch - game_pitch) * 57.29578f, dyaw_c * 57.29578f, dp_c * 57.29578f);
}

/* LB_STEREO_TEST_EYES (Mac, no headset): feed skyrim_stereo_set_eyes as the compositor would. "game": the game's own
 * frustum for both eyes and +-LB_STEREO_EYE_UNITS sideways at 70 units per metre (must render exactly what the offset
 * path does); "l,r,t,b,separation_m": the left eye's tangents, the right eye's mirrored (-r, -l, t, b), the eyes
 * separation_m apart. */
static void stereo_test_eyes(const GameCamera &cam, float d_units) {
    static int mode = -1;
    static float v[5];
    if (mode < 0) {
        const char *e = getenv("LB_STEREO_TEST_EYES");
        mode = !e ? 0 : !strcmp(e, "game") ? 1 : sscanf(e, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5 ? 2 : 0;
    }
    if (!mode || g_eyes_given) return;
    float t[2][4], pose[2][16] = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    float half_m = mode == 1 ? d_units / 70.0f : v[4] / 2;
    const float *f = mode == 1 ? cam.frustum : v;
    t[0][0] = f[0]; t[0][1] = f[1]; t[0][2] = f[2]; t[0][3] = f[3];
    t[1][0] = mode == 1 ? f[0] : -f[1]; t[1][1] = mode == 1 ? f[1] : -f[0]; t[1][2] = f[2]; t[1][3] = f[3];
    pose[0][12] = -half_m;
    pose[1][12] = half_m;
    skyrim_stereo_set_eyes(&t[0][0], &pose[0][0], 70.0f);
    LOG("stereo test eyes: L %.3f %.3f %.3f %.3f R %.3f %.3f %.3f %.3f, eyes %.4f m apart", t[0][0], t[0][1], t[0][2],
        t[0][3], t[1][0], t[1][1], t[1][2], t[1][3], 2 * half_m);
}
static void stereo_after_present(void) {
    StereoState &st = g_stereo;
    if (!st.on) return;
    { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); g_eyes_drawing = StereoEyes(); }
    g_late_done = false;
    bool matrices_set = false;
    int base = 0, index = 0;
    const StereoSlot *slot = stereo_slot(g_frame + 1, &base, &index);
    const char eye = slot ? slot->eye : 0;
    float k = 0;
    int ev = 0;
    GameCamera cam;
    const bool menu = st.alternate && menu_open();  // alternate mode: no shift under a menu
    const bool valid = !menu && (eye == 'L' || eye == 'R') && game_camera_read(&cam);
    if (valid) {
        const float l = cam.frustum[0], r = cam.frustum[1];
        if (r > l) { k = 2.0f / (r - l) * (eye == 'L' ? -st.d : st.d); ev = eye == 'L' ? -1 : 1; }
        st.cam_applied = cam;
    } else if (!st.alternate && (eye == 'L' || eye == 'R')) {
        LOG("stereo capture: frame %d eye %c: no world camera, rendering the centre eye", g_frame + 1, eye);
    } else if (!eye && stereo_live_multiview() && game_camera_read(&cam)) {
        // Live single-pass stereo (LB_STEREO_MULTIVIEW=1, no capture frame): view 0 (layer 0) the left eye, view 1
        // mirrored to the right (dxvk_stereo.h), every frame.
        const float l = cam.frustum[0], r = cam.frustum[1];
        float m[2][16];
        static float hud_units = -1;
        if (hud_units < 0) {
            const char *h = getenv("LB_STEREO_HUD_DEPTH");
            hud_units = (h && atof(h) > 0 ? (float)atof(h) : 2.0f) * 70.0f;  // metres at 70 units per metre
        }
        stereo_test_eyes(cam, st.d);
        StereoEyes eyes;
        { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); eyes = g_stereo_eyes; }
        // Menus are shown on the fixed screen with the plain offset (Present reports no camera for them), so
        // they are rendered that way too.
        const bool use_matrices = stereo_matrix_mode() && !menu_open() && stereo_eye_matrices(cam, st.d, eyes, m);
        memset(g_ui_k, 0, sizeof g_ui_k);
        if (use_matrices && eyes.set) {
            for (int e = 0; e < 2; e++) {
                // Interface vertices skip the matrices, so each eye's own frustum centre is undone first: clip c
                // then points at c times the half-extent from the eye's axis in both eyes. The eye x metres to the
                // side (in the game camera's frame) sees a point D metres away x/D to its left: clip x drops by
                // P00 x / D. DXVK subtracts the offset. (Units per metre cancel.)
                const float *tg = eyes.tangents[e];
                if (!(tg[1] > tg[0]) || !(tg[2] > tg[3])) continue;
                g_ui_k[e][0] = (tg[1] + tg[0]) / (tg[1] - tg[0]) + 2.0f / (tg[1] - tg[0]) * eyes.eye_pose[e][12] / (hud_units / 70.0f);
                g_ui_k[e][1] = (tg[2] + tg[3]) / (tg[2] - tg[3]);
            }
        } else if (r > l) {
            g_ui_k[0][0] = 2.0f / (r - l) * -st.d / hud_units;  // a point hud_units away: clip offset k / w, w = depth
            g_ui_k[1][0] = -g_ui_k[0][0];
        }
        if (use_matrices) {
            { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); g_eyes_drawing = eyes; }
            memcpy(g_live_m_world, m, sizeof m);
            g_live_m_valid = true;
            g_live_m_fp_valid = g_fp_near > 0 && stereo_eye_matrices(cam, st.d, eyes, g_live_m_fp, g_fp_near);
            dxvk_stereo_set_view_clip_matrices(m[0], m[1]);  // the eyes' cameras; no offset on top
            matrices_set = true;
        } else if (r > l) {
            k = 2.0f / (r - l) * -st.d;
        }
    }
    if (!matrices_set && g_live_m_valid) {
        // Back to the plain offset: the matrices stay in DXVK until replaced, so reset them.
        static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        dxvk_stereo_set_view_clip_matrices(identity, identity);
        g_live_m_valid = g_live_m_fp_valid = false;
    }
    if (eye) st.cam_applied_valid = valid || (eye == 'C' && game_camera_read(&st.cam_applied));
    st.k_applied = k;
    st.fp_only = slot && slot->fp_only;
    st.skip_fp = slot && slot->skip_fp;
    // The first-person flag is off between frames (passes_present), so a first-person-only frame starts unshifted.
    dxvk_stereo_set_clip_shift(st.fp_only && fp_kind() == FP_NONE ? 0.0f : k);
    if (st.hold) {
        if (slot && (index == 0 || !st.holding)) {
            // Each group holds its own moment: real time at its start, read from the clock rt_game_ns gives when not
            // held (kernel32.c now_ns), since rt_game_ns is the held clock between back-to-back groups (an A-B range
            // steps by the group's length by default). Those jump forward to the next group's moment, as the clock
            // does when let go between groups, rather than going back to the first group's.
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            st.hold_t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            g_fake_clock_ns = st.hold_t0;  // before the switch, so no thread reads a stale held value (or 0)
            st.holding = true;
            g_fake_clock_on = 1;
        } else if (!slot && st.holding) {
            st.holding = false;
            g_fake_clock_on = 0;
        }
        if (st.holding) g_fake_clock_ns = st.hold_t0 + (uint64_t)index * 1000ull;
    }
    st.eye_applied = ev;
}
/* LB_VR180=start:count[:tan] (offline capture, Mac): a stereo 180-degree video, rendered by the game itself.
 *
 * From frame `start`, every block of 16 frames is one video frame (count blocks). Frame 0 of a block advances the
 * game clock by 1/24 s; frames 1..15 render the same moment from rig L, R, C (eyes at x = -e, +e, 0 along the
 * camera's right axis, e = LB_VR180_EYE units, default 2.24 = 32 mm) and faces 0..4 (front, left, right, up, down;
 * square frustum +-tan, default 1.1). Each face frame writes <save>/shot-B-RK.bmp, stereo-B-RK.txt and, for rig C,
 * depth-B-CK.f32 (B = the block's first frame). tools: scratchpad vr180/stitch, vr180/reproject.
 *
 * How: the game recomputes the world camera's rotation and position during its update every frame, then culls and
 * renders. The override is written at BeginScene, after the update and before culling, so the game culls and
 * renders the turned view natively (shadows, LOD, effects), and the first-person arms stay where they are in the
 * world. The eye's offset is not a camera move: first-person geometry is drawn as if the camera stood at the
 * game's own camera position, so a moved camera gives the arms, weapon and spell hand no parallax. Instead DXVK
 * adds the offset to every perspective vertex in clip space (dxvk_stereo_set_clip_offset; it needs
 * LB_STEREO_EYE_UNITS set at launch, and LB_STEREO_PASSES=size so square shadow maps of another size are left
 * alone: keep the face size different from iShadowMapResolution). The game clock (rt_game_ns, kernel32.c) is scripted from `start` on: it moves 1 us per frame within a
 * block, so animation, effects and eye adaptation hold still across the 15 renders.
 *
 * LB_VR180_PENDING=n (default 30): before a block starts, wait while more than n shot-*.bmp files are in the save
 * folder, so a stitcher deleting its inputs keeps disk use bounded. */
struct Vr180 {
    bool init = false, on = false;
    int start = 0, count = 0, pending = 30;
    float tan = 1.1f, e = 2.24f;
    uint64_t t0 = 0;
    float frustum0[6];
    bool have_frustum0 = false;
    int rig_frame = -1;
    GameCamera rig{};
    bool rig_valid = false;
};
static Vr180 g_vr;
static void vr180_init(void) {
    g_vr.init = true;
    const char *e = getenv("LB_VR180");
    if (!e || sscanf(e, "%d:%d:%f", &g_vr.start, &g_vr.count, &g_vr.tan) < 2 || g_vr.count <= 0) return;
    if (const char *x = getenv("LB_VR180_EYE")) g_vr.e = (float)atof(x);
    if (const char *x = getenv("LB_VR180_PENDING")) g_vr.pending = atoi(x);
    g_vr.on = true;
    LOG("vr180: %d blocks from frame %d, tan %.3f, eye %.3f units", g_vr.count, g_vr.start, g_vr.tan, g_vr.e);
}
// Frame f's place: -1 outside, else block index; *slot = 0 (clock frame) or 1..15 (rig*5 + face + 1).
static int vr180_slot(int f, int *slot) {
    if (!g_vr.on || f < g_vr.start || f >= g_vr.start + 16 * g_vr.count) return -1;
    *slot = (f - g_vr.start) % 16;
    return (f - g_vr.start) / 16;
}
static const char kRig[3] = {'L', 'R', 'C'};
// The face basis in rig coordinates (x right, y up, z forward): forward, up, right.
static const float kFace[5][3][3] = {
    {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}},    // front
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   // left
    {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}},   // right
    {{0, 1, 0}, {0, 0, -1}, {1, 0, 0}},   // up
    {{0, -1, 0}, {0, 0, 1}, {1, 0, 0}},   // down
};
// At BeginScene of the frame being rendered (g_frame + 1): turn the camera for a face frame.
extern "C++" void vr180_scene_hook(void) {
    if (!g_vr.init) vr180_init();
    if (!g_vr.on) return;
    const int f = g_frame + 1;
    int slot = 0;
    const int block = vr180_slot(f, &slot);
    uint32_t root = M32(0x1B2E8BC), w = root >= 0x10000 ? M32(root + 0xB8) : 0;
    if (w < 0x10000) return;
    if (!g_vr.have_frustum0) { memcpy(g_vr.frustum0, MEM + w + 0xE8, sizeof g_vr.frustum0); g_vr.have_frustum0 = true; }
    if (block < 0 || slot == 0) {
        memcpy(MEM + w + 0xE8, g_vr.frustum0, 4 * 4);  // l r t b back to the game's
        return;
    }
    if (g_vr.rig_frame != f) {  // the game's own camera of this frame, before any override (BeginScene may repeat)
        g_vr.rig_valid = game_camera_read(&g_vr.rig);
        g_vr.rig_frame = f;
    }
    if (!g_vr.rig_valid) return;
    const int rig = (slot - 1) / 5, face = (slot - 1) % 5;
    // Gamebryo camera: world rotation columns are forward, up, right (world coordinates).
    const float *R = g_vr.rig.rot;
    const float F[3] = {R[0], R[3], R[6]}, U[3] = {R[1], R[4], R[7]}, X[3] = {R[2], R[5], R[8]};
    auto world = [&](const float v[3], float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = v[0] * X[i] + v[1] * U[i] + v[2] * F[i];
    };
    float fw[3], up[3], rt[3];
    world(kFace[face][0], fw); world(kFace[face][1], up); world(kFace[face][2], rt);
    const float rot[9] = {fw[0], up[0], rt[0], fw[1], up[1], rt[1], fw[2], up[2], rt[2]};
    const float fr[4] = {-g_vr.tan, g_vr.tan, g_vr.tan, -g_vr.tan};
    memcpy(MEM + w + 0x54, rot, sizeof rot);
    memcpy(MEM + w + 0xE8, fr, sizeof fr);
    (void)block;
}
// Before Present: write the face frame's files.
static void vr180_before_present(IDirect3DDevice9 *d) {
    int slot = 0;
    const int block = vr180_slot(g_frame, &slot);
    if (block < 0 || slot == 0) return;
    const int rig = (slot - 1) / 5, face = (slot - 1) % 5, base = g_vr.start + 16 * block;
    char tag[32];
    snprintf(tag, sizeof tag, "%05d-%c%d", base, kRig[rig], face);
    shot_write(d, tag, rig == 2);
    char path[1100];
    snprintf(path, sizeof path, "%s/stereo-%s.txt", g_save_dir, tag);
    if (FILE *out = fopen(path, "w")) {
        IDirect3DSurface9 *back = nullptr;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) { back->GetDesc(&desc); back->Release(); }
        const float *R = g_vr.rig.rot, *P = g_vr.rig.pos;
        fprintf(out, "frame %d\nblock %d\nbase %d\nrig %c\nface %d\ntan %.9g\nnear %.9g\nfar %.9g\neye_x %.9g\n"
                "width %u\nheight %u\nunits_per_metre 70\n", g_frame, block, base, kRig[rig], face, g_vr.tan,
                g_vr.rig.frustum[4], g_vr.rig.frustum[5], rig == 0 ? -g_vr.e : rig == 1 ? g_vr.e : 0.0f,
                desc.Width, desc.Height);
        fprintf(out, "rig_rot %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\nrig_pos %.9g %.9g %.9g\n", R[0], R[1],
                R[2], R[3], R[4], R[5], R[6], R[7], R[8], P[0], P[1], P[2]);
        fclose(out);
    }
}
static int vr180_pending_shots(void) {
    int n = 0;
    if (DIR *dir = opendir(g_save_dir)) {
        while (struct dirent *ent = readdir(dir))
            if (!strncmp(ent->d_name, "shot-", 5) && strstr(ent->d_name, ".bmp")) n++;
        closedir(dir);
    }
    return n;
}
// After Present: the clock for the next frame, and backpressure before a block starts.
static void vr180_after_present(void) {
    if (!g_vr.init) vr180_init();
    if (!g_vr.on) return;
    const int f = g_frame + 1;
    if (f < g_vr.start) return;
    if (!g_fake_clock_on) { g_vr.t0 = rt_game_ns(); g_fake_clock_ns = g_vr.t0; g_fake_clock_on = 1; }
    const int k = f - g_vr.start;
    if (k % 16 == 0 && k / 16 < g_vr.count && g_vr.pending > 0) {
        for (int waited = 0; vr180_pending_shots() > g_vr.pending; waited++) {
            if (waited % 60 == 0) LOG("vr180: block %d waits for the stitcher (%d shots pending)", k / 16, vr180_pending_shots());
            usleep(500000);
        }
    }
    g_fake_clock_ns = g_vr.t0 + (uint64_t)(k / 16) * 41666667ull + (uint64_t)k * 1000ull;
    {   /* The next frame's eye offset, in the face's view space: t = ex * (rig x axis), and dclip = t * P with the
         * square frustum (P00 = P11 = 1/tan, P23 = 1). For z, P22 = far / (far - near) would be exact, but the sky
         * and clouds are drawn pinned to the far plane (z = w), and that pushes them past it (they vanish from one
         * eye on the left and right faces). Taking P22 as 1 keeps z = w pinned; for everything else the error is
         * (P22 - 1) * tz, about 1e-4 in clip z. */
        int slot = 0;
        const int block = vr180_slot(f, &slot);
        float off[4] = {0, 0, 0, 0};
        if (block >= 0 && slot > 0 && g_vr.have_frustum0) {
            const int rig = (slot - 1) / 5, face = (slot - 1) % 5;
            const float ex = rig == 0 ? -g_vr.e : rig == 1 ? g_vr.e : 0.0f;
            const float tx = ex * kFace[face][2][0], ty = ex * kFace[face][1][0], tz = ex * kFace[face][0][0];
            off[0] = tx / g_vr.tan; off[1] = ty / g_vr.tan; off[2] = tz; off[3] = tz;
        }
        dxvk_stereo_set_clip_offset(off[0], off[1], off[2], off[3]);
    }
    if (k % 16 == 0 && k / 16 < g_vr.count && (k / 16) % 10 == 0) LOG("vr180: block %d of %d", k / 16, g_vr.count);
}
#else
static void stereo_before_present(IDirect3DDevice9 *) {}
static void stereo_after_present(void) {}
static void stereo_fp_changed(int) {}
static void stereo_late_eyes(void) {}
static bool stereo_skip_fp(void) { return false; }
static void vr180_before_present(IDirect3DDevice9 *) {}
static void vr180_after_present(void) {}
void vr180_scene_hook(void) {}
#endif

void B_IDirect3DDevice9_Present(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    static std::vector<std::pair<int, int>> trace_ranges;  // LB_D3DTRACE=from:to[,from:to...]
    if (g_trace_from < 0) {
        g_trace_from = 1 << 30;
        for (const char *e = getenv("LB_D3DTRACE"); e && *e;) {
            int a = 0, b = 0;
            if (sscanf(e, "%d:%d", &a, &b) == 2) trace_ranges.push_back({a, b});
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    g_frame++;
    camera_log();
    g_d3d_trace = 0;
    for (const auto &r : trace_ranges) g_d3d_trace |= g_frame >= r.first && g_frame <= r.second;
    if (g_d3d_trace) fprintf(stderr, "D3D %d Present\n", g_frame);
    extern void touch_draw(IDirect3DDevice9 *d);
    touch_draw(d);
    { extern void hud_screen_present(IDirect3DDevice9 *); hud_screen_present(d); }
    shot_before_present(d);
    fp_present();
    stereo_before_present(d);
    vr180_before_present(d);
    RECT vr = view_rect();
    const RECT *dst = ARG(2) ? (const RECT *)GPTR(ARG(2)) : (vr.left || vr.top) ? &vr : nullptr;
#if defined(__APPLE__)
    // The immersive compositor gets this frame's scene depth with its colour (DXVK present tap). The target is
    // only known to be alive within this frame, so it's named for this Present and forgotten after it.
    /* With a menu up the world camera isn't what's on screen (the journal, for one, renders through another
     * camera), so the frame carries none and the compositor shows it on its fixed screen. */
    g_frame_camera_valid = !menu_open() && game_camera_read(&g_frame_camera);
    { std::lock_guard<std::mutex> hold(g_stereo_eyes_lock); g_eyes_presented = g_frame_camera_valid ? g_eyes_drawing : StereoEyes(); }
    // Where the camera was when, so the compositor can keep the player's movement going between frames.
    motion_camera(motion_now(), g_frame_camera_valid ? g_frame_camera.pos : nullptr);
    g_frame_eye = g_stereo.on ? g_stereo.eye_applied : 0;  // set after the previous Present for this frame
    g_frame_eye_k = g_stereo.on ? g_stereo.k_applied : 0.0f;
    game_fov_apply();
    dxvk_present_tap_set_depth(menu_open() ? nullptr : d3d_scene_depth_target());
#endif
    platform_present_draws(g_d3d_draws);
    g_d3d_draws = 0;
    passes_present();
    platform_present_begin();
    HRESULT hr = d->Present((const RECT *)GPTR(ARG(1)), dst, nullptr, (const RGNDATA *)GPTR(ARG(4)));
    platform_present_end();
#if defined(__APPLE__)
    dxvk_present_tap_set_depth(nullptr);
    d3d_scene_depth_reset();
#endif
    stereo_after_present();
    vr180_after_present();
    pace_present();
    platform_present_hook();
    bisect_present();
    RET(hr, 5);
}

void B_IDirect3DDevice9_GetNPatchMode(CPU *c) {
    auto *d = (IDirect3DDevice9 *)com_host(ARG(0));
    fpush(c, d->GetNPatchMode());
    RET(0, 1);
}

void B_IDirect3DSwapChain9_Present(CPU *c) {
    auto *s = (IDirect3DSwapChain9 *)com_host(ARG(0));
    RECT vr = view_rect();
    const RECT *dst = ARG(2) ? (const RECT *)GPTR(ARG(2)) : (vr.left || vr.top) ? &vr : nullptr;
    HRESULT hr = s->Present((const RECT *)GPTR(ARG(1)), dst, nullptr, (const RGNDATA *)GPTR(ARG(4)), ARG(5));
    pace_present();
    platform_present_hook();
    RET(hr, 6);
}

void B_IDirect3DSwapChain9_GetPresentParameters(CPU *c) {
    auto *s = (IDirect3DSwapChain9 *)com_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = s->GetPresentParameters(&pp);
    uint32_t g = ARG(1);
    M32(g + 0) = pp.BackBufferWidth;
    M32(g + 4) = pp.BackBufferHeight;
    M32(g + 8) = pp.BackBufferFormat;
    M32(g + 12) = pp.BackBufferCount;
    M32(g + 16) = pp.MultiSampleType;
    M32(g + 20) = pp.MultiSampleQuality;
    M32(g + 24) = pp.SwapEffect;
    M32(g + 28) = 0x00010010u;
    M32(g + 32) = pp.Windowed;
    M32(g + 36) = pp.EnableAutoDepthStencil;
    M32(g + 40) = pp.AutoDepthStencilFormat;
    M32(g + 44) = pp.Flags;
    M32(g + 48) = pp.FullScreen_RefreshRateInHz;
    M32(g + 52) = pp.PresentationInterval;
    RET(hr, 2);
}

// Sources may be system-memory textures written through a persistent lock
// pointer; push those writes to the host first.
void B_IDirect3DDevice9_UpdateTexture(CPU *c) {
    auto *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *src = (IDirect3DBaseTexture9 *)com_host(ARG(1));
    auto *dst = (IDirect3DBaseTexture9 *)com_host(ARG(2));
    shadow_flush(src);
    RET(dev->UpdateTexture(src, dst), 3);
}

void B_IDirect3DDevice9_UpdateSurface(CPU *c) {
    auto *dev = (IDirect3DDevice9 *)com_host(ARG(0));
    auto *src = (IDirect3DSurface9 *)com_host(ARG(1));
    auto *dst = (IDirect3DSurface9 *)com_host(ARG(3));
    shadow_flush(src);
    RET(dev->UpdateSurface(src, (const RECT *)GPTR(ARG(2)), dst, (const POINT *)GPTR(ARG(4))), 5);
}

// ---------------------------------------------------------------- locks
// Guest code cannot address host memory, so every lock hands the guest a
// shadow copy in guest memory and copies it back on unlock.
namespace {
struct Shadow { uint32_t guest; void *host; size_t size; DWORD flags; bool persist; pthread_t th; };
// A resource can be locked several times at once (nested locks of buffer
// ranges, or locks from several threads): every lock keeps its own shadow,
// and an unlock ends the newest one taken by the same thread.
std::map<std::pair<void *, uint32_t>, std::vector<Shadow>> g_shadows;
std::mutex g_shadow_lock;

// System-memory textures keep one guest buffer for their whole life: games
// (and Bink's frame buffers) keep writing through the pointer after unlock,
// which is valid on Windows for D3DPOOL_SYSTEMMEM. The buffer is copied to
// the host object on unlock and again before it is used as an update source.
struct Persist { uint32_t guest; size_t size; std::function<void(const uint8_t *)> upload; };
std::map<std::pair<void *, uint32_t>, Persist> g_persist;

uint32_t shadow_begin(void *obj, uint32_t sub, void *hostbits, size_t size, DWORD flags,
                      std::function<void(const uint8_t *)> upload = nullptr) {
    uint32_t g = 0;
    bool persist = upload != nullptr;
    std::lock_guard<std::mutex> l(g_shadow_lock);
    if (persist) {
        auto it = g_persist.find({obj, sub});
        if (it != g_persist.end() && it->second.size == size) {
            g = it->second.guest; // guest copy is authoritative
        } else {
            if (it != g_persist.end()) gfree(it->second.guest);
            g = gmalloc((uint32_t)size + 16);
            if (!g) rt_fatal("out of guest memory for a %zu byte lock", size);
            memcpy(MEM + g, hostbits, size);
            g_persist[{obj, sub}] = Persist{g, size, upload};
        }
    } else {
        g = gmalloc((uint32_t)size + 16);
        if (!g) rt_fatal("out of guest memory for a %zu byte lock", size);
        if (!(flags & D3DLOCK_DISCARD)) memcpy(MEM + g, hostbits, size);
    }
    g_shadows[{obj, sub}].push_back(Shadow{g, hostbits, size, flags, persist, pthread_self()});
    return g;
}

bool shadow_end(void *obj, uint32_t sub) {
    Shadow s;
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        auto it = g_shadows.find({obj, sub});
        if (it == g_shadows.end() || it->second.empty()) return false;
        auto &v = it->second;
        size_t k = v.size() - 1;
        for (size_t j = v.size(); j-- > 0;)
            if (pthread_equal(v[j].th, pthread_self())) { k = j; break; }
        s = v[k];
        v.erase(v.begin() + k);
        if (v.empty()) g_shadows.erase(it);
    }
    if (!(s.flags & D3DLOCK_READONLY)) memcpy(s.host, MEM + s.guest, s.size);
    if (!s.persist) gfree(s.guest);
    return true;
}

// Vertex/index buffers keep a guest mirror of their whole contents. A lock
// hands out the mirror at the locked offset and the unlock copies the locked
// range back, so a NOOVERWRITE lock of a big range that writes only part of
// it leaves the rest intact, without reading the (write-combined) mapping.
std::map<void *, std::pair<uint32_t, uint32_t>> g_bufmirror;  // obj -> guest, size

uint32_t buffer_shadow(void *obj, uint32_t total, uint32_t off, void *hostbits, uint32_t size, DWORD flags) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto it = g_bufmirror.find(obj);
    if (it == g_bufmirror.end() || it->second.second != total) {
        if (it != g_bufmirror.end()) gfree(it->second.first);
        uint32_t g = gmalloc(total + 16);
        if (!g) rt_fatal("out of guest memory for a %u byte buffer", total);
        memset(MEM + g, 0, total);
        // the first lock sees what the buffer holds (zero for a new buffer)
        if (!(flags & D3DLOCK_DISCARD)) memcpy(MEM + g + off, hostbits, size);
        it = g_bufmirror.insert_or_assign(obj, std::make_pair(g, total)).first;
    }
    uint32_t g = it->second.first + off;
    g_shadows[{obj, 0}].push_back(Shadow{g, hostbits, size, flags, true, pthread_self()});
    return g;
}
}

// Pushes guest-side writes made since the last unlock into the host object.
void shadow_flush(void *obj) {
    std::vector<std::pair<const uint8_t *, std::function<void(const uint8_t *)>>> todo;
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        for (auto &kv : g_persist)
            if (kv.first.first == obj && !g_shadows.count(kv.first)) todo.push_back({MEM + kv.second.guest, kv.second.upload});
    }
    for (auto &t : todo) t.second(t.first);
}

void shadow_forget(void *obj) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto bm = g_bufmirror.find(obj);
    if (bm != g_bufmirror.end()) { gfree(bm->second.first); g_bufmirror.erase(bm); }
    for (auto it = g_persist.begin(); it != g_persist.end();) {
        if (it->first.first == obj) { gfree(it->second.guest); it = g_persist.erase(it); }
        else ++it;
    }
}

namespace {
bool is_block_format(D3DFORMAT f) {
    switch ((uint32_t)f) {
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case MAKEFOURCC('A', 'T', 'I', '1'): case MAKEFOURCC('A', 'T', 'I', '2'):
        return true;
    default:
        return false;
    }
}

uint32_t format_bits(D3DFORMAT f) {
    switch ((uint32_t)f) {
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: return 8;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_V8U8: case D3DFMT_A8R3G3B2:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_L6V5U5: return 16;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    default: return 32;
    }
}

// Bytes from the first locked byte to the last: with a rect, pBits points at
// its top-left corner, so the last row ends at its right edge, not a full
// pitch later (copying pitch * h would run past the end of the surface).
size_t rect_bytes(const D3DSURFACE_DESC &desc, const RECT *r, INT pitch) {
    uint32_t h = r ? (uint32_t)(r->bottom - r->top) : desc.Height;
    uint32_t w = r ? (uint32_t)(r->right - r->left) : desc.Width;
    size_t row;
    if (is_block_format(desc.Format)) {
        h = (h + 3) / 4;
        bool b8 = desc.Format == D3DFMT_DXT1 || desc.Format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1');
        row = (size_t)((w + 3) / 4) * (b8 ? 8 : 16);
    } else {
        row = ((size_t)w * format_bits(desc.Format) + 7) / 8;
    }
    if (!h) return 0;
    if (row > (size_t)pitch) row = (size_t)pitch;
    return (size_t)pitch * (h - 1) + row;
}

void lock_rect_common(CPU *c, void *obj, uint32_t sub, const D3DSURFACE_DESC &desc, HRESULT hr,
                      const D3DLOCKED_RECT &lr, const RECT *r, DWORD flags, uint32_t out,
                      std::function<void(const uint8_t *)> upload = nullptr) {
    if (FAILED(hr)) { M32(out) = 0; M32(out + 4) = 0; return; }
    if (desc.Pool != D3DPOOL_SYSTEMMEM || r) upload = nullptr;
    uint32_t g = shadow_begin(obj, sub, lr.pBits, rect_bytes(desc, r, lr.Pitch), flags, upload);
    M32(out) = (uint32_t)lr.Pitch;
    M32(out + 4) = g;
}
}

void B_IDirect3DSurface9_LockRect(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    const RECT *r = (const RECT *)GPTR(ARG(2));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    void *bt; uint32_t bsub; HRESULT hr;
    if (bc_surface_target(s, &bt, &bsub) &&
        bc_lock(bt, bsub, desc.Width, desc.Height, r, ARG(3), ARG(1),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return s->LockRect(l, rr, ARG(3)); }, &hr))
        RET(hr, 4);
    D3DLOCKED_RECT lr;
    hr = s->LockRect(&lr, r, ARG(3));
    size_t bytes = rect_bytes(desc, nullptr, lr.Pitch);
    lock_rect_common(c, s, 0, desc, hr, lr, r, ARG(3), ARG(1), [s, bytes](const uint8_t *src) {
        D3DLOCKED_RECT l2;
        if (SUCCEEDED(s->LockRect(&l2, nullptr, 0))) {
            memcpy(l2.pBits, src, bytes);
            s->UnlockRect();
        }
    });
    RET(hr, 4);
}

void B_IDirect3DSurface9_UnlockRect(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    void *bt; uint32_t bsub;
    if (bc_surface_target(s, &bt, &bsub)) bc_unlock(bt, bsub);
    shadow_end(s, 0);
    RET(s->UnlockRect(), 1);
}

void B_IDirect3DTexture9_LockRect(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    uint32_t level = ARG(1);
    const RECT *r = (const RECT *)GPTR(ARG(3));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    HRESULT hr;
    if (bc_lock(t, level, desc.Width, desc.Height, r, ARG(4), ARG(2),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return t->LockRect(level, l, rr, ARG(4)); }, &hr))
        RET(hr, 5);
    D3DLOCKED_RECT lr;
    hr = t->LockRect(level, &lr, r, ARG(4));
    size_t bytes = rect_bytes(desc, nullptr, lr.Pitch);
    lock_rect_common(c, t, level, desc, hr, lr, r, ARG(4), ARG(2), [t, level, bytes](const uint8_t *src) {
        D3DLOCKED_RECT l2;
        if (SUCCEEDED(t->LockRect(level, &l2, nullptr, 0))) {
            memcpy(l2.pBits, src, bytes);
            t->UnlockRect(level);
        }
    });
    RET(hr, 5);
}

void B_IDirect3DTexture9_UnlockRect(CPU *c) {
    auto *t = (IDirect3DTexture9 *)com_host(ARG(0));
    bc_unlock(t, ARG(1));
    shadow_end(t, ARG(1));
    RET(t->UnlockRect(ARG(1)), 2);
}

void B_IDirect3DCubeTexture9_LockRect(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    uint32_t face = ARG(1), level = ARG(2);
    const RECT *r = (const RECT *)GPTR(ARG(4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    HRESULT hr;
    if (bc_lock(t, face << 16 | level, desc.Width, desc.Height, r, ARG(5), ARG(3),
                [&](D3DLOCKED_RECT *l, const RECT *rr) { return t->LockRect((D3DCUBEMAP_FACES)face, level, l, rr, ARG(5)); }, &hr))
        RET(hr, 6);
    D3DLOCKED_RECT lr;
    hr = t->LockRect((D3DCUBEMAP_FACES)face, level, &lr, r, ARG(5));
    lock_rect_common(c, t, face << 16 | level, desc, hr, lr, r, ARG(5), ARG(3));
    RET(hr, 6);
}

void B_IDirect3DCubeTexture9_UnlockRect(CPU *c) {
    auto *t = (IDirect3DCubeTexture9 *)com_host(ARG(0));
    bc_unlock(t, ARG(1) << 16 | ARG(2));
    shadow_end(t, ARG(1) << 16 | ARG(2));
    RET(t->UnlockRect((D3DCUBEMAP_FACES)ARG(1), ARG(2)), 3);
}

static void lock_box_common(void *obj, uint32_t sub, const D3DVOLUME_DESC &desc, HRESULT hr,
                            const D3DLOCKED_BOX &lb, const D3DBOX *b, DWORD flags, uint32_t out) {
    if (FAILED(hr)) { M32(out) = M32(out + 4) = M32(out + 8) = 0; return; }
    uint32_t depth = b ? b->Back - b->Front : desc.Depth;
    size_t size = (size_t)lb.SlicePitch * depth;
    uint32_t g = shadow_begin(obj, sub, lb.pBits, size, flags);
    M32(out) = (uint32_t)lb.RowPitch;
    M32(out + 4) = (uint32_t)lb.SlicePitch;
    M32(out + 8) = g;
}

void B_IDirect3DVolumeTexture9_LockBox(CPU *c) {
    auto *t = (IDirect3DVolumeTexture9 *)com_host(ARG(0));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(ARG(1), &desc);
    D3DLOCKED_BOX lb;
    const D3DBOX *b = (const D3DBOX *)GPTR(ARG(3));
    HRESULT hr = t->LockBox(ARG(1), &lb, b, ARG(4));
    lock_box_common(t, ARG(1), desc, hr, lb, b, ARG(4), ARG(2));
    RET(hr, 5);
}

void B_IDirect3DVolumeTexture9_UnlockBox(CPU *c) {
    auto *t = (IDirect3DVolumeTexture9 *)com_host(ARG(0));
    shadow_end(t, ARG(1));
    RET(t->UnlockBox(ARG(1)), 2);
}

void B_IDirect3DVolume9_LockBox(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    D3DLOCKED_BOX lb;
    const D3DBOX *b = (const D3DBOX *)GPTR(ARG(2));
    HRESULT hr = v->LockBox(&lb, b, ARG(3));
    lock_box_common(v, 0, desc, hr, lb, b, ARG(3), ARG(1));
    RET(hr, 4);
}

void B_IDirect3DVolume9_UnlockBox(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    shadow_end(v, 0);
    RET(v->UnlockBox(), 1);
}

template <class B, class D>
static void buffer_lock(CPU *c) {
    auto *b = (B *)com_host(ARG(0));
    uint32_t off = ARG(1), size = ARG(2), flags = ARG(4);
    D desc;
    b->GetDesc(&desc);
    if (off > desc.Size) off = desc.Size;
    if (size == 0 || (uint64_t)off + size > desc.Size) size = desc.Size - off;
    void *p = nullptr;
    HRESULT hr = b->Lock(off, size, &p, flags);
    if (FAILED(hr) || !p) { M32(ARG(3)) = 0; RET(hr, 5); }
    M32(ARG(3)) = buffer_shadow(b, desc.Size, off, p, size, flags);
    RET(hr, 5);
}

template <class B>
static void buffer_unlock(CPU *c) {
    auto *b = (B *)com_host(ARG(0));
    shadow_end(b, 0);
    RET(b->Unlock(), 1);
}

void B_IDirect3DVertexBuffer9_Lock(CPU *c) { buffer_lock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
void B_IDirect3DVertexBuffer9_Unlock(CPU *c) { buffer_unlock<IDirect3DVertexBuffer9>(c); }
void B_IDirect3DIndexBuffer9_Lock(CPU *c) { buffer_lock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
void B_IDirect3DIndexBuffer9_Unlock(CPU *c) { buffer_unlock<IDirect3DIndexBuffer9>(c); }

// ---------------------------------------------------------------- misc

void B_IDirect3DSurface9_GetContainer(CPU *c) {
    auto *s = (IDirect3DSurface9 *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = s->GetContainer(iid, &out);
    int iface = d3d9_iface_from_iid(iid);
    M32(ARG(2)) = SUCCEEDED(hr) && out && iface >= 0 ? d3d9_wrap(out, iface) : 0;
    RET(hr, 3);
}

void B_IDirect3DVolume9_GetContainer(CPU *c) {
    auto *v = (IDirect3DVolume9 *)com_host(ARG(0));
    GUID iid;
    memcpy(&iid, GPTR(ARG(1)), sizeof iid);
    void *out = nullptr;
    HRESULT hr = v->GetContainer(iid, &out);
    int iface = d3d9_iface_from_iid(iid);
    M32(ARG(2)) = SUCCEEDED(hr) && out && iface >= 0 ? d3d9_wrap(out, iface) : 0;
    RET(hr, 3);
}

void B_IDirect3DSurface9_GetDC(CPU *c) { RET(D3DERR_INVALIDCALL, 2); }
void B_IDirect3DSurface9_ReleaseDC(CPU *c) { RET(D3DERR_INVALIDCALL, 2); }

// ---------------------------------------------------------------- entry point
void d3d9_register_vtables(void);

static void shim_Direct3DCreate9(CPU *c) {
    static bool init = false;
    if (!init) { d3d9_register_vtables(); init = true; }
    LOG("Direct3DCreate9(%u)", ARG(0));
    rt_host_affinity_begin();
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    rt_host_affinity_end();
    RET(d3d9_wrap(d3d, IFACE_IDirect3D9), 1);
}

static const ShimDef defs[] = {
    {"d3d9.dll", "Direct3DCreate9", shim_Direct3DCreate9},
    {nullptr, nullptr, nullptr},
};

extern "C" void d3d9_init(void) { rt_register_shims(defs); }
