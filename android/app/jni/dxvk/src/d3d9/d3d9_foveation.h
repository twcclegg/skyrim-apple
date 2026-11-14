#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../dxvk/dxvk_include.h"

/* Foveating the game's own render: see include/native/skyrim_foveation.h and docs/handoff/foveated-render.md.
 * Everything here is inert unless LB_GAME_FOVEATION=1 and a provider (or LB_GAME_FOVEATION_EMULATE) gives a
 * physical size. */

namespace dxvk {

  /* How a draw into a foveated target is rasterized. */
  enum class D3D9FoveationMode : uint32_t {
    None     = 0,   // target is not foveated
    Mapped   = 1,   // scene geometry: logical viewport, rate map on the render pass
    Unmapped = 2,   // full-screen quad reading warped textures texel for texel: no map, viewport scaled
  };

  struct D3D9FoveationConfig {
    bool       enabled  = false;
    bool       log      = false;
    VkExtent2D screen   = { 0, 0 };   // the game's back-buffer size
    VkExtent2D physical = { 0, 0 };   // what back-buffer-sized targets are really created at
    void*      rateMap  = nullptr;    // id<MTLRasterizationRateMap>; null = uniform scale (emulation)

    bool Matches(uint32_t width, uint32_t height) const {
      return enabled && width == screen.width && height == screen.height;
    }

    float ScaleX() const { return float(physical.width)  / float(screen.width);  }
    float ScaleY() const { return float(physical.height) / float(screen.height); }

    /* Logical -> physical by plain proportion. Exact for whole-target rectangles and for the emulation;
     * for part of a mapped target it is only where a uniform scale would put it. */
    int32_t DownX(int32_t x) const { return int32_t(std::floor(float(x) * ScaleX())); }
    int32_t DownY(int32_t y) const { return int32_t(std::floor(float(y) * ScaleY())); }
    int32_t UpX(int32_t x)   const { return std::min(int32_t(std::ceil(float(x) * ScaleX())), int32_t(physical.width));  }
    int32_t UpY(int32_t y)   const { return std::min(int32_t(std::ceil(float(y) * ScaleY())), int32_t(physical.height)); }
  };

  /* Written when the swap chain's back buffers are (re)created, on the thread that owns the device; read on
   * that thread only. The command-stream thread gets what it needs by value. */
  inline D3D9FoveationConfig g_foveation;

  /* Per-frame counts, logged with LB_GAME_FOVEATION_LOG=1 (d3d9_swapchain.cpp). */
  struct D3D9FoveationStats {
    uint32_t mapped        = 0;   // draws into foveated targets through the map
    uint32_t unmapped      = 0;   // full-screen quads run in warped space
    uint32_t hazards       = 0;   // scene geometry sampling a warped texture (wrong texels until resolved)
    uint32_t tainted       = 0;   // quads that carried warped content into an ordinary target
    uint32_t mixedTargets  = 0;   // framebuffers mixing a foveated and an ordinary attachment
    uint32_t partialClears = 0;   // clears of part of a foveated target
    uint32_t partialCopies = 0;   // StretchRect of part of a foveated surface
    uint32_t modeSwitches  = 0;   // mapped <-> unmapped changes (each restarts the render pass)
  };

  inline D3D9FoveationStats g_foveationStats;

  /* Asks the provider (or the emulation switch) what to do for this back-buffer size. */
  void D3D9ConfigureFoveation(uint32_t width, uint32_t height, bool multisampled);

  /* Logs and resets the counts once per presented frame when asked to. */
  void D3D9FoveationEndFrame();

}
