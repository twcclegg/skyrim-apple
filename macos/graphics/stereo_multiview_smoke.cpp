// Single-pass stereo with Vulkan multiview (third_party/dxvk include/native/dxvk_stereo.h, LB_STEREO_MULTIVIEW)
// through real D3D9, DXVK and MoltenVK on this GPU, checked by pixel readback of both image layers.
//
// With LB_STEREO_MULTIVIEW=1 at launch, render targets and depth-stencil surfaces with the back buffer's exact size
// get two image layers, a pass whose attachments all have two layers renders with view mask 0b11, and every
// programmable vertex shader subtracts the clip-space offset of its own view (gl_ViewIndex). One draw therefore
// lands in both layers, each with its own eye offset. Layer 1 is read with dxvk_stereo_get_render_target_data_layer.
// The variable is read once per process, so the test is two invocations: --multiview=on and --multiview=off (this
// executable sets LB_STEREO_EYE_UNITS=1 and sets or unsets LB_STEREO_MULTIVIEW itself before any D3D9 call).
//
// Expected pixel shift, as in stereo_shift_smoke.cpp: an offset (ox, oy, 0, 0) on a vertex with w = 2 in a 128x128
// target moves it by dx = -ox * 128 / (2w), dy = +oy * 128 / (2w): -/+8 px for ox = +/-0.25.
//
// Checks with --multiview=on (PASS/FAIL lines, one JSON summary line, exit 2 on any FAIL, 1 on an API error):
//   1. policy: 128x128 render targets, render-target textures and depth surfaces (and the automatic depth buffer)
//      have two layers; the back buffer, a 64x128 target and a 256x256 depth surface have one; layer 1 of a
//      one-layer surface and layer 2 of a two-layer one cannot be read.
//   2. a perspective triangle (w = 2) drawn ONCE into a two-layer target: at rest in both layers; with
//      set_clip_shift(k) left by 8 px in layer 0 and right by 8 px in layer 1; with set_view_clip_offsets moved by
//      each view's own offset. A w == 1 quad in the same draws is identical in both layers.
//   3. Clear: a full Clear and a rectangle Clear issued after draws (inside the multiview render pass) reach both
//      layers.
//   4. depth per layer: a near triangle survives a far one drawn after it in both layers, at each layer's shifted
//      position, and the depth stored in each layer is the triangle's (bracketed by unshifted w == 1 probes).
//   5. mono passes: the back buffer (one layer) with the two-layer automatic depth buffer renders with view 0's
//      offset; a two-layer target with a one-layer depth surface renders mono into layer 0 and leaves layer 1 as
//      cleared.
//   6. reads of a two-layer render-target texture (drawn with multiview) see layer 0: StretchRect into the back
//      buffer and fixed-function sampling both match layer 0 pixel for pixel.
// With --multiview=off: multiview reports off, the 128x128 target has one layer, layer 1 cannot be read, and the
// triangle moves with view 0's offset (set_clip_shift(k): -8 px), as before multiview.
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <d3d9.h>
#include <dxvk_stereo.h>
#include <wsi/native_wsi.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    char detail[160];
    std::snprintf(detail, sizeof detail, "%s failed: 0x%08x", what, unsigned(hr));
    throw std::runtime_error(detail);
  }
}

// vs_3_0: dcl_position v0 / dcl_color v1 / dcl_position o0 / dcl_color o1 / mov o0, v0 / mov o1, v1
static const DWORD kVertexShader[] = {
  0xFFFE0300,
  0x0200001F, 0x80000000, 0x900F0000,
  0x0200001F, 0x8000000A, 0x900F0001,
  0x0200001F, 0x80000000, 0xE00F0000,
  0x0200001F, 0x8000000A, 0xE00F0001,
  0x02000001, 0xE00F0000, 0x90E40000,
  0x02000001, 0xE00F0001, 0x90E40001,
  0x0000FFFF };

// ps_3_0 / dcl_color v0 / mov oC0, v0
static const DWORD kPixelShader[] = {
  0xFFFF0300,
  0x0200001F, 0x8000000A, 0x900F0000,
  0x02000001, 0x800F0800, 0x90E40000,
  0x0000FFFF };

struct Vertex { float x, y, z, w; uint32_t color; };
static_assert(sizeof(Vertex) == 20, "vertex stride");

constexpr int      kSize = 128;              // square back buffer; stereo targets have this size
constexpr int      kPixels = kSize * kSize;
constexpr float    kW = 2.0f;                // w of every perspective vertex
constexpr float    kK = 0.25f;               // clip-space offset
constexpr double   kShiftPx = double(kK) * kSize / (2.0 * kW);  // 8
constexpr float    kDepthTol = 1.0f / 512.0f;
constexpr uint32_t kClear = 0x101020, kRed = 0xFF0000, kGreen = 0x00FF00, kBlue = 0x0000FF, kGrey = 0x404040;

static const float kTri[3][2] = { { 40.25f, 32.25f }, { 88.25f, 32.25f }, { 64.25f, 88.25f } };
// A far triangle around kTri (and around its +-8 px shifted copies).
static const float kBigTri[3][2] = { { 4.25f, 8.25f }, { 124.25f, 8.25f }, { 64.25f, 124.25f } };
constexpr float kQuadX0 = 15.5f, kQuadX1 = 111.5f, kQuadY0 = 99.5f, kQuadY1 = 115.5f;
constexpr int   kQuadMinX = 16, kQuadMaxX = 111, kQuadMinY = 100, kQuadMaxY = 115;
// Rectangle Clear (D3DRECT, exclusive right/bottom): 32 x 16 px.
constexpr LONG  kRectX1 = 8, kRectY1 = 8, kRectX2 = 40, kRectY2 = 24;

static Vertex clipVertex(float px, float py, float depth, float w, uint32_t argb) {
  return { (2.0f * px / float(kSize) - 1.0f) * w, (1.0f - 2.0f * py / float(kSize)) * w, depth * w, w, argb };
}

struct Blob {
  int count = 0;
  double cx = 0.0, cy = 0.0;
  int minX = 1 << 30, maxX = -1, minY = 1 << 30, maxY = -1;
};

template <typename Pred>
static Blob measure(const std::vector<uint32_t>& pixels, Pred match) {
  Blob b;
  double sx = 0.0, sy = 0.0;
  for (int y = 0; y < kSize; ++y)
    for (int x = 0; x < kSize; ++x) {
      if (!match(pixels[size_t(y) * kSize + x])) continue;
      ++b.count; sx += x; sy += y;
      if (x < b.minX) b.minX = x;
      if (x > b.maxX) b.maxX = x;
      if (y < b.minY) b.minY = y;
      if (y > b.maxY) b.maxY = y;
    }
  if (b.count) { b.cx = sx / b.count; b.cy = sy / b.count; }
  return b;
}

static Blob measureRgb(const std::vector<uint32_t>& pixels, uint32_t rgb) {
  return measure(pixels, [rgb](uint32_t p) { return p == rgb; });
}

struct Results {
  int checks = 0, failed = 0;
  std::string failedNames;
  void expect(bool ok, const char* name, const char* fmt, ...) {
    char detail[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);
    ++checks;
    if (!ok) {
      ++failed;
      failedNames += (failedNames.empty() ? "\"" : ",\"") + std::string(name) + "\"";
    }
    std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
    std::fflush(stdout);
  }
};

struct Probe {
  IDirect3DDevice9* device = nullptr;
  IDirect3DSurface9* sysmem = nullptr;  // 128x128 A8R8G8B8 readback

  // GetRenderTargetData from one image layer.
  std::vector<uint32_t> read(IDirect3DSurface9* target, unsigned layer) {
    check(HRESULT(dxvk_stereo_get_render_target_data_layer(device, target, sysmem, layer)),
        "dxvk_stereo_get_render_target_data_layer");
    D3DLOCKED_RECT locked{};
    check(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect");
    std::vector<uint32_t> pixels(static_cast<size_t>(kPixels));
    for (int y = 0; y < kSize; ++y)
      for (int x = 0; x < kSize; ++x) {
        uint32_t v;
        std::memcpy(&v, static_cast<const char*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
        pixels[size_t(y) * kSize + x] = v & 0xFFFFFFu;
      }
    check(sysmem->UnlockRect(), "UnlockRect");
    return pixels;
  }

  void triangle(const float (&tri)[3][2], float depth, uint32_t argb) {
    const Vertex v[3] = { clipVertex(tri[0][0], tri[0][1], depth, kW, argb),
                          clipVertex(tri[1][0], tri[1][1], depth, kW, argb),
                          clipVertex(tri[2][0], tri[2][1], depth, kW, argb) };
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vertex)), "DrawPrimitiveUP(triangle)");
  }

  void quad(float x0, float y0, float x1, float y1, float depth, uint32_t argb) {
    const Vertex v[4] = { clipVertex(x0, y0, depth, 1.0f, argb), clipVertex(x1, y0, depth, 1.0f, argb),
                          clipVertex(x0, y1, depth, 1.0f, argb), clipVertex(x1, y1, depth, 1.0f, argb) };
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex)), "DrawPrimitiveUP(quad)");
  }

  void bind(IDirect3DSurface9* target, IDirect3DSurface9* depth) {
    check(device->SetRenderTarget(0, target), "SetRenderTarget");
    check(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
  }

  // One draw of the quad (green, w == 1) and one of the triangle (red, w = 2); no depth test.
  void sceneFrame(IDirect3DSurface9* target, IDirect3DSurface9* depth) {
    bind(target, depth);
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kClear, 1.0f, 0), "Clear");
    check(device->BeginScene(), "BeginScene");
    quad(kQuadX0, kQuadY0, kQuadX1, kQuadY1, 0.5f, 0xFF000000u | kGreen);
    triangle(kTri, 0.5f, 0xFF000000u | kRed);
    check(device->EndScene(), "EndScene");
  }
};

static int run(bool multiview) {
  ::setenv("LB_STEREO_EYE_UNITS", "1", 1);
  ::unsetenv("LB_STEREO_PASSES");
  if (multiview) ::setenv("LB_STEREO_MULTIVIEW", "1", 1);
  else ::unsetenv("LB_STEREO_MULTIVIEW");
  SDL_setenv("DXVK_CONFIG_FILE", SKYRIM_GRAPHICS_CONFIG_PATH, 1);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
  if (SDL_Vulkan_LoadLibrary(SKYRIM_MOLTENVK_LIBRARY_PATH) != 0) throw std::runtime_error(SDL_GetError());
  SDL_Window* window = SDL_CreateWindow("Skyrim stereo multiview probe", SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, kSize, kSize, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  if (!window) throw std::runtime_error(SDL_GetError());
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d || !d3d->GetAdapterCount()) throw std::runtime_error("No D3D9 adapter");
  D3DADAPTER_IDENTIFIER9 adapter{};
  check(d3d->GetAdapterIdentifier(0, 0, &adapter), "GetAdapterIdentifier");
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = pp.BackBufferHeight = kSize;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = dxvk::wsi::toHwnd(window);
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  Probe p;
  check(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
      &p.device), "CreateDevice");
  IDirect3DDevice9* device = p.device;
  Results r;
  std::printf("INFO setup: LB_STEREO_MULTIVIEW=%s LB_STEREO_EYE_UNITS=1, back buffer %dx%d, w=%g, k=%g\n",
      multiview ? "1" : "(unset)", kSize, kSize, kW, kK);

  IDirect3DSurface9 *backbuffer = nullptr, *autoDepth = nullptr, *target = nullptr, *depth = nullptr;
  IDirect3DSurface9 *narrow = nullptr, *bigDepth = nullptr, *texLevel = nullptr;
  IDirect3DTexture9* texture = nullptr;
  check(device->GetRenderTarget(0, &backbuffer), "GetRenderTarget");
  check(device->GetDepthStencilSurface(&autoDepth), "GetDepthStencilSurface");
  check(device->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr),
      "CreateRenderTarget(stereo)");
  check(device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth,
      nullptr), "CreateDepthStencilSurface(stereo)");
  check(device->CreateRenderTarget(kSize / 2, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &narrow,
      nullptr), "CreateRenderTarget(64x128)");
  check(device->CreateDepthStencilSurface(2 * kSize, 2 * kSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
      &bigDepth, nullptr), "CreateDepthStencilSurface(256x256)");
  check(device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture,
      nullptr), "CreateTexture(render target)");
  check(texture->GetSurfaceLevel(0, &texLevel), "GetSurfaceLevel");
  check(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &p.sysmem, nullptr),
      "CreateOffscreenPlainSurface");

  IDirect3DVertexDeclaration9* decl = nullptr;
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  const D3DVERTEXELEMENT9 elements[] = {
    { 0,  0, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    D3DDECL_END() };
  check(device->CreateVertexDeclaration(elements, &decl), "CreateVertexDeclaration");
  check(device->CreateVertexShader(kVertexShader, &vs), "CreateVertexShader");
  check(device->CreatePixelShader(kPixelShader, &ps), "CreatePixelShader");
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "CULLMODE");
  check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "LIGHTING");
  check(device->SetVertexDeclaration(decl), "SetVertexDeclaration");
  check(device->SetVertexShader(vs), "SetVertexShader");
  check(device->SetPixelShader(ps), "SetPixelShader");

  const double triCx = (kTri[0][0] + kTri[1][0] + kTri[2][0]) / 3.0;
  const double triCy = (kTri[0][1] + kTri[1][1] + kTri[2][1]) / 3.0;
  auto quadOk = [](const Blob& q) {
    return q.count == (kQuadMaxX - kQuadMinX + 1) * (kQuadMaxY - kQuadMinY + 1) && q.minX == kQuadMinX &&
        q.maxX == kQuadMaxX && q.minY == kQuadMinY && q.maxY == kQuadMaxY;
  };
  auto layerName = [](char* buf, size_t n, const char* base, int layer) {
    std::snprintf(buf, n, "%s_layer%d", base, layer);
    return buf;
  };

  const int mvEnabled = dxvk_stereo_multiview_enabled(device);
  r.expect(mvEnabled == (multiview ? 1 : 0), "multiview_enabled", "dxvk_stereo_multiview_enabled() = %d, expect %d",
      mvEnabled, multiview ? 1 : 0);
  r.expect(dxvk_stereo_enabled() == 1, "stereo_enabled", "dxvk_stereo_enabled() = %d", dxvk_stereo_enabled());

  if (!multiview) {
    // Switch off: one layer everywhere, layer 1 unreadable, view 0's offset only.
    const int layers = dxvk_stereo_surface_layers(target);
    r.expect(layers == 1, "off_target_one_layer", "128x128 render target: %d layer(s)", layers);
    const long hr = dxvk_stereo_get_render_target_data_layer(device, target, p.sysmem, 1);
    r.expect(hr == D3DERR_INVALIDCALL, "off_layer1_unreadable", "layer 1 read: 0x%08lx", hr & 0xFFFFFFFFl);
    dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
    p.sceneFrame(target, nullptr);
    const Blob rest = measureRgb(p.read(target, 0), kRed);
    dxvk_stereo_set_clip_shift(kK);
    p.sceneFrame(target, nullptr);
    const Blob shifted = measureRgb(p.read(target, 0), kRed);
    r.expect(rest.count >= 1000 && std::fabs(shifted.cx - rest.cx + kShiftPx) <= 0.25 &&
        std::fabs(shifted.cy - rest.cy) <= 0.25, "off_shift_view0",
        "set_clip_shift(%g): moved (%+.2f, %+.2f), expect (%+.2f, +0.00) +-0.25", kK, shifted.cx - rest.cx,
        shifted.cy - rest.cy, -kShiftPx);
    const float v0[4] = { -kK, 0.0f, 0.0f, 0.0f }, v1[4] = { 0.0f, kK, 0.0f, 0.0f };
    dxvk_stereo_set_view_clip_offsets(v0, v1);
    p.sceneFrame(target, nullptr);
    const Blob views = measureRgb(p.read(target, 0), kRed);
    r.expect(std::fabs(views.cx - rest.cx - kShiftPx) <= 0.25 && std::fabs(views.cy - rest.cy) <= 0.25,
        "off_view_offsets_view0", "set_view_clip_offsets(view0 = -k x): moved (%+.2f, %+.2f), expect (%+.2f, +0.00)",
        views.cx - rest.cx, views.cy - rest.cy, kShiftPx);
  } else {
    // 1: which surfaces get two layers.
    struct LayerCase { const char* name; IDirect3DSurface9* s; int expect; };
    const LayerCase layerCases[] = {
      { "layers_render_target_128",  target,     2 },
      { "layers_depth_surface_128",  depth,      2 },
      { "layers_auto_depth",         autoDepth,  2 },
      { "layers_rt_texture_128",     texLevel,   2 },
      { "layers_back_buffer",        backbuffer, 1 },
      { "layers_render_target_64x128", narrow,   1 },
      { "layers_depth_surface_256",  bigDepth,   2 },  // larger than the back buffer: still the scene's
    };
    for (const LayerCase& c : layerCases) {
      const int layers = dxvk_stereo_surface_layers(c.s);
      r.expect(layers == c.expect, c.name, "%d layer(s), expect %d", layers, c.expect);
    }
    const long hrBack = dxvk_stereo_get_render_target_data_layer(device, backbuffer, p.sysmem, 1);
    const long hrTwo = dxvk_stereo_get_render_target_data_layer(device, target, p.sysmem, 2);
    r.expect(hrBack == D3DERR_INVALIDCALL && hrTwo == D3DERR_INVALIDCALL, "missing_layers_unreadable",
        "back buffer layer 1: 0x%08lx, stereo target layer 2: 0x%08lx", hrBack & 0xFFFFFFFFl, hrTwo & 0xFFFFFFFFl);

    // 2: one draw, both layers.
    char name[96];
    dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
    p.sceneFrame(target, nullptr);
    std::vector<uint32_t> restPx[2] = { p.read(target, 0), p.read(target, 1) };
    Blob rest[2];
    for (int l = 0; l < 2; ++l) {
      rest[l] = measureRgb(restPx[l], kRed);
      const Blob q = measureRgb(restPx[l], kGreen);
      r.expect(rest[l].count >= 1000 && std::fabs(rest[l].cx - triCx) <= 1.0 && std::fabs(rest[l].cy - triCy) <= 1.0
          && quadOk(q), layerName(name, sizeof name, "rest", l),
          "triangle %d px at (%.2f, %.2f), expect >= 1000 px near (%.2f, %.2f) +-1; quad %d px x %d..%d y %d..%d",
          rest[l].count, rest[l].cx, rest[l].cy, triCx, triCy, q.count, q.minX, q.maxX, q.minY, q.maxY);
    }
    r.expect(restPx[0] == restPx[1], "rest_layers_identical", "layer 0 and layer 1 %s",
        restPx[0] == restPx[1] ? "match pixel for pixel" : "differ");

    struct ViewCase { const char* name; float v0[4], v1[4]; double dx[2], dy[2]; };
    const ViewCase viewCases[] = {
      // set_clip_shift(k): view 0 gets +k (left by 8), view 1 gets -k (right by 8).
      { "shift_k",       { kK, 0, 0, 0 },  { -kK, 0, 0, 0 }, { -kShiftPx, +kShiftPx }, { 0.0, 0.0 } },
      // Independent offsets: view 0 right by 8, view 1 down by 8.
      { "view_offsets",  { -kK, 0, 0, 0 }, { 0, kK, 0, 0 },  { +kShiftPx, 0.0 },       { 0.0, +kShiftPx } },
    };
    for (const ViewCase& c : viewCases) {
      if (std::strcmp(c.name, "shift_k") == 0) dxvk_stereo_set_clip_shift(kK);
      else dxvk_stereo_set_view_clip_offsets(c.v0, c.v1);
      p.sceneFrame(target, nullptr);
      for (int l = 0; l < 2; ++l) {
        const auto px = p.read(target, l);
        const Blob t = measureRgb(px, kRed), q = measureRgb(px, kGreen);
        const double dx = t.cx - rest[l].cx, dy = t.cy - rest[l].cy;
        const bool ok = t.count > 0 && std::abs(t.count - rest[l].count) <= rest[l].count / 50 &&
            std::fabs(dx - c.dx[l]) <= 0.25 && std::fabs(dy - c.dy[l]) <= 0.25 && quadOk(q);
        r.expect(ok, layerName(name, sizeof name, c.name, l),
            "offset (%g, %g, 0, 0): triangle %d px moved (%+.2f, %+.2f), expect (%+.2f, %+.2f) +-0.25; quad %s",
            l ? c.v1[0] : c.v0[0], l ? c.v1[1] : c.v0[1], t.count, dx, dy, c.dx[l], c.dy[l],
            quadOk(q) ? "unmoved" : "WRONG");
      }
    }

    // 3: clears inside the multiview pass (after a draw, so the render pass is running).
    dxvk_stereo_set_clip_shift(kK);
    p.sceneFrame(target, nullptr);
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kGrey, 1.0f, 0), "Clear(full, after draw)");
    for (int l = 0; l < 2; ++l) {
      const Blob g = measureRgb(p.read(target, l), kGrey);
      r.expect(g.count == kPixels, layerName(name, sizeof name, "clear_full_in_pass", l),
          "%d of %d px grey", g.count, kPixels);
    }
    p.sceneFrame(target, nullptr);
    const D3DRECT rect = { kRectX1, kRectY1, kRectX2, kRectY2 };
    check(device->Clear(1, &rect, D3DCLEAR_TARGET, 0xFF000000u | kBlue, 1.0f, 0), "Clear(rect, after draw)");
    for (int l = 0; l < 2; ++l) {
      const auto px = p.read(target, l);
      const Blob b = measureRgb(px, kBlue);
      const Blob t = measureRgb(px, kRed);
      const int expect = int((kRectX2 - kRectX1) * (kRectY2 - kRectY1));
      r.expect(b.count == expect && b.minX == kRectX1 && b.maxX == kRectX2 - 1 && b.minY == kRectY1 &&
          b.maxY == kRectY2 - 1 && std::abs(t.count - rest[l].count) <= rest[l].count / 50, layerName(name, sizeof name, "clear_rect_in_pass", l),
          "%d px blue x %d..%d y %d..%d (expect %d px x %ld..%ld y %ld..%ld); triangle kept: %d px",
          b.count, b.minX, b.maxX, b.minY, b.maxY, expect, long(kRectX1), long(kRectX2 - 1), long(kRectY1),
          long(kRectY2 - 1), t.count);
    }

    // 4a: depth test per layer. Near green triangle first, then a far red one around it, same offsets.
    p.bind(target, depth);
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "ZENABLE");
    check(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
    check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "ZFUNC");
    dxvk_stereo_set_clip_shift(kK);
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000u | kClear,
        1.0f, 0), "Clear(depth)");
    check(device->BeginScene(), "BeginScene");
    p.triangle(kTri, 0.25f, 0xFF000000u | kGreen);
    p.triangle(kBigTri, 0.75f, 0xFF000000u | kRed);
    check(device->EndScene(), "EndScene");
    for (int l = 0; l < 2; ++l) {
      const auto px = p.read(target, l);
      const Blob g = measureRgb(px, kGreen), red = measureRgb(px, kRed);
      const double expectDx = l == 0 ? -kShiftPx : +kShiftPx;
      const bool ok = std::abs(g.count - rest[l].count) <= rest[l].count / 50 &&
          std::fabs(g.cx - rest[l].cx - expectDx) <= 0.25 && std::fabs(g.cy - rest[l].cy) <= 0.25 && red.count > 1000;
      r.expect(ok, layerName(name, sizeof name, "depth_near_survives", l),
          "near triangle %d px (expect %d +-2%%) moved %+.2f px (expect %+.2f); far triangle %d px around it",
          g.count, rest[l].count, g.cx - rest[l].cx, expectDx, red.count);
    }

    // 4b: the depth value stored in each layer. Triangle (blue) at NDC depth 0.5 with the shift, then unshifted
    // w == 1 full-screen probes with depth writes off and ZFUNC LESS: red only at lo (red: stored > lo), green only
    // at hi (green: stored > hi). Triangle pixels must be red 255 and green 0; elsewhere the clear (1.0) gives both.
    {
      const float lo = 0.5f - kDepthTol, hi = 0.5f + kDepthTol;
      check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000u, 1.0f, 0),
          "Clear(depth probe)");
      check(device->BeginScene(), "BeginScene");
      p.triangle(kTri, 0.5f, 0xFF0000FFu);
      check(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE), "ZWRITEENABLE");
      check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS), "ZFUNC");
      check(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED), "COLORWRITEENABLE(red)");
      p.quad(-1.0f, -1.0f, kSize + 1.0f, kSize + 1.0f, lo, 0xFFFFFFFFu);
      check(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN), "COLORWRITEENABLE(green)");
      p.quad(-1.0f, -1.0f, kSize + 1.0f, kSize + 1.0f, hi, 0xFFFFFFFFu);
      check(device->EndScene(), "EndScene");
      check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF), "COLORWRITEENABLE");
      check(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
      check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "ZFUNC");
      for (int l = 0; l < 2; ++l) {
        const auto px = p.read(target, l);
        auto isTri = [](uint32_t v) { return (v & 0xFFu) == 0xFFu; };
        const Blob t = measure(px, isTri);
        int wrong = 0, outside = 0;
        for (uint32_t v : px) {
          if (isTri(v)) wrong += ((v >> 16) & 0xFFu) != 0xFFu || ((v >> 8) & 0xFFu) != 0x00u;
          else outside += v != 0xFFFF00u;
        }
        const double expectDx = l == 0 ? -kShiftPx : +kShiftPx;
        r.expect(t.count >= 1000 && wrong == 0 && outside == 0 && std::fabs(t.cx - rest[l].cx - expectDx) <= 0.25,
            layerName(name, sizeof name, "depth_stored", l),
            "triangle %d px moved %+.2f (expect %+.2f): %d px with depth outside (%.4f, %.4f], %d px elsewhere not at "
            "the cleared depth", t.count, t.cx - rest[l].cx, expectDx, wrong, lo, hi, outside);
      }
    }
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");

    // 5a: back buffer (one layer) + automatic depth (two layers): mono, view 0's offset (+k: left by 8).
    dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
    p.sceneFrame(backbuffer, autoDepth);
    const Blob bbRest = measureRgb(p.read(backbuffer, 0), kRed);
    dxvk_stereo_set_clip_shift(kK);
    p.sceneFrame(backbuffer, autoDepth);
    const Blob bbShift = measureRgb(p.read(backbuffer, 0), kRed);
    r.expect(bbRest.count >= 1000 && std::fabs(bbShift.cx - bbRest.cx + kShiftPx) <= 0.25 &&
        std::fabs(bbShift.cy - bbRest.cy) <= 0.25, "mono_back_buffer_view0",
        "back buffer + two-layer depth: moved (%+.2f, %+.2f), expect (%+.2f, +0.00) +-0.25",
        bbShift.cx - bbRest.cx, bbShift.cy - bbRest.cy, -kShiftPx);

    // 5b: two-layer target + a larger (256x256) depth-stencil, as the game's scene uses a 1280x1024 one with its
    // 1280x720 targets: the depth-stencil has two layers too, so the pass is multiview and each layer gets its eye.
    p.sceneFrame(target, bigDepth);
    {
      const Blob t0 = measureRgb(p.read(target, 0), kRed), t1 = measureRgb(p.read(target, 1), kRed);
      r.expect(std::fabs(t0.cx - rest[0].cx + kShiftPx) <= 0.25 && std::fabs(t1.cx - rest[1].cx - kShiftPx) <= 0.25 &&
          std::abs(t0.count - rest[0].count) <= rest[0].count / 50 && std::abs(t1.count - rest[1].count) <=
          rest[1].count / 50, "larger_depth_multiview",
          "two-layer target + 256x256 two-layer depth: layer 0 moved %+.2f, layer 1 %+.2f (expect %+.2f, %+.2f)",
          t0.cx - rest[0].cx, t1.cx - rest[1].cx, -kShiftPx, kShiftPx);
    }

    // 6: D3D9 reads of a two-layer render-target texture see layer 0 (the left eye here): StretchRect into the
    // back buffer, and sampling it with a fixed-function full-screen quad (point filter, pre-transformed).
    dxvk_stereo_set_clip_shift(kK);
    p.sceneFrame(texLevel, nullptr);
    {
      const auto src0 = p.read(texLevel, 0), src1 = p.read(texLevel, 1);
      const Blob s0 = measureRgb(src0, kRed), s1 = measureRgb(src1, kRed);
      r.expect(std::fabs(s0.cx - rest[0].cx + kShiftPx) <= 0.25 && std::fabs(s1.cx - rest[1].cx - kShiftPx) <= 0.25,
          "rt_texture_multiview", "render-target texture: layer 0 moved %+.2f, layer 1 %+.2f (expect %+.2f, %+.2f)",
          s0.cx - rest[0].cx, s1.cx - rest[1].cx, -kShiftPx, kShiftPx);

      check(device->StretchRect(texLevel, nullptr, backbuffer, nullptr, D3DTEXF_NONE), "StretchRect");
      const auto blit = p.read(backbuffer, 0);
      r.expect(blit == src0 && blit != src1, "stretchrect_reads_layer0", "StretchRect result %s layer 0, %s layer 1",
          blit == src0 ? "matches" : "differs from", blit == src1 ? "matches" : "differs from");

      struct TexVertex { float x, y, z, rhw, u, v; };
      const TexVertex quadTex[4] = { { -0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f },
                                     { kSize - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
                                     { -0.5f, kSize - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f },
                                     { kSize - 0.5f, kSize - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f } };
      p.bind(backbuffer, nullptr);
      check(device->SetVertexShader(nullptr), "SetVertexShader(null)");
      check(device->SetPixelShader(nullptr), "SetPixelShader(null)");
      check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "SetFVF");
      check(device->SetTexture(0, texture), "SetTexture");
      check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "MINFILTER");
      check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "MAGFILTER");
      check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP");
      check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "COLORARG1");
      check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u, 1.0f, 0), "Clear(back buffer)");
      check(device->BeginScene(), "BeginScene");
      check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quadTex, sizeof(TexVertex)), "DrawPrimitiveUP(textured)");
      check(device->EndScene(), "EndScene");
      check(device->SetTexture(0, nullptr), "SetTexture(null)");
      check(device->SetVertexDeclaration(decl), "SetVertexDeclaration");
      check(device->SetVertexShader(vs), "SetVertexShader");
      check(device->SetPixelShader(ps), "SetPixelShader");
      const auto sampled = p.read(backbuffer, 0);
      r.expect(sampled == src0 && sampled != src1, "sample_reads_layer0", "sampled texture %s layer 0, %s layer 1",
          sampled == src0 ? "matches" : "differs from", sampled == src1 ? "matches" : "differs from");
    }
  }
  dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);

  {
    // Occlusion queries around the quad (w == 1, so it covers the same pixels in both views). Inside a multiview
    // pass each query uses one pool slot per view; DXVK reserves them in pairs and reports the views' average,
    // so the count equals the quad's area in either mode. 64 queries in one pass also exercise neighbouring slots;
    // a result that never becomes available would mean a slot the GPU never wrote.
    constexpr int kQueries = 64;
    const DWORD area = DWORD((kQuadMaxX - kQuadMinX + 1) * (kQuadMaxY - kQuadMinY + 1));
    std::vector<IDirect3DQuery9*> queries(kQueries, nullptr);
    for (auto& q : queries) check(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q), "CreateQuery(OCCLUSION)");
    p.bind(target, nullptr);
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kClear, 1.0f, 0), "Clear");
    check(device->BeginScene(), "BeginScene");
    for (auto* q : queries) {
      check(q->Issue(D3DISSUE_BEGIN), "Issue(BEGIN)");
      p.quad(kQuadX0, kQuadY0, kQuadX1, kQuadY1, 0.5f, 0xFF000000u | kGreen);
      check(q->Issue(D3DISSUE_END), "Issue(END)");
    }
    check(device->EndScene(), "EndScene");
    int wrong = 0, pending = 0;
    DWORD first = 0;
    for (int i = 0; i < kQueries; i++) {
      DWORD count = 0;
      HRESULT hr = S_FALSE;
      for (int tries = 0; tries < 2000 && hr == S_FALSE; tries++) {
        hr = queries[i]->GetData(&count, sizeof count, D3DGETDATA_FLUSH);
        if (hr == S_FALSE) SDL_Delay(1);
      }
      if (i == 0) first = count;
      if (hr != S_OK) pending++;
      else if (count != area) wrong++;
      queries[i]->Release();
    }
    r.expect(pending == 0 && wrong == 0, "occlusion_query_count",
        "%d occlusion queries: first %lu samples (expect %lu), %d wrong, %d never available", kQueries,
        (unsigned long)first, (unsigned long)area, wrong, pending);
  }

  const bool pass = r.failed == 0;
  std::printf("{\"probe\":\"d3d9_stereo_multiview\",\"adapter\":\"%s\",\"multiview\":%s,\"checks\":%d,\"failed\":%d,"
      "\"failed_checks\":[%s],\"pass\":%s}\n", adapter.Description, multiview ? "true" : "false", r.checks, r.failed,
      r.failedNames.c_str(), pass ? "true" : "false");
  std::printf("%s\n", pass ? "PASS stereo_multiview_smoke" : "FAIL stereo_multiview_smoke");
  std::fflush(stdout);

  ps->Release(); vs->Release(); decl->Release();
  p.sysmem->Release(); texLevel->Release(); texture->Release(); bigDepth->Release(); narrow->Release();
  depth->Release(); target->Release(); autoDepth->Release(); backbuffer->Release();
  device->Release(); d3d->Release();
  SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary(); SDL_Quit();
  return pass ? 0 : 2;
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--multiview=on") == 0) return run(true);
    if (argc == 2 && std::strcmp(argv[1], "--multiview=off") == 0) return run(false);
    std::fprintf(stderr, "usage: stereo_multiview_smoke --multiview=on | --multiview=off\n");
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stereo_multiview_smoke: %s\n", e.what());
    SDL_Quit();
    return 1;
  }
}
