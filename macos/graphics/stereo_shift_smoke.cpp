// The stereo experiment's clip-space offset (third_party/dxvk include/native/dxvk_stereo.h) through real
// D3D9, DXVK and MoltenVK on this GPU, checked by pixel readback.
//
// With LB_STEREO_EYE_UNITS non-zero at launch, every programmable vs_* shader ends with
//     pos -= offset * (abs(pos.w - 1) > 1e-5)        (DxsoCompiler::emitVsClipping)
// where offset = (ox, oy, oz, ow) is the last dxvk_stereo_set_clip_offset() (set_clip_shift(k) is (k, 0, 0, 0)).
// D3D9DeviceEx::UpdateStereoClipShift zeroes it for draws whose viewport does not have the back buffer's
// aspect ratio unless LB_STEREO_PASSES=all. Both variables are read once per process, so the test is two
// invocations: --passes=default and --passes=all (this executable sets the variables itself before any D3D9
// call; ctest also passes them, see macos/CMakeLists.txt).
//
// Expected pixel shift. For a vertex with clip (x, y, z, w), w != 1, the shader outputs (x-ox, y-oy, z-oz, w-ow).
// D3D9 maps NDC to pixels as px = (x/w + 1) * W/2, py = (1 - y/w) * H/2 (pixel centres at integers; DXVK's
// half-pixel viewport bias keeps it so). With oz = ow = 0 and every vertex at the same w:
//     dx = -ox * W / (2w)        dy = +oy * H / (2w)
// so +k moves geometry left and -k right. Here W = H = 128, w = 2, k = 0.25: dx = -/+8 px exactly, which keeps
// rasterisation a pure translation. In the 64x128 off-screen target W = 64, so dx = -4 px when it is shifted.
// With ow != 0 the divide changes too: px - W/2 scales by w / (w - ow) about the centre, and the depth written
// is (z - oz) / (w - ow).
//
// Checks (PASS/FAIL lines, one JSON summary line, exit 2 on any FAIL, 1 on an API error):
//   1. perspective triangle (w = 2), programmable VS: at rest where the vertices say; +k, -k, (0, k) move it
//      by the formula above.
//   2. a screen-space quad (w = 1) in the same frames never moves.
//   3. a 64x128 render target (aspect 1:2, back buffer 1:1): unshifted by default, shifted with
//      LB_STEREO_PASSES=all.
//   4. depth: set_clip_shift leaves z and w alone (depth z/w unchanged; a sky triangle at z == w stays on the
//      far plane, and moves sideways like any other perspective geometry); a full offset with oz == ow writes
//      (z - oz)/(w - ow), which keeps z == w on the far plane. A sky triangle under oz != ow is printed as INFO.
//   5. the same triangle through the fixed-function vertex pipeline is printed as INFO: it is not shifted
//      (known gap), and the line says so if that ever changes.
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

// vs_3_0
//   dcl_position v0 / dcl_color v1 / dcl_position o0 / dcl_color o1
//   mov o0, v0 / mov o1, v1
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

struct Vertex   { float x, y, z, w; uint32_t color; };
struct FFVertex { float x, y, z;    uint32_t color; };
static_assert(sizeof(Vertex) == 20 && sizeof(FFVertex) == 16, "vertex strides");

constexpr int      kSize = 128;              // square back buffer
constexpr int      kOffW = 64, kOffH = 128;  // off-screen target, not the back buffer's aspect
constexpr float    kW = 2.0f;                // w of every perspective vertex
constexpr float    kK = 0.25f;               // clip-space offset used throughout
constexpr float    kDepthTol = 1.0f / 512.0f;
constexpr uint32_t kClear = 0x101020, kRed = 0xFF0000, kGreen = 0x00FF00;

// Triangle corners in back-buffer pixels (centres at integers), off the pixel grid by a quarter.
static const float kTri[3][2] = { { 40.25f, 32.25f }, { 88.25f, 32.25f }, { 64.25f, 88.25f } };
// The same shape in the 64x128 off-screen target.
static const float kOffTri[3][2] = { { 16.25f, 32.25f }, { 48.25f, 32.25f }, { 32.25f, 88.25f } };
// Screen-space quad (w = 1) edges; it covers pixels x 16..111, y 100..115.
constexpr float kQuadX0 = 15.5f, kQuadX1 = 111.5f, kQuadY0 = 99.5f, kQuadY1 = 115.5f;
constexpr int   kQuadMinX = 16, kQuadMaxX = 111, kQuadMinY = 100, kQuadMaxY = 115;

// Pixel (px, py) of a width x height viewport at NDC depth `depth`, as clip coordinates with the given w.
static Vertex clipVertex(float px, float py, float depth, float w, int width, int height, uint32_t argb) {
  return { (2.0f * px / float(width) - 1.0f) * w, (1.0f - 2.0f * py / float(height)) * w, depth * w, w, argb };
}

static void centroidOf(const float (&tri)[3][2], double& cx, double& cy) {
  cx = (tri[0][0] + tri[1][0] + tri[2][0]) / 3.0;
  cy = (tri[0][1] + tri[1][1] + tri[2][1]) / 3.0;
}

struct Blob {
  int count = 0;
  double cx = 0.0, cy = 0.0;
  int minX = 1 << 30, maxX = -1, minY = 1 << 30, maxY = -1;
};

template <typename Pred>
static Blob measure(const std::vector<uint32_t>& pixels, int width, int height, Pred match) {
  Blob b;
  double sx = 0.0, sy = 0.0;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      if (!match(pixels[size_t(y) * width + x])) continue;
      ++b.count; sx += x; sy += y;
      if (x < b.minX) b.minX = x;
      if (x > b.maxX) b.maxX = x;
      if (y < b.minY) b.minY = y;
      if (y > b.maxY) b.maxY = y;
    }
  if (b.count) { b.cx = sx / b.count; b.cy = sy / b.count; }
  return b;
}

static Blob measureRgb(const std::vector<uint32_t>& pixels, int width, int height, uint32_t rgb) {
  return measure(pixels, width, height, [rgb](uint32_t p) { return p == rgb; });
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
  void info(const char* name, const char* fmt, ...) {
    char detail[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);
    std::printf("INFO %s: %s\n", name, detail);
    std::fflush(stdout);
  }
};

struct Probe {
  IDirect3DDevice9* device = nullptr;
  IDirect3DSurface9* backbuffer = nullptr;
  IDirect3DSurface9* depth = nullptr;
  IDirect3DSurface9* offscreen = nullptr;
  IDirect3DSurface9* readbackMain = nullptr;
  IDirect3DSurface9* readbackOff = nullptr;
  IDirect3DVertexDeclaration9* decl = nullptr;
  IDirect3DVertexShader9* vs = nullptr;
  IDirect3DPixelShader9* ps = nullptr;

  std::vector<uint32_t> read(IDirect3DSurface9* target, IDirect3DSurface9* sysmem, int width, int height) {
    check(device->GetRenderTargetData(target, sysmem), "GetRenderTargetData");
    D3DLOCKED_RECT locked{};
    check(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect");
    std::vector<uint32_t> pixels(size_t(width) * height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        uint32_t v;
        std::memcpy(&v, static_cast<const char*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
        pixels[size_t(y) * width + x] = v & 0xFFFFFFu;
      }
    check(sysmem->UnlockRect(), "UnlockRect");
    return pixels;
  }

  void triangle(const float (&tri)[3][2], float depth, float w, int width, int height, uint32_t argb) {
    const Vertex v[3] = {
      clipVertex(tri[0][0], tri[0][1], depth, w, width, height, argb),
      clipVertex(tri[1][0], tri[1][1], depth, w, width, height, argb),
      clipVertex(tri[2][0], tri[2][1], depth, w, width, height, argb) };
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vertex)), "DrawPrimitiveUP(triangle)");
  }

  // A w == 1 quad in back-buffer pixels; never shifted.
  void quad(float x0, float y0, float x1, float y1, float depth, uint32_t argb) {
    const Vertex v[4] = {
      clipVertex(x0, y0, depth, 1.0f, kSize, kSize, argb), clipVertex(x1, y0, depth, 1.0f, kSize, kSize, argb),
      clipVertex(x0, y1, depth, 1.0f, kSize, kSize, argb), clipVertex(x1, y1, depth, 1.0f, kSize, kSize, argb) };
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex)), "DrawPrimitiveUP(quad)");
  }

  // Back buffer, no depth: the screen-space quad (green) and the perspective triangle (red, w = 2).
  void mainFrame(Blob& tri, Blob& screenQuad) {
    check(device->SetRenderTarget(0, backbuffer), "SetRenderTarget(backbuffer)");
    check(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kClear, 1.0f, 0), "Clear");
    check(device->BeginScene(), "BeginScene");
    quad(kQuadX0, kQuadY0, kQuadX1, kQuadY1, 0.5f, 0xFF000000u | kGreen);
    triangle(kTri, 0.5f, kW, kSize, kSize, 0xFF000000u | kRed);
    check(device->EndScene(), "EndScene");
    const auto pixels = read(backbuffer, readbackMain, kSize, kSize);
    tri = measureRgb(pixels, kSize, kSize, kRed);
    screenQuad = measureRgb(pixels, kSize, kSize, kGreen);
  }

  // 64x128 render target (SetRenderTarget makes the viewport 64x128), no depth buffer.
  Blob offscreenFrame() {
    check(device->SetRenderTarget(0, offscreen), "SetRenderTarget(offscreen)");
    check(device->SetDepthStencilSurface(nullptr), "SetDepthStencilSurface(null)");
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kClear, 1.0f, 0), "Clear(offscreen)");
    check(device->BeginScene(), "BeginScene");
    triangle(kOffTri, 0.5f, kW, kOffW, kOffH, 0xFF000000u | kRed);
    check(device->EndScene(), "EndScene");
    const auto pixels = read(offscreen, readbackOff, kOffW, kOffH);
    check(device->SetRenderTarget(0, backbuffer), "SetRenderTarget(backbuffer)");
    check(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
    return measureRgb(pixels, kOffW, kOffH, kRed);
  }

  // Draws the triangle (blue, depth test LESSEQUAL against a 1.0 clear, depth writes on) with clip z = depth*w,
  // then brackets the stored depth with two unshifted (w == 1) full-screen probes, depth writes off and ZFUNC
  // LESS: the first at `lo` writes red only, so red means stored > lo; the second at `hi` writes green only, so
  // green means stored > hi. A triangle pixel is in range (lo, hi] when red is 255 and green is 0.
  struct DepthResult { Blob tri; int tooNear = 0, tooFar = 0; };
  DepthResult depthFrame(float ndcDepth, float lo, float hi) {
    check(device->SetRenderTarget(0, backbuffer), "SetRenderTarget(backbuffer)");
    check(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "ZENABLE");
    check(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
    check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "ZFUNC");
    check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF), "COLORWRITEENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000u, 1.0f, 0),
        "Clear(depth)");
    check(device->BeginScene(), "BeginScene");
    triangle(kTri, ndcDepth, kW, kSize, kSize, 0xFF0000FFu);
    check(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE), "ZWRITEENABLE");
    check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS), "ZFUNC");
    check(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED), "COLORWRITEENABLE(red)");
    quad(-1.0f, -1.0f, kSize + 1.0f, kSize + 1.0f, lo, 0xFFFFFFFFu);
    check(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN), "COLORWRITEENABLE(green)");
    quad(-1.0f, -1.0f, kSize + 1.0f, kSize + 1.0f, hi, 0xFFFFFFFFu);
    check(device->EndScene(), "EndScene");
    check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF), "COLORWRITEENABLE");
    check(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
    check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "ZFUNC");
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    const auto pixels = read(backbuffer, readbackMain, kSize, kSize);
    DepthResult r;
    auto isTri = [](uint32_t p) { return (p & 0xFFu) == 0xFFu; };
    r.tri = measure(pixels, kSize, kSize, isTri);
    for (uint32_t p : pixels) {
      if (!isTri(p)) continue;
      r.tooNear += ((p >> 16) & 0xFFu) != 0xFFu;  // stored <= lo
      r.tooFar  += ((p >> 8) & 0xFFu) != 0x00u;   // stored > hi
    }
    return r;
  }
};

static int run(bool passesAll) {
  // Both variables are cached by DXVK at the first shader translation / draw; set them before any D3D9 call.
  ::setenv("LB_STEREO_EYE_UNITS", "1", 1);
  if (passesAll) ::setenv("LB_STEREO_PASSES", "all", 1);
  else ::unsetenv("LB_STEREO_PASSES");
  SDL_setenv("DXVK_CONFIG_FILE", SKYRIM_GRAPHICS_CONFIG_PATH, 1);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
  if (SDL_Vulkan_LoadLibrary(SKYRIM_MOLTENVK_LIBRARY_PATH) != 0)
    throw std::runtime_error(SDL_GetError());
  SDL_Window* window = SDL_CreateWindow("Skyrim stereo shift probe", SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, kSize, kSize, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  if (!window) throw std::runtime_error(SDL_GetError());
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d || !d3d->GetAdapterCount()) throw std::runtime_error("No D3D9 adapter");
  D3DADAPTER_IDENTIFIER9 adapter{};
  check(d3d->GetAdapterIdentifier(0, 0, &adapter), "GetAdapterIdentifier");
  check(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
      D3DRTYPE_SURFACE, D3DFMT_D24S8), "CheckDeviceFormat(D24S8 surface)");
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
  check(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &p.device), "CreateDevice");
  IDirect3DDevice9* device = p.device;

  Results r;
  std::printf("INFO setup: LB_STEREO_EYE_UNITS=1 LB_STEREO_PASSES=%s, back buffer %dx%d, off-screen %dx%d, "
      "w=%g, k=%g\n", passesAll ? "all" : "(unset)", kSize, kSize, kOffW, kOffH, kW, kK);
  r.expect(dxvk_stereo_enabled() == 1, "stereo_enabled", "dxvk_stereo_enabled() = %d", dxvk_stereo_enabled());

  check(device->GetRenderTarget(0, &p.backbuffer), "GetRenderTarget");
  check(device->GetDepthStencilSurface(&p.depth), "GetDepthStencilSurface");
  check(device->CreateRenderTarget(kOffW, kOffH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
      &p.offscreen, nullptr), "CreateRenderTarget(offscreen)");
  check(device->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
      &p.readbackMain, nullptr), "CreateOffscreenPlainSurface(main)");
  check(device->CreateOffscreenPlainSurface(kOffW, kOffH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
      &p.readbackOff, nullptr), "CreateOffscreenPlainSurface(offscreen)");

  const D3DVERTEXELEMENT9 elements[] = {
    { 0,  0, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    D3DDECL_END() };
  check(device->CreateVertexDeclaration(elements, &p.decl), "CreateVertexDeclaration");
  check(device->CreateVertexShader(kVertexShader, &p.vs), "CreateVertexShader");
  check(device->CreatePixelShader(kPixelShader, &p.ps), "CreatePixelShader");
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "CULLMODE");
  check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "LIGHTING");
  check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
  check(device->SetVertexDeclaration(p.decl), "SetVertexDeclaration");
  check(device->SetVertexShader(p.vs), "SetVertexShader");
  check(device->SetPixelShader(p.ps), "SetPixelShader");

  double triCx, triCy, offCx, offCy;
  centroidOf(kTri, triCx, triCy);
  centroidOf(kOffTri, offCx, offCy);
  const double shiftPx = double(kK) * kSize / (2.0 * kW);     // 8: |dx| = |dy| for +-k on the back buffer
  const double offShiftPx = double(kK) * kOffW / (2.0 * kW);  // 4: |dx| in the 64x128 target

  // 1 and 2: back buffer, programmable VS.
  auto quadOk = [](const Blob& q) {
    return q.count == (kQuadMaxX - kQuadMinX + 1) * (kQuadMaxY - kQuadMinY + 1) && q.minX == kQuadMinX &&
        q.maxX == kQuadMaxX && q.minY == kQuadMinY && q.maxY == kQuadMaxY;
  };
  auto quadCheck = [&](const char* name, const Blob& q) {
    r.expect(quadOk(q), name, "%d px, x %d..%d, y %d..%d (expect %d px, x %d..%d, y %d..%d)",
        q.count, q.minX, q.maxX, q.minY, q.maxY,
        (kQuadMaxX - kQuadMinX + 1) * (kQuadMaxY - kQuadMinY + 1), kQuadMinX, kQuadMaxX, kQuadMinY, kQuadMaxY);
  };

  Blob rest, restQuad;
  dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
  p.mainFrame(rest, restQuad);
  r.expect(rest.count >= 1000 && std::fabs(rest.cx - triCx) <= 1.0 && std::fabs(rest.cy - triCy) <= 1.0,
      "perspective_rest", "%d px, centroid (%.2f, %.2f), expect >= 1000 px near (%.2f, %.2f) +-1",
      rest.count, rest.cx, rest.cy, triCx, triCy);
  quadCheck("screen_quad_rest", restQuad);

  struct ShiftCase { const char* name; const char* quadName; float ox, oy; bool viaShift; double dx, dy; };
  const ShiftCase cases[] = {
    { "perspective_plus_k",  "screen_quad_plus_k",  +kK, 0.0f, true,  -shiftPx, 0.0 },
    { "perspective_minus_k", "screen_quad_minus_k", -kK, 0.0f, true,  +shiftPx, 0.0 },
    { "perspective_offset_y", "screen_quad_offset_y", 0.0f, +kK, false, 0.0, +shiftPx },
  };
  for (const ShiftCase& c : cases) {
    if (c.viaShift) dxvk_stereo_set_clip_shift(c.ox);
    else dxvk_stereo_set_clip_offset(c.ox, c.oy, 0.0f, 0.0f);
    Blob tri, q;
    p.mainFrame(tri, q);
    const double dx = tri.cx - rest.cx, dy = tri.cy - rest.cy;
    const bool ok = tri.count > 0 && std::abs(tri.count - rest.count) <= rest.count / 50 &&
        std::fabs(dx - c.dx) <= 0.25 && std::fabs(dy - c.dy) <= 0.25;
    r.expect(ok, c.name, "offset (%g, %g, 0, 0) via %s: %d px, moved (%+.2f, %+.2f), expect (%+.2f, %+.2f) +-0.25",
        c.ox, c.oy, c.viaShift ? "set_clip_shift" : "set_clip_offset", tri.count, dx, dy, c.dx, c.dy);
    quadCheck(c.quadName, q);
  }

  // 3: a viewport without the back buffer's aspect ratio.
  dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
  const Blob offRest = p.offscreenFrame();
  r.expect(offRest.count >= 500 && std::fabs(offRest.cx - offCx) <= 1.0 && std::fabs(offRest.cy - offCy) <= 1.0,
      "offscreen_rest", "%dx%d target: %d px, centroid (%.2f, %.2f), expect >= 500 px near (%.2f, %.2f) +-1",
      kOffW, kOffH, offRest.count, offRest.cx, offRest.cy, offCx, offCy);
  dxvk_stereo_set_clip_shift(kK);
  const Blob offShifted = p.offscreenFrame();
  {
    const double expectDx = passesAll ? -offShiftPx : 0.0;
    const double dx = offShifted.cx - offRest.cx, dy = offShifted.cy - offRest.cy;
    const bool ok = offShifted.count > 0 && std::abs(offShifted.count - offRest.count) <= offRest.count / 50 &&
        std::fabs(dx - expectDx) <= 0.25 && std::fabs(dy) <= 0.25;
    r.expect(ok, passesAll ? "offscreen_shifted_passes_all" : "offscreen_unshifted_by_default",
        "%dx%d target, k = %g: %d px, moved (%+.2f, %+.2f), expect (%+.2f, +0.00) +-0.25", kOffW, kOffH, kK,
        offShifted.count, dx, dy, expectDx);
  }

  // 4: depth. Each case sets its offset, draws at clip z = depth * w (w = 2) and brackets the stored depth.
  struct DepthCase {
    const char* name; float ox, oy, oz, ow; float depth;
  };
  const DepthCase depthCases[] = {
    // set_clip_shift touches x only: z and w are unchanged, so the depth z/w is too.
    { "depth_shift_keeps_z",          kK, 0.0f, 0.0f,  0.0f, 0.5f },
    // Sky at z == w stays exactly on the far plane (and moves sideways: w = 2 is perspective).
    { "depth_shift_sky_far_plane",    kK, 0.0f, 0.0f,  0.0f, 1.0f },
    // A full offset subtracts z and w too: depth (z - oz)/(w - ow) = (1 + 2)/(2 + 2) = 0.75, and the triangle
    // halves in size about the centre (w' = 4; powers of two keep the GPU's divide exact).
    { "depth_offset_zw",               0.0f, 0.0f, -2.0f, -2.0f, 0.5f },
    // oz == ow keeps z == w: (2 + 2)/(2 + 2) = 1, still the far plane.
    { "depth_offset_zw_sky_far_plane", 0.0f, 0.0f, -2.0f, -2.0f, 1.0f },
  };
  const double triArea = 0.5 * std::fabs((kTri[1][0] - kTri[0][0]) * (kTri[2][1] - kTri[0][1]) -
                                         (kTri[2][0] - kTri[0][0]) * (kTri[1][1] - kTri[0][1]));
  for (const DepthCase& c : depthCases) {
    const double z = double(c.depth) * kW, w = kW;
    const double wNew = w - c.ow;
    const double expectDepth = (z - c.oz) / wNew;
    const int minCount = int(0.8 * triArea * (w / wNew) * (w / wNew));
    // Pixel position after the offset: px' = (x - ox)/w' mapped to pixels, i.e. scale about the centre by
    // w/w' and add -ox*W/(2w'), +oy*H/(2w').
    const double half = kSize / 2.0;
    const double expectCx = half + (triCx - half) * (w / wNew) - double(c.ox) * kSize / (2.0 * wNew);
    const double expectCy = half + (triCy - half) * (w / wNew) + double(c.oy) * kSize / (2.0 * wNew);
    const float lo = float(expectDepth) - kDepthTol;
    const float hi = std::fmin(float(expectDepth) + kDepthTol, 1.0f);
    dxvk_stereo_set_clip_offset(c.ox, c.oy, c.oz, c.ow);
    const Probe::DepthResult d = p.depthFrame(c.depth, lo, hi);
    const bool ok = d.tri.count >= minCount && d.tooNear == 0 && d.tooFar == 0 &&
        std::fabs(d.tri.cx - expectCx) <= 1.0 && std::fabs(d.tri.cy - expectCy) <= 1.0;
    r.expect(ok, c.name, "offset (%g, %g, %g, %g), clip z %g w %g: %d px (expect >= %d), %d px depth <= %.4f, "
        "%d px depth > %.4f (expect depth %.4f); centroid (%.2f, %.2f), expect (%.2f, %.2f) +-1",
        c.ox, c.oy, c.oz, c.ow, z, w, d.tri.count, minCount, d.tooNear, lo, d.tooFar, hi, expectDepth,
        d.tri.cx, d.tri.cy, expectCx, expectCy);
  }
  {
    // Not asserted: oz != ow pushes a sky vertex past the far plane (z' > w'), and DXVK draws D3D9 with depth
    // clipping on, so it is expected to vanish. Later stereo work (eye translations with tz != 0) must cope.
    const float oz = 0.25f, ow = 0.5f;
    dxvk_stereo_set_clip_offset(0.0f, 0.0f, oz, ow);
    const Probe::DepthResult d = p.depthFrame(1.0f, 1.0f - kDepthTol, 1.0f);
    r.info("depth_offset_sky_oz_ne_ow", "offset (0, 0, %g, %g) on z == w = %g: z'/w' = %.4f > 1; %d px drawn "
        "(0 = clipped by the far plane)", oz, ow, kW, (kW - oz) / (kW - ow), d.tri.count);
  }

  // 5: the fixed-function vertex pipeline. Projection 2*I gives clip (2x, 2y, 2z, 2): w = 2 as above.
  dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
  check(device->SetVertexShader(nullptr), "SetVertexShader(null)");
  check(device->SetPixelShader(nullptr), "SetPixelShader(null)");
  check(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE), "SetFVF");
  D3DMATRIX identity{}, projection{};
  for (int i = 0; i < 4; ++i) { identity.m[i][i] = 1.0f; projection.m[i][i] = kW; }
  check(device->SetTransform(D3DTS_WORLD, &identity), "SetTransform(WORLD)");
  check(device->SetTransform(D3DTS_VIEW, &identity), "SetTransform(VIEW)");
  check(device->SetTransform(D3DTS_PROJECTION, &projection), "SetTransform(PROJECTION)");
  check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP");
  check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "COLORARG1");
  check(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), "ALPHAOP");
  check(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "ALPHAARG1");
  check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "COLOROP(1)");
  auto ffFrame = [&]() {
    FFVertex v[3];
    for (int i = 0; i < 3; ++i)
      v[i] = { 2.0f * kTri[i][0] / kSize - 1.0f, 1.0f - 2.0f * kTri[i][1] / kSize, 0.5f, 0xFF000000u | kRed };
    check(device->SetRenderTarget(0, p.backbuffer), "SetRenderTarget(backbuffer)");
    check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000u | kClear, 1.0f, 0), "Clear");
    check(device->BeginScene(), "BeginScene");
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(FFVertex)), "DrawPrimitiveUP(fixed function)");
    check(device->EndScene(), "EndScene");
    return measureRgb(p.read(p.backbuffer, p.readbackMain, kSize, kSize), kSize, kSize, kRed);
  };
  const Blob ffRest = ffFrame();
  dxvk_stereo_set_clip_shift(kK);
  const Blob ffShifted = ffFrame();
  dxvk_stereo_set_clip_offset(0.0f, 0.0f, 0.0f, 0.0f);
  // Drawing nothing would make the INFO line meaningless, so that much is checked.
  r.expect(ffRest.count >= 1000 && ffShifted.count >= 1000, "fixed_function_drawn",
      "%d px at rest, %d px with k = %g", ffRest.count, ffShifted.count, kK);
  {
    const double dx = ffShifted.cx - ffRest.cx;
    if (std::fabs(dx) <= 0.25)
      r.info("fixed_function_shift", "not shifted (known gap): moved %+.2f px with k = %g; a programmable VS "
          "moves %+.2f", dx, kK, -shiftPx);
    else
      r.info("fixed_function_shift", "moved %+.2f px with k = %g (programmable VS: %+.2f); the known gap may be "
          "closed, update this test", dx, kK, -shiftPx);
  }

  const bool pass = r.failed == 0;
  std::printf("{\"probe\":\"d3d9_stereo_shift\",\"adapter\":\"%s\",\"passes\":\"%s\",\"checks\":%d,\"failed\":%d,"
      "\"failed_checks\":[%s],\"pass\":%s}\n", adapter.Description, passesAll ? "all" : "default", r.checks,
      r.failed, r.failedNames.c_str(), pass ? "true" : "false");
  std::printf("%s\n", pass ? "PASS stereo_shift_smoke" : "FAIL stereo_shift_smoke");
  std::fflush(stdout);

  p.ps->Release(); p.vs->Release(); p.decl->Release();
  p.readbackOff->Release(); p.readbackMain->Release(); p.offscreen->Release();
  p.depth->Release(); p.backbuffer->Release();
  device->Release(); d3d->Release();
  SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary(); SDL_Quit();
  return pass ? 0 : 2;
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--passes=default") == 0) return run(false);
    if (argc == 2 && std::strcmp(argv[1], "--passes=all") == 0) return run(true);
    std::fprintf(stderr, "usage: stereo_shift_smoke --passes=default | --passes=all\n");
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stereo_shift_smoke: %s\n", e.what());
    SDL_Quit();
    return 1;
  }
}
