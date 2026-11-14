// Foveating the game's own render (docs/handoff/foveated-render.md): the plumbing in DXVK, checked without the
// game and without a rate map. LB_GAME_FOVEATION_EMULATE shrinks back-buffer-sized targets by a plain factor,
// which goes through the same code as a real map except for the map itself:
//   - the back buffer, the automatic depth buffer and a back-buffer-sized render-target texture get the small
//     image while the game is told the size it asked for;
//   - geometry (depth test on) lands where the scale puts it;
//   - a full-screen quad (depth test off) that samples the small texture copies it texel for texel;
//   - GetRenderTargetData returns the small picture in the top-left corner of a surface of the game's size.
//   - the frame's counts say the two geometry draws went the mapped way and the quad the unmapped way.
// Run with --off for the same scene with the switch off (the picture then fills the surface).
//
// Built by macos/graphics/CMakeLists.txt next to graphics_smoke; on Linux, against a DXVK built from
// desktop/CMakeLists.txt:
//   clang++ -std=gnu++17 -DDXVK_NATIVE -DDXVK_WSI_SDL2 -I android/app/jni/dxvk/include/native/directx
//     -I android/app/jni/dxvk/include/native/windows -I android/app/jni/dxvk/include/native
//     macos/graphics/foveation_smoke.cpp -o foveation_smoke -L <build>/dxvk -ldxvk_d3d9 -lSDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <d3d9.h>
#include <wsi/native_wsi.h>
#include <skyrim_foveation.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifndef SKYRIM_MOLTENVK_LIBRARY_PATH
#define SKYRIM_MOLTENVK_LIBRARY_PATH nullptr
#endif

static void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    char detail[160];
    std::snprintf(detail, sizeof detail, "%s failed: 0x%08x", what, unsigned(hr));
    throw std::runtime_error(detail);
  }
}

static const int W = 256, H = 128;
static const uint32_t RED = 0xff0000u, GREEN = 0x00ff00u, BLUE = 0x0000ffu, YELLOW = 0xffff00u;

struct Solid { float x, y, z, rhw; DWORD color; };
struct Textured { float x, y, z, rhw, u, v; };

// Two triangles over the game-pixel rectangle [x0, x1) x [y0, y1).
static void solid_rect(IDirect3DDevice9* device, float x0, float y0, float x1, float y1, DWORD color) {
  const Solid v[] = {{x0 - 0.5f, y0 - 0.5f, 0.5f, 1, color}, {x1 - 0.5f, y0 - 0.5f, 0.5f, 1, color},
      {x0 - 0.5f, y1 - 0.5f, 0.5f, 1, color}, {x1 - 0.5f, y1 - 0.5f, 0.5f, 1, color}};
  check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF");
  check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Solid)), "DrawPrimitiveUP");
}

int main(int argc, char** argv) {
  const bool off = argc > 1 && !std::strcmp(argv[1], "--off");
  try {
#ifdef SKYRIM_GRAPHICS_CONFIG_PATH
    SDL_setenv("DXVK_CONFIG_FILE", SKYRIM_GRAPHICS_CONFIG_PATH, 1);
#endif
    SDL_setenv("LB_GAME_FOVEATION", off ? "0" : "1", 1);
    SDL_setenv("LB_GAME_FOVEATION_EMULATE", "0.5", 0);
    SDL_setenv("LB_GAME_FOVEATION_LOG", "1", 0);
    const float scale = off ? 1.0f : float(std::atof(SDL_getenv("LB_GAME_FOVEATION_EMULATE")));
    if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    if (SDL_Vulkan_LoadLibrary(SKYRIM_MOLTENVK_LIBRARY_PATH) != 0) throw std::runtime_error(SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("Skyrim foveation probe", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) throw std::runtime_error(SDL_GetError());
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || !d3d->GetAdapterCount()) throw std::runtime_error("No D3D9 adapter");
    D3DADAPTER_IDENTIFIER9 adapter{};
    check(d3d->GetAdapterIdentifier(0, 0, &adapter), "GetAdapterIdentifier");
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dxvk::wsi::toHwnd(window);
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    check(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device), "CreateDevice");

    IDirect3DSurface9 *backBuffer = nullptr, *readback = nullptr, *sceneSurface = nullptr;
    IDirect3DTexture9* scene = nullptr;
    check(device->GetRenderTarget(0, &backBuffer), "GetRenderTarget");
    D3DSURFACE_DESC desc{};
    check(backBuffer->GetDesc(&desc), "GetDesc");
    check(device->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr),
        "CreateOffscreenPlainSurface");
    check(device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scene, nullptr),
        "CreateTexture");
    check(scene->GetSurfaceLevel(0, &sceneSurface), "GetSurfaceLevel");

    // Reads the back buffer and compares the colour at four game-pixel positions; with the switch on they are
    // looked up where the scale puts them.
    struct Sample { int x, y; uint32_t want; };
    bool pass = desc.Width == unsigned(W) && desc.Height == unsigned(H);
    auto verify = [&](const char* what, const Sample* samples, int count) {
      check(device->GetRenderTargetData(backBuffer, readback), "GetRenderTargetData");
      D3DLOCKED_RECT locked{};
      check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect");
      for (int i = 0; i < count; ++i) {
        const int x = int(samples[i].x * scale), y = int(samples[i].y * scale);
        uint32_t p;
        std::memcpy(&p, static_cast<const char*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
        p &= 0x00ffffffu;
        const bool ok = p == samples[i].want;
        pass = pass && ok;
        std::printf("%s: game pixel (%d,%d) read at (%d,%d) = %06x, want %06x%s\n", what, samples[i].x,
            samples[i].y, x, y, p, samples[i].want, ok ? "" : "  MISMATCH");
      }
      check(readback->UnlockRect(), "UnlockRect");
    };

    check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "CULLMODE");
    check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "LIGHTING");
    check(device->BeginScene(), "BeginScene");

    // 1. Geometry into the back buffer: green over the middle, on red.
    check(device->SetRenderState(D3DRS_ZENABLE, TRUE), "ZENABLE");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0), "Clear");
    solid_rect(device, 64, 32, 192, 96, D3DCOLOR_XRGB(0, 255, 0));
    const Sample geometry[] = {{128, 64, GREEN}, {72, 40, GREEN}, {16, 16, RED}, {220, 100, RED}};
    verify("geometry", geometry, 4);

    // 2. Geometry into a back-buffer-sized texture: yellow over the top-left quarter, on blue.
    check(device->SetRenderTarget(0, sceneSurface), "SetRenderTarget(texture)");
    check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0), "Clear");
    solid_rect(device, 0, 0, 128, 64, D3DCOLOR_XRGB(255, 255, 0));

    // 3. A full-screen quad copies that texture to the back buffer, as a post-processing pass would.
    check(device->SetRenderTarget(0, backBuffer), "SetRenderTarget(back buffer)");
    check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "ZENABLE");
    check(device->SetTexture(0, scene), "SetTexture");
    check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP");
    check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "COLORARG1");
    check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "MINFILTER");
    check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "MAGFILTER");
    const Textured quad[] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {W - 0.5f, -0.5f, 0, 1, 1, 0},
        {-0.5f, H - 0.5f, 0, 1, 0, 1}, {W - 0.5f, H - 0.5f, 0, 1, 1, 1}};
    check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "SetFVF");
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Textured)), "DrawPrimitiveUP");
    check(device->SetTexture(0, nullptr), "SetTexture");
    const Sample copied[] = {{32, 16, YELLOW}, {120, 56, YELLOW}, {140, 72, BLUE}, {240, 120, BLUE}};
    verify("quad", copied, 4);

    check(device->EndScene(), "EndScene");
    check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");

    // How the three draws were sorted: two as geometry, the quad as a pass in warped space.
    skyrim_foveation_counts counts{};
    const bool on = skyrim_foveation_last_frame(&counts) != 0;
    const bool sorted = off ? !on : on && counts.mapped == 2 && counts.unmapped == 1 && counts.hazards == 0;
    pass = pass && sorted;
    std::printf("counts: on %d, mapped %u, unmapped %u, hazards %u, tainted %u, mixed targets %u, mode switches %u%s\n",
        int(on), counts.mapped, counts.unmapped, counts.hazards, counts.tainted, counts.mixed_targets,
        counts.mode_switches, sorted ? "" : "  MISMATCH");

    std::printf("{\"probe\":\"d3d9_foveation_plumbing\",\"adapter\":\"%s\",\"scale\":%.3f,\"pass\":%s}\n",
        adapter.Description, scale, pass ? "true" : "false");
    sceneSurface->Release(); scene->Release(); readback->Release(); backBuffer->Release();
    device->Release(); d3d->Release();
    SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary(); SDL_Quit();
    return pass ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "foveation_smoke: %s\n", e.what());
    SDL_Quit();
    return 1;
  }
}
