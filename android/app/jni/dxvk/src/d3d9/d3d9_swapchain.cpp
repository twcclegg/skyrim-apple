#include "d3d9_swapchain.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "d3d9_surface.h"
#include "d3d9_monitor.h"

#include "d3d9_hud.h"
#include "d3d9_foveation.h"

#include <mutex>

#include "skyrim_foveation.h"

namespace {
  std::atomic<skyrim_foveation_provider_fn> g_foveationProviderFn   = { nullptr };
  std::atomic<void*>                        g_foveationProviderUser = { nullptr };
  std::mutex                                g_foveationCurrentMutex;
  skyrim_foveation_map                      g_foveationCurrent      = { };
  bool                                      g_foveationCurrentOn    = false;
  skyrim_foveation_counts                   g_foveationLastFrame    = { };
}

#if defined(__APPLE__)
#include "skyrim_present_tap.h"

namespace {
  std::atomic<skyrim_present_tap_fn> g_presentTapFn   = { nullptr };
  std::atomic<void*>                 g_presentTapUser = { nullptr };
  std::atomic<bool>                  g_presentTapExclusive = { false };
  std::atomic<IDirect3DSurface9*>    g_presentTapDepth = { nullptr };
}
#endif

namespace dxvk {

#ifndef DXVK_NATIVE
  struct D3D9WindowData {
    bool unicode;
    bool filter;
    WNDPROC proc;
    D3D9SwapChainEx* swapchain;
  };


  static dxvk::recursive_mutex g_windowProcMapMutex;
  static std::unordered_map<HWND, D3D9WindowData> g_windowProcMap;


  template <typename T, typename J, typename ... Args>
  auto CallCharsetFunction(T unicode, J ascii, bool isUnicode, Args... args) {
    return isUnicode
      ? unicode(args...)
      : ascii  (args...);
  }


  class D3D9WindowMessageFilter {

  public:

    D3D9WindowMessageFilter(HWND window, bool filter = true)
      : m_window(window) {
      std::lock_guard lock(g_windowProcMapMutex);
      auto it = g_windowProcMap.find(m_window);
      m_filter = std::exchange(it->second.filter, filter);
    }

    ~D3D9WindowMessageFilter() {
      std::lock_guard lock(g_windowProcMapMutex);
      auto it = g_windowProcMap.find(m_window);
      it->second.filter = m_filter;
    }

    D3D9WindowMessageFilter             (const D3D9WindowMessageFilter&) = delete;
    D3D9WindowMessageFilter& operator = (const D3D9WindowMessageFilter&) = delete;

  private:

    HWND m_window;
    bool m_filter;

  };


  LRESULT CALLBACK D3D9WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);


  void ResetWindowProc(HWND window) {
    std::lock_guard lock(g_windowProcMapMutex);

    auto it = g_windowProcMap.find(window);
    if (it == g_windowProcMap.end())
      return;

    auto proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      GetWindowLongPtrW, GetWindowLongPtrA, it->second.unicode,
        window, GWLP_WNDPROC));


    if (proc == D3D9WindowProc)
      CallCharsetFunction(
        SetWindowLongPtrW, SetWindowLongPtrA, it->second.unicode,
          window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.proc));

    g_windowProcMap.erase(window);
  }


  void HookWindowProc(HWND window, D3D9SwapChainEx* swapchain) {
    std::lock_guard lock(g_windowProcMapMutex);

    ResetWindowProc(window);

    D3D9WindowData windowData;
    windowData.unicode = IsWindowUnicode(window);
    windowData.filter  = false;
    windowData.proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      SetWindowLongPtrW, SetWindowLongPtrA, windowData.unicode,
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D9WindowProc)));
    windowData.swapchain = swapchain;

    g_windowProcMap[window] = std::move(windowData);
  }


  LRESULT CALLBACK D3D9WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCALCSIZE && wparam == TRUE)
      return 0;

    D3D9WindowData windowData = {};

    {
      std::lock_guard lock(g_windowProcMapMutex);

      auto it = g_windowProcMap.find(window);
      if (it != g_windowProcMap.end())
        windowData = it->second;
    }

    bool unicode = windowData.proc
      ? windowData.unicode
      : IsWindowUnicode(window);

    if (!windowData.proc || windowData.filter)
      return CallCharsetFunction(
        DefWindowProcW, DefWindowProcA, unicode,
          window, message, wparam, lparam);

    if (message == WM_DESTROY)
      ResetWindowProc(window);
    else if (message == WM_ACTIVATEAPP) {
      D3DDEVICE_CREATION_PARAMETERS create_parms;
      windowData.swapchain->GetDevice()->GetCreationParameters(&create_parms);

      if (!(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {
        if (wparam) {
          // Heroes of Might and Magic V needs this to resume drawing after a focus loss
          D3DPRESENT_PARAMETERS params;
          RECT rect;

          GetMonitorRect(GetDefaultMonitor(), &rect);
          windowData.swapchain->GetPresentParameters(&params);
          SetWindowPos(window, nullptr, rect.left, rect.top, params.BackBufferWidth, params.BackBufferHeight,
                       SWP_NOACTIVATE | SWP_NOZORDER);
        }
        else {
          if (IsWindowVisible(window))
            ShowWindow(window, SW_MINIMIZE);
        }
      }
    }

    return CallCharsetFunction(
      CallWindowProcW, CallWindowProcA, unicode,
        windowData.proc, window, message, wparam, lparam);
  }
#endif


  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  struct D3D9PresentInfo {
    float scale[2];
    float offset[2];
  };


  static inline void ConvertDisplayMode(const D3DDISPLAYMODEEX& mode, wsi::WsiMode* wsiMode) {
    wsiMode->width        = mode.Width;
    wsiMode->height       = mode.Height;
    wsiMode->refreshRate  = wsi::WsiRational{ mode.RefreshRate, 1 };
    wsiMode->bitsPerPixel = GetMonitorFormatBpp(EnumerateFormat(mode.Format));
    wsiMode->interlaced   = false;
  }


  static inline void ConvertDisplayMode(const wsi::WsiMode& devMode, D3DDISPLAYMODEEX* pMode) {
      pMode->Size             = sizeof(D3DDISPLAYMODEEX);
      pMode->Width            = devMode.width;
      pMode->Height           = devMode.height;
      pMode->RefreshRate      = devMode.refreshRate.numerator / devMode.refreshRate.denominator;
      pMode->Format           = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  }


  D3D9SwapChainEx::D3D9SwapChainEx(
          D3D9DeviceEx*          pDevice,
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode)
    : D3D9SwapChainExBase(pDevice)
    , m_device           (pDevice->GetDXVKDevice())
    , m_context          (m_device->createContext())
    , m_frameLatencyCap  (pDevice->GetOptions()->maxFrameLatency)
    , m_frameLatencySignal(new sync::Fence(m_frameId))
    , m_dialog           (pDevice->GetOptions()->enableDialogMode) {
    this->NormalizePresentParameters(pPresentParams);
    m_presentParams = *pPresentParams;
    m_window = m_presentParams.hDeviceWindow;

    UpdatePresentRegion(nullptr, nullptr);
    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();

    CreateBackBuffers(m_presentParams.BackBufferCount);
    CreateBlitter();
    CreateHud();

    InitRamp();

#if defined(__APPLE__)
    m_presentTap = env::getEnvVar("LB_PRESENT_TAP") == "1";
    if (m_presentTap)
      Logger::info("D3D9SwapChainEx: present tap enabled (LB_PRESENT_TAP=1)");
#endif

    // Apply initial window mode and fullscreen state
    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {
    DestroyBackBuffers();
#ifndef DXVK_NATIVE
    ResetWindowProc(m_window);
#endif

    wsi::restoreDisplayMode(m_monitor);
    NotifyDisplayRefreshRate(0.0);

    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DSwapChain9)
     || (GetParent()->IsExtended() && riid == __uuidof(IDirect3DSwapChain9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D9SwapChainEx::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD    dwFlags) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    D3D9FoveationEndFrame();

#ifdef __APPLE__
    // Skyrim Apple port: GPU load for the device log every ~10 s, from DXVK's own GPU idle counter (the time its
    // submission queue waited on fences with nothing else to do), as the HUD's gpuload item computes it.
    {
      static auto     lastTime  = dxvk::high_resolution_clock::now();
      static uint64_t lastIdle  = 0;
      static uint32_t frames    = 0;
      frames++;
      auto now = dxvk::high_resolution_clock::now();
      uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - lastTime).count();
      static uint64_t every_us = 0;
      if (!every_us) {
        const char* e = std::getenv("LB_POWER_LOG_S");
        every_us = e && std::atof(e) > 0 ? uint64_t(std::atof(e) * 1e6) : 10'000'000;
      }
      if (us >= every_us) {
        uint64_t idle = m_device->getStatCounters().getCtr(DxvkStatCounter::GpuIdleTicks);
        uint64_t idleDiff = idle - lastIdle;
        double busy = us > idleDiff ? double(us - idleDiff) / double(us) : 0.0;
        if (lastIdle)
          std::fprintf(stderr, "dxvk: gpu busy %.0f%%, %.1f ms per frame over %u frames (%.0f fps)\n",
            busy * 100.0, busy * double(us) / 1000.0 / frames, frames, frames * 1e6 / double(us));
        lastIdle = idle;
        lastTime = now;
        frames = 0;
      }
    }
#endif

    uint32_t presentInterval = m_presentParams.PresentationInterval;

    // This is not true directly in d3d9 to to timing differences that don't matter for us.
    // For our purposes...
    // D3DPRESENT_INTERVAL_DEFAULT (0) == D3DPRESENT_INTERVAL_ONE (1) which means VSYNC.
    presentInterval = std::max(presentInterval, 1u);

    if (presentInterval == D3DPRESENT_INTERVAL_IMMEDIATE || (dwFlags & D3DPRESENT_FORCEIMMEDIATE))
      presentInterval = 0;

    auto options = m_parent->GetOptions();

    if (options->presentInterval >= 0)
      presentInterval = options->presentInterval;

    bool vsync  = presentInterval != 0;

    HWND window = m_presentParams.hDeviceWindow;
    if (hDestWindowOverride != nullptr)
      window    = hDestWindowOverride;

    bool recreate = false;
    recreate   |= m_presenter == nullptr;
    recreate   |= window != m_window;    
    recreate   |= m_dialog != m_lastDialog;

    m_window    = window;

    m_dirty    |= vsync != m_vsync;
    m_dirty    |= UpdatePresentRegion(pSourceRect, pDestRect);
    m_dirty    |= recreate;
    m_dirty    |= m_presenter != nullptr &&
                 !m_presenter->hasSwapChain();

    m_vsync     = vsync;

    m_lastDialog = m_dialog;

#if defined(__APPLE__)
    // Tap-only: the WSI swapchain is left alone (m_dirty stays set and is handled on the first normal present).
    m_presentTapOnly = PresentTapExclusive();
    if (m_presentTapOnly != m_presentTapOnlyLogged) {
      m_presentTapOnlyLogged = m_presentTapOnly;
      Logger::info(m_presentTapOnly ? "D3D9SwapChainEx: tap-only presents (immersive)" : "D3D9SwapChainEx: WSI presents resumed");
    }
    if (m_presentTapOnly) {
      try {
        PresentImage(0);
        return D3D_OK;
      } catch (const DxvkError& e) {
        Logger::err(e.message());
        return D3DERR_DEVICEREMOVED;
      }
    }
#endif

    try {
      if (recreate)
        CreatePresenter();

      if (std::exchange(m_dirty, false))
        RecreateSwapChain(vsync);

      // We aren't going to device loss simply because
      // 99% of D3D9 games don't handle this properly and
      // just end up crashing (like with alt-tab loss)
      if (!m_presenter->hasSwapChain())
        return D3D_OK;

      PresentImage(presentInterval);
      return D3D_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_DEVICEREMOVED;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetFrontBufferData(IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // This function can do absolutely everything!
    // Copies the front buffer between formats with an implicit resolve.
    // Oh, and the dest is systemmem...
    // This is a slow function anyway, it waits for the copy to finish.
    // so there's no reason to not just make and throwaway temp images.

    // If extent of dst > src, then we blit to a subrect of the size
    // of src onto a temp image of dst's extents,
    // then copy buffer back to dst (given dst is subresource)

    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);

    if (unlikely(dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTexInfo = dst->GetCommonTexture();
    D3D9CommonTexture* srcTexInfo = m_backBuffers.back()->GetCommonTexture();

    if (unlikely(dstTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM))
      return D3DERR_INVALIDCALL;

    Rc<DxvkBuffer> dstBuffer = dstTexInfo->GetBuffer(dst->GetSubresource());
    Rc<DxvkImage>  srcImage  = srcTexInfo->GetImage();

    if (srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      DxvkImageCreateInfo resolveInfo;
      resolveInfo.type          = VK_IMAGE_TYPE_2D;
      resolveInfo.format        = srcImage->info().format;
      resolveInfo.flags         = 0;
      resolveInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      resolveInfo.extent        = srcImage->info().extent;
      resolveInfo.numLayers     = 1;
      resolveInfo.mipLevels     = 1;
      resolveInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      resolveInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                | VK_PIPELINE_STAGE_TRANSFER_BIT;
      resolveInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                | VK_ACCESS_TRANSFER_WRITE_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      resolveInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      resolveInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      Rc<DxvkImage> resolvedSrc = m_device->createImage(
        resolveInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      m_parent->EmitCs([
        cDstImage = resolvedSrc,
        cSrcImage = srcImage
      ] (DxvkContext* ctx) {
        VkImageSubresourceLayers resolveSubresource;
        resolveSubresource.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveSubresource.mipLevel        = 0;
        resolveSubresource.baseArrayLayer  = 0;
        resolveSubresource.layerCount      = 1;

        VkImageResolve resolveRegion;
        resolveRegion.srcSubresource = resolveSubresource;
        resolveRegion.srcOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.dstSubresource = resolveSubresource;
        resolveRegion.dstOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.extent         = cSrcImage->info().extent;

        ctx->resolveImage(
          cDstImage, cSrcImage,
          resolveRegion, VK_FORMAT_UNDEFINED);
      });

      srcImage = std::move(resolvedSrc);
    }

    D3D9Format srcFormat = srcTexInfo->Desc()->Format;
    D3D9Format dstFormat = dstTexInfo->Desc()->Format;

    bool similar = AreFormatsSimilar(srcFormat, dstFormat);

    if (!similar || srcImage->info().extent != dstTexInfo->GetExtent()) {
      DxvkImageCreateInfo blitCreateInfo;
      blitCreateInfo.type          = VK_IMAGE_TYPE_2D;
      blitCreateInfo.format        = dstTexInfo->GetFormatMapping().FormatColor;
      blitCreateInfo.flags         = 0;
      blitCreateInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      blitCreateInfo.extent        = dstTexInfo->GetExtent();
      blitCreateInfo.numLayers     = 1;
      blitCreateInfo.mipLevels     = 1;
      blitCreateInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                   | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      blitCreateInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                   | VK_PIPELINE_STAGE_TRANSFER_BIT;
      blitCreateInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                   | VK_ACCESS_TRANSFER_WRITE_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      blitCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      blitCreateInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      Rc<DxvkImage> blittedSrc = m_device->createImage(
        blitCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(blittedSrc->info().format);
      const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);

      const VkImageSubresource dstSubresource = dstTexInfo->GetSubresourceFromIndex(dstFormatInfo->aspectMask, 0);
      const VkImageSubresource srcSubresource = srcTexInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask, 0);

      VkImageSubresourceLayers dstSubresourceLayers = {
        dstSubresource.aspectMask,
        dstSubresource.mipLevel,
        dstSubresource.arrayLayer, 1 };

      VkImageSubresourceLayers srcSubresourceLayers = {
        srcSubresource.aspectMask,
        srcSubresource.mipLevel,
        srcSubresource.arrayLayer, 1 };

      VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);

      // Blit to a subrect of the src extents
      VkImageBlit blitInfo;
      blitInfo.dstSubresource = dstSubresourceLayers;
      blitInfo.srcSubresource = srcSubresourceLayers;
      blitInfo.dstOffsets[0] = VkOffset3D{ 0, 0, 0 };
      blitInfo.dstOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),  int32_t(srcExtent.height),  1 };
      blitInfo.srcOffsets[0] = VkOffset3D{ 0, 0, 0 };
      blitInfo.srcOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),  int32_t(srcExtent.height),  1 };

      m_parent->EmitCs([
        cDstImage = blittedSrc,
        cDstMap   = dstTexInfo->GetMapping().Swizzle,
        cSrcImage = srcImage,
        cSrcMap   = srcTexInfo->GetMapping().Swizzle,
        cBlitInfo = blitInfo
      ] (DxvkContext* ctx) {
        ctx->blitImage(
          cDstImage, cDstMap,
          cSrcImage, cSrcMap,
          cBlitInfo, VK_FILTER_NEAREST);
      });

      srcImage = std::move(blittedSrc);
    }

    const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);
    const VkImageSubresource srcSubresource = srcTexInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask, 0);
    VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };
    VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);

    m_parent->EmitCs([
      cBuffer       = dstBuffer,
      cImage        = srcImage,
      cSubresources = srcSubresourceLayers,
      cLevelExtent  = srcExtent
    ] (DxvkContext* ctx) {
      ctx->copyImageToBuffer(cBuffer, 0, 4, 0,
        cImage, cSubresources, VkOffset3D { 0, 0, 0 },
        cLevelExtent);
    });
    
    dstTexInfo->SetWrittenByGPU(dst->GetSubresource(), true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetBackBuffer(
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    // Could be doing a device reset...
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(ppBackBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(iBackBuffer >= m_presentParams.BackBufferCount)) {
      Logger::err(str::format("D3D9: GetBackBuffer: Invalid back buffer index: ", iBackBuffer));
      return D3DERR_INVALIDCALL;
    }

    *ppBackBuffer = ref(m_backBuffers[iBackBuffer].ptr());
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
    // We could use D3DKMTGetScanLine but Wine doesn't implement that.
    // So... we lie here and make some stuff up
    // enough that it makes games work.

    // Assume there's 20 lines in a vBlank.
    constexpr uint32_t vBlankLineCount = 20;

    if (pRasterStatus == nullptr)
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    if (FAILED(this->GetDisplayModeEx(&mode, nullptr)))
      return D3DERR_INVALIDCALL;

    uint32_t scanLineCount = mode.Height + vBlankLineCount;

    auto nowUs = std::chrono::time_point_cast<std::chrono::microseconds>(
      dxvk::high_resolution_clock::now())
      .time_since_epoch();

    auto frametimeUs = std::chrono::microseconds(1000000u / mode.RefreshRate);
    auto scanLineUs  = frametimeUs / scanLineCount;

    pRasterStatus->ScanLine = (nowUs % frametimeUs) / scanLineUs;
    pRasterStatus->InVBlank = pRasterStatus->ScanLine >= mode.Height;

    if (pRasterStatus->InVBlank)
      pRasterStatus->ScanLine = 0;

    return D3D_OK;
  }

  
  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayMode(D3DDISPLAYMODE* pMode) {
    if (pMode == nullptr)
      return D3DERR_INVALIDCALL;

    *pMode = D3DDISPLAYMODE();

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    HRESULT hr = this->GetDisplayModeEx(&mode, nullptr);

    if (FAILED(hr))
      return hr;

    pMode->Width       = mode.Width;
    pMode->Height      = mode.Height;
    pMode->Format      = mode.Format;
    pMode->RefreshRate = mode.RefreshRate;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    *pPresentationParameters = m_presentParams;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetLastPresentCount(UINT* pLastPresentCount) {
    Logger::warn("D3D9SwapChainEx::GetLastPresentCount: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics) {
    Logger::warn("D3D9SwapChainEx::GetPresentStats: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    if (pMode == nullptr && pRotation == nullptr)
      return D3DERR_INVALIDCALL;

    if (pRotation != nullptr)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;

    if (pMode != nullptr) {
      wsi::WsiMode devMode = { };

      if (!wsi::getCurrentDisplayMode(GetDefaultMonitor(), &devMode)) {
        Logger::err("D3D9SwapChainEx::GetDisplayModeEx: Failed to enum display settings");
        return D3DERR_INVALIDCALL;
      }

      ConvertDisplayMode(devMode, pMode);
    }

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::Reset(
          D3DPRESENT_PARAMETERS* pPresentParams,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    this->SynchronizePresent();
    this->NormalizePresentParameters(pPresentParams);

    m_dirty    |= m_presentParams.BackBufferFormat   != pPresentParams->BackBufferFormat
               || m_presentParams.BackBufferCount    != pPresentParams->BackBufferCount;

    bool changeFullscreen = m_presentParams.Windowed != pPresentParams->Windowed;

    if (pPresentParams->Windowed) {
      if (changeFullscreen)
        this->LeaveFullscreenMode();

      // Port: the platform layer owns the (fullscreen) window; a smaller back
      // buffer is scaled up to it on present (resolution scale), not resized.
    }
    else {
      if (changeFullscreen) {
        if (FAILED(this->EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
          return D3DERR_INVALIDCALL;
      }

#ifdef _WIN32
      D3D9WindowMessageFilter filter(m_window);
#endif

      if (!changeFullscreen) {
        if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode, false)))
          return D3DERR_INVALIDCALL;
      }
    }

    m_presentParams = *pPresentParams;

    if (changeFullscreen)
      SetGammaRamp(0, &m_ramp);

    CreateBackBuffers(m_presentParams.BackBufferCount);

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::WaitForVBlank() {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::WaitForVBlank: Stub");

    return D3D_OK;
  }

  static bool validateGammaRamp(const WORD (&ramp)[256]) {
    if (ramp[0] >= ramp[std::size(ramp) - 1]) {
      Logger::err("validateGammaRamp: ramp inverted or flat");
      return false;
    }

    for (size_t i = 1; i < std::size(ramp); i++) {
      if (ramp[i] < ramp[i - 1]) {
        Logger::err("validateGammaRamp: ramp not monotonically increasing");
        return false;
      }
      if (ramp[i] - ramp[i - 1] >= UINT16_MAX / 2) {
        Logger::err("validateGammaRamp: huuuge jump");
        return false;
      }
    }

    return true;
  }


  void    D3D9SwapChainEx::SetGammaRamp(
            DWORD         Flags,
      const D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(pRamp == nullptr))
      return;

    if (unlikely(!validateGammaRamp(pRamp->red)
              && !validateGammaRamp(pRamp->blue)
              && !validateGammaRamp(pRamp->green)))
      return;

    m_ramp = *pRamp;

    bool isIdentity = true;

    std::array<DxvkGammaCp, NumControlPoints> cp;
      
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

      cp[i].r = pRamp->red[i];
      cp[i].g = pRamp->green[i];
      cp[i].b = pRamp->blue[i];
      cp[i].a = 0;

      isIdentity &= cp[i].r == identity
                 && cp[i].g == identity
                 && cp[i].b == identity;
    }

    if (!isIdentity && !m_presentParams.Windowed)
      m_blitter->setGammaRamp(NumControlPoints, cp.data());
    else
      m_blitter->setGammaRamp(0, nullptr);
  }


  void    D3D9SwapChainEx::GetGammaRamp(D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (likely(pRamp != nullptr))
      *pRamp = m_ramp;
  }


  void    D3D9SwapChainEx::Invalidate(HWND hWindow) {
    if (hWindow == nullptr)
      hWindow = m_parent->GetWindow();

    if (m_presentParams.hDeviceWindow == hWindow) {
      m_presenter = nullptr;

      m_device->waitForSubmission(&m_presentStatus);
      m_device->waitForIdle();
    }
  }


  HRESULT D3D9SwapChainEx::SetDialogBoxMode(bool bEnableDialogs) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // https://docs.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setdialogboxmode
    // The MSDN documentation says this will error out under many weird conditions.
    // However it doesn't appear to error at all in any of my tests of these
    // cases described in the documentation.

    m_dialog = bEnableDialogs;

    return D3D_OK;
  }


  D3D9Surface* D3D9SwapChainEx::GetBackBuffer(UINT iBackBuffer) {
    if (iBackBuffer >= m_presentParams.BackBufferCount)
      return nullptr;

    return m_backBuffers[iBackBuffer].ptr();
  }


  void D3D9SwapChainEx::NormalizePresentParameters(D3DPRESENT_PARAMETERS* pPresentParams) {
    if (pPresentParams->hDeviceWindow == nullptr)
      pPresentParams->hDeviceWindow    = m_parent->GetWindow();

    pPresentParams->BackBufferCount    = std::max(pPresentParams->BackBufferCount, 1u);

    const int32_t forcedMSAA = m_parent->GetOptions()->forceSwapchainMSAA;
    if (forcedMSAA != -1) {
      pPresentParams->MultiSampleType    = D3DMULTISAMPLE_TYPE(forcedMSAA);
      pPresentParams->MultiSampleQuality = 0;
    }

    if (pPresentParams->Windowed) {
      GetWindowClientSize(pPresentParams->hDeviceWindow,
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }
    else {
      GetMonitorClientSize(GetDefaultMonitor(),
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }

    if (pPresentParams->BackBufferFormat == D3DFMT_UNKNOWN)
      pPresentParams->BackBufferFormat = D3DFMT_X8R8G8B8;

    if (env::getEnvVar("DXVK_FORCE_WINDOWED") == "1")
      pPresentParams->Windowed         = TRUE;
  }


  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
    m_parent->Flush();

    // Retrieve the image and image view to present
    auto swapImage = m_backBuffers[0]->GetCommonTexture()->GetImage();
    auto swapImageView = m_backBuffers[0]->GetImageView(false);

    // Bump our frame id.
    ++m_frameId;

    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
#if defined(__APPLE__)
      if (m_presentTapOnly) {
        // No WSI acquire, blit or present. Same copy-then-signal order as below, so the frame fence covers the copy.
        m_context->beginRecording(m_device->createCommandList());
        int32_t tapSlot = RecordPresentTap(swapImage);
        m_context->signal(m_frameLatencySignal, m_frameId);
        SubmitTapOnly();
        if (tapSlot >= 0)
          NotifyPresentTap(uint32_t(tapSlot));
        break;
      }
#endif
      SynchronizePresent();

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);

      while (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) {
        RecreateSwapChain(m_vsync);
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);
      }

      m_context->beginRecording(
        m_device->createCommandList());

#if defined(__APPLE__)
      // Copy before the WSI blit and the frame-latency signal, so the frame fence covers the copy.
      int32_t tapSlot = (m_presentTap && i == 0) ? RecordPresentTap(swapImage) : -1;
#endif

      VkRect2D srcRect = {
        {  int32_t(m_srcRect.left),                    int32_t(m_srcRect.top)                    },
        { uint32_t(m_srcRect.right - m_srcRect.left), uint32_t(m_srcRect.bottom - m_srcRect.top) } };

      // Foveated game render: the back buffer's image is smaller than the
      // game's back buffer. A window shows the warped picture stretched;
      // undoing the map is the immersive compositor's job.
      if (unlikely(m_backBuffers[0]->GetCommonTexture()->IsFoveated())) {
        const int32_t x1 = g_foveation.UpX(m_srcRect.right);
        const int32_t y1 = g_foveation.UpY(m_srcRect.bottom);
        srcRect.offset = { g_foveation.DownX(m_srcRect.left), g_foveation.DownY(m_srcRect.top) };
        srcRect.extent = { uint32_t(std::max(x1 - srcRect.offset.x, 1)), uint32_t(std::max(y1 - srcRect.offset.y, 1)) };
      }

      VkRect2D dstRect = {
        {  int32_t(m_dstRect.left),                    int32_t(m_dstRect.top)                    },
        { uint32_t(m_dstRect.right - m_dstRect.left), uint32_t(m_dstRect.bottom - m_dstRect.top) } };

      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), dstRect,
        swapImageView, srcRect);

      if (m_hud != nullptr)
        m_hud->render(m_context, info.format, info.imageExtent);

      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      SubmitPresent(sync, i);

#if defined(__APPLE__)
      if (tapSlot >= 0)
        NotifyPresentTap(uint32_t(tapSlot));
#endif
    }

    SyncFrameLatency();

    // Rotate swap chain buffers so that the back
    // buffer at index 0 becomes the front buffer.
    for (uint32_t i = 1; i < m_backBuffers.size(); i++)
      m_backBuffers[i]->Swap(m_backBuffers[i - 1].ptr());

    m_parent->m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);
  }


#if defined(__APPLE__)
  int32_t D3D9SwapChainEx::RecordPresentTap(const Rc<DxvkImage>& Source) {
    // No consumer: never mark slots busy, so nothing can leak.
    if (g_presentTapFn.load(std::memory_order_acquire) == nullptr)
      return -1;

    const DxvkImageCreateInfo& src = Source->info();

    if (src.sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("D3D9SwapChainEx: present tap skipped, multisampled back buffer"); }
      return -1;
    }

    // (Re)create the slots when the back buffer changes. A new state object keeps the old images alive until the
    // consumer has released every frame it still holds from them.
    bool recreate = m_presentTapState == nullptr;
    if (!recreate) {
      const DxvkImageCreateInfo& tap = m_presentTapState->images[0]->info();
      recreate = tap.format != src.format || tap.extent.width != src.extent.width
              || tap.extent.height != src.extent.height;
    }

    if (recreate) {
      Rc<D3D9PresentTapState> state = new D3D9PresentTapState();
      state->fence = m_frameLatencySignal;

      DxvkImageCreateInfo info = { };
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = src.format;
      info.flags       = 0;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { src.extent.width, src.extent.height, 1u };
      info.numLayers   = 1;
      info.mipLevels   = 1;
      info.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access      = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;

      for (uint32_t s = 0; s < D3D9PresentTapState::Slots; s++)
        state->images[s] = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      m_presentTapState = state;
      m_presentTapNext  = 0;
    }

    for (uint32_t n = 0; n < D3D9PresentTapState::Slots; n++) {
      uint32_t slot = (m_presentTapNext + n) % D3D9PresentTapState::Slots;
      bool expected = false;

      if (!m_presentTapState->busy[slot].compare_exchange_strong(expected, true))
        continue;

      m_presentTapNext = (slot + 1) % D3D9PresentTapState::Slots;

      VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      m_context->copyImage(
        m_presentTapState->images[slot], layers, VkOffset3D { 0, 0, 0 },
        Source,                          layers, VkOffset3D { 0, 0, 0 },
        VkExtent3D { src.extent.width, src.extent.height, 1u });
      RecordPresentTapDepth(slot);
      return int32_t(slot);
    }

    // The consumer still holds every slot: skip the tap this frame rather than stall the game.
    return -1;
  }


  void D3D9SwapChainEx::RecordPresentTapDepth(uint32_t Slot) {
    D3D9PresentTapState* state = m_presentTapState.ptr();
    state->hasDepth[Slot] = false;

    // Only a single-sample, single-channel float colour target the size of the tap image: the game's own
    // scene depth (runtime/hud_screen.cpp). Anything else is ignored rather than copied.
    IDirect3DSurface9* named = g_presentTapDepth.load(std::memory_order_acquire);
    if (named == nullptr)
      return;
    auto* surface = static_cast<D3D9Surface*>(named);
    D3D9CommonTexture* texture = surface->GetCommonTexture();
    if (texture == nullptr)
      return;
    Rc<DxvkImage> depth = texture->GetImage();
    if (depth == nullptr)
      return;
    const DxvkImageCreateInfo& src = depth->info();
    const DxvkImageCreateInfo& color = state->images[Slot]->info();
    if (src.format != VK_FORMAT_R32_SFLOAT || src.sampleCount != VK_SAMPLE_COUNT_1_BIT
     || src.extent.width != color.extent.width || src.extent.height != color.extent.height)
      return;

    if (state->depthImages[Slot] == nullptr) {
      DxvkImageCreateInfo info = { };
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = VK_FORMAT_R32_SFLOAT;
      info.flags       = 0;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { src.extent.width, src.extent.height, 1u };
      info.numLayers   = 1;
      info.mipLevels   = 1;
      info.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access      = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;
      state->depthImages[Slot] = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    m_context->copyImage(
      state->depthImages[Slot], layers, VkOffset3D { 0, 0, 0 },
      depth,                    layers, VkOffset3D { 0, 0, 0 },
      VkExtent3D { src.extent.width, src.extent.height, 1u });
    state->hasDepth[Slot] = true;
  }


  void D3D9SwapChainEx::NotifyPresentTap(uint32_t Slot) {
    D3D9PresentTapState* state = m_presentTapState.ptr();
    skyrim_present_tap_fn fn = g_presentTapFn.load(std::memory_order_acquire);

    if (fn == nullptr) {
      // Callback cleared between record and notify: the copy is harmless, just free the slot.
      state->busy[Slot].store(false, std::memory_order_release);
      return;
    }

    const DxvkImageCreateInfo& info = state->images[Slot]->info();

    skyrim_present_tap_frame frame = { };
    frame.vk_device = reinterpret_cast<void*>(m_device->handle());
    frame.vk_queue  = reinterpret_cast<void*>(m_device->queues().graphics.queueHandle);
    frame.vk_image  = uint64_t(reinterpret_cast<uintptr_t>(state->images[Slot]->handle()));
    frame.slot      = Slot;
    frame.width     = info.extent.width;
    frame.height    = info.extent.height;
    frame.vk_format = uint32_t(info.format);
    frame.frame_id  = m_frameId;
    frame.state     = state;
    if (state->hasDepth[Slot]) {
      const DxvkImageCreateInfo& depth = state->depthImages[Slot]->info();
      frame.depth_vk_image  = uint64_t(reinterpret_cast<uintptr_t>(state->depthImages[Slot]->handle()));
      frame.depth_width     = depth.extent.width;
      frame.depth_height    = depth.extent.height;
      frame.depth_vk_format = uint32_t(depth.format);
    }

    state->incRef(); // dropped by skyrim_present_tap_release
    fn(g_presentTapUser.load(std::memory_order_acquire), &frame);
  }


  bool D3D9SwapChainEx::PresentTapExclusive() const {
    // All three are required: the env knob, a registered consumer, and the runtime's immersive flag.
    return m_presentTap
        && g_presentTapExclusive.load(std::memory_order_acquire)
        && g_presentTapFn.load(std::memory_order_acquire) != nullptr;
  }


  void D3D9SwapChainEx::SubmitTapOnly() {
    // Like SubmitPresent without the WSI semaphores and presentImage. m_presentStatus is untouched, so the next
    // normal present still synchronizes against the last real WSI present.
    m_parent->EmitCs([this,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      m_device->submitCommandList(cCommandList, VK_NULL_HANDLE, VK_NULL_HANDLE);
    });

    m_parent->FlushCsChunk();
  }
#endif


  void D3D9SwapChainEx::SubmitPresent(const vk::PresenterSync& Sync, uint32_t FrameId) {
    // Present from CS thread so that we don't
    // have to synchronize with it first.
    m_presentStatus.result = VK_NOT_READY;

    m_parent->EmitCs([this,
      cFrameId     = FrameId,
      cSync        = Sync,
      cHud         = m_hud,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      if (cHud != nullptr && !cFrameId)
        cHud->update();

      m_device->presentImage(m_presenter, &m_presentStatus);
    });

    m_parent->FlushCsChunk();
  }


  void D3D9SwapChainEx::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);

    // Port: Android reports SUBOPTIMAL for every present on a rotated display
    // (the compositor applies the rotation); the swap chain is still usable.
    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      RecreateSwapChain(m_vsync);
  }


  void D3D9SwapChainEx::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D9SwapChainEx: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
  }


  void D3D9SwapChainEx::CreatePresenter() {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);

    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D9SwapChainEx::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device->vkd(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D9SwapChainEx::DestroyBackBuffers() {
    for (auto& backBuffer : m_backBuffers)
      backBuffer->ClearContainer();

    m_backBuffers.clear();
  }


  void D3D9SwapChainEx::CreateBackBuffers(uint32_t NumBackBuffers) {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    DestroyBackBuffers();

    int NumFrontBuffer = m_parent->GetOptions()->noExplicitFrontBuffer ? 0 : 1;
    m_backBuffers.resize(NumBackBuffers + NumFrontBuffer);

    // Create new back buffer
    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_presentParams.BackBufferWidth,  1u);
    desc.Height             = std::max(m_presentParams.BackBufferHeight, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = EnumerateFormat(m_presentParams.BackBufferFormat);
    desc.MultiSample        = m_presentParams.MultiSampleType;
    desc.MultisampleQuality = m_presentParams.MultiSampleQuality;
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Usage              = D3DUSAGE_RENDERTARGET;
    desc.Discard            = FALSE;
    desc.IsBackBuffer       = TRUE;
    desc.IsAttachmentOnly   = FALSE;

    // Foveated game render: decided here, per back-buffer size, before the
    // back buffers and everything the game creates at their size.
    m_parent->SuspendFoveationMap();
    D3D9ConfigureFoveation(desc.Width, desc.Height, desc.MultiSample != D3DMULTISAMPLE_NONE);

    for (uint32_t i = 0; i < m_backBuffers.size(); i++)
      m_backBuffers[i] = new D3D9Surface(m_parent, &desc, this);

    auto swapImage = m_backBuffers[0]->GetCommonTexture()->GetImage();

    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    for (uint32_t i = 0; i < m_backBuffers.size(); i++) {
      m_context->clearColorImage(
        m_backBuffers[i]->GetCommonTexture()->GetImage(),
        clearColor, subresources);
    }

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D9SwapChainEx::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);
  }


  void D3D9SwapChainEx::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr) {
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
      m_hud->addItem<hud::HudSamplerCount>("samplers", -1, m_parent);
    }
  }


  void D3D9SwapChainEx::InitRamp() {
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      DWORD identity = DWORD(MapGammaControlPoint(float(i) / float(NumControlPoints - 1)));

      m_ramp.red[i]   = identity;
      m_ramp.green[i] = identity;
      m_ramp.blue[i]  = identity;
    }
  }


  void D3D9SwapChainEx::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());
  }


  uint32_t D3D9SwapChainEx::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_parent->GetFrameLatency();

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_presentParams.BackBufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D9SwapChainEx::PickFormats(
          D3D9Format                Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D9SwapChainEx: Unexpected format: ", Format));
        
      case D3D9Format::A8R8G8B8:
      case D3D9Format::X8R8G8B8:
      case D3D9Format::A8B8G8R8:
      case D3D9Format::X8B8G8R8: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::A2R10G10B10:
      case D3D9Format::A2B10G10R10: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::X1R5G5B5:
      case D3D9Format::A1R5G5B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }

      case D3D9Format::R5G6B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G6R5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G6B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }
    }

    return n;
  }


  uint32_t D3D9SwapChainEx::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D9SwapChainEx::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  void D3D9SwapChainEx::NotifyDisplayRefreshRate(
          double                  RefreshRate) {
    m_displayRefreshRate = RefreshRate;

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(RefreshRate);
  }


  HRESULT D3D9SwapChainEx::EnterFullscreenMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode, true))) {
      Logger::err("D3D9: EnterFullscreenMode: Failed to change display mode");
      return D3DERR_NOTAVAILABLE;
    }

#ifndef DXVK_NATIVE
    // Testing shows we shouldn't hook WM_NCCALCSIZE but we shouldn't change
    // windows style either.
    //
    // Some games restore window styles after we have changed it, so hooking is
    // also required. Doing it will allow us to create fullscreen windows
    // regardless of their style and it also appears to work on Windows.
    HookWindowProc(m_window, this);

    D3D9WindowMessageFilter filter(m_window);
#endif
    
    m_monitor = GetDefaultMonitor();

    if (!wsi::enterFullscreenMode(m_monitor, m_window, &m_windowState, true)) {
        Logger::err("D3D9: EnterFullscreenMode: Failed to enter fullscreen mode");
        return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::LeaveFullscreenMode() {    
    if (!wsi::restoreDisplayMode(m_monitor))
      Logger::warn("D3D9: LeaveFullscreenMode: Failed to restore display mode");
    NotifyDisplayRefreshRate(0.0);
    
    m_monitor = nullptr;

#ifndef DXVK_NATIVE
    ResetWindowProc(m_window);
#endif
    
    if (!wsi::isWindow(m_window))
      return D3D_OK;

    if (!wsi::leaveFullscreenMode(m_window, &m_windowState)) {
      Logger::err("D3D9: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return D3DERR_NOTAVAILABLE;
    }
    
    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::ChangeDisplayMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          bool                   EnteringFullscreen) {
    D3DDISPLAYMODEEX mode;

    if (pFullscreenDisplayMode) {
      mode = *pFullscreenDisplayMode;
    } else {
      mode.Width            = pPresentParams->BackBufferWidth;
      mode.Height           = pPresentParams->BackBufferHeight;
      mode.Format           = pPresentParams->BackBufferFormat;
      mode.RefreshRate      = pPresentParams->FullScreen_RefreshRateInHz;
      mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      mode.Size             = sizeof(D3DDISPLAYMODEEX);
    }

    wsi::WsiMode wsiMode = { };
    ConvertDisplayMode(mode, &wsiMode);

    if (!wsi::setWindowMode(GetDefaultMonitor(), m_window, &wsiMode, EnteringFullscreen))
      return D3DERR_NOTAVAILABLE;

    return D3D_OK;
  }


  bool    D3D9SwapChainEx::UpdatePresentRegion(const RECT* pSourceRect, const RECT* pDestRect) {
    if (pSourceRect == nullptr) {
      m_srcRect.top    = 0;
      m_srcRect.left   = 0;
      m_srcRect.right  = m_presentParams.BackBufferWidth;
      m_srcRect.bottom = m_presentParams.BackBufferHeight;
    }
    else
      m_srcRect = *pSourceRect;

    RECT dstRect;
    if (pDestRect == nullptr) {
      // TODO: Should we hook WM_SIZE message for this?
      UINT width, height;
      GetWindowClientSize(m_window, &width, &height);

      dstRect.top    = 0;
      dstRect.left   = 0;
      dstRect.right  = LONG(width);
      dstRect.bottom = LONG(height);
    }
    else
      dstRect = *pDestRect;

    bool recreate = 
       m_dstRect.left   != dstRect.left
    || m_dstRect.top    != dstRect.top
    || m_dstRect.right  != dstRect.right
    || m_dstRect.bottom != dstRect.bottom;

    m_dstRect = dstRect;

    return recreate;
  }

  VkExtent2D D3D9SwapChainEx::GetPresentExtent() {
    return VkExtent2D {
      std::max<uint32_t>(m_dstRect.right  - m_dstRect.left, 1u),
      std::max<uint32_t>(m_dstRect.bottom - m_dstRect.top,  1u) };
  }


  VkFullScreenExclusiveEXT D3D9SwapChainEx::PickFullscreenMode() {
    return m_dialog
      ? VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
  }


  std::string D3D9SwapChainEx::GetApiName() {
    return this->GetParent()->IsExtended() ? "D3D9Ex" : "D3D9";
  }


  void D3D9ConfigureFoveation(uint32_t width, uint32_t height, bool multisampled) {
    D3D9FoveationConfig config;
    skyrim_foveation_map map = { width, height, 0, 0, nullptr };

    if (env::getEnvVar("LB_GAME_FOVEATION") == "1") {
      config.log = env::getEnvVar("LB_GAME_FOVEATION_LOG") == "1";

      skyrim_foveation_provider_fn fn = g_foveationProviderFn.load(std::memory_order_acquire);
      bool have = false;

      if (multisampled) {
        Logger::warn("Foveation: multisampled back buffer, left off (rate maps with MSAA are untried)");
      } else if (fn != nullptr && fn(g_foveationProviderUser.load(std::memory_order_acquire), &map) != 0) {
        have = true;
      } else {
        // No map: a plain uniform scale, to exercise the plumbing where
        // there is no Metal (LB_GAME_FOVEATION_EMULATE=0.25..1)
        std::string emulate = env::getEnvVar("LB_GAME_FOVEATION_EMULATE");
        float scale = emulate.empty() ? 0.0f : std::strtof(emulate.c_str(), nullptr);

        if (scale >= 0.25f && scale <= 1.0f) {
          map.physical_width  = std::max<uint32_t>(uint32_t(float(width)  * scale + 0.5f), 1u);
          map.physical_height = std::max<uint32_t>(uint32_t(float(height) * scale + 0.5f), 1u);
          map.mtl_rate_map    = nullptr;
          have = true;
        }
      }

      if (have && map.physical_width != 0 && map.physical_height != 0) {
        config.enabled  = true;
        config.screen   = { width, height };
        config.physical = { map.physical_width, map.physical_height };
        config.rateMap  = map.mtl_rate_map;
      } else if (!multisampled) {
        Logger::warn("Foveation: LB_GAME_FOVEATION=1 but no map provider and no LB_GAME_FOVEATION_EMULATE, left off");
      }
    }

    g_foveation      = config;
    g_foveationStats = { };

    { std::lock_guard<std::mutex> lock(g_foveationCurrentMutex);
      g_foveationCurrent   = map;
      g_foveationCurrentOn = config.enabled; }

    if (config.enabled) {
      Logger::info(str::format("Foveation: game ", width, "x", height, " rendered at ",
        config.physical.width, "x", config.physical.height,
        config.rateMap != nullptr ? " through a rate map" : " (uniform scale, no map)"));
    }
  }


  void D3D9FoveationEndFrame() {
    if (likely(!g_foveation.enabled))
      return;

    static uint32_t s_frame = 0;

    if (g_foveation.log && (s_frame % 60) == 0) {
      const D3D9FoveationStats& st = g_foveationStats;
      Logger::info(str::format("Foveation frame ", s_frame,
        ": mapped ", st.mapped, ", unmapped quads ", st.unmapped,
        ", hazards ", st.hazards, ", tainted ", st.tainted,
        ", mixed targets ", st.mixedTargets, ", partial clears ", st.partialClears,
        ", partial copies ", st.partialCopies, ", mode switches ", st.modeSwitches));
    }

    {
      std::lock_guard<std::mutex> lock(g_foveationCurrentMutex);
      const D3D9FoveationStats& st = g_foveationStats;
      g_foveationLastFrame = { st.mapped, st.unmapped, st.hazards, st.tainted,
        st.mixedTargets, st.partialClears, st.partialCopies, st.modeSwitches };
    }

    g_foveationStats = { };
    s_frame += 1;
  }

}


extern "C" void skyrim_foveation_set_provider(skyrim_foveation_provider_fn fn, void* user) {
  g_foveationProviderUser.store(user, std::memory_order_release);
  g_foveationProviderFn.store(fn, std::memory_order_release);
}


extern "C" int skyrim_foveation_current(skyrim_foveation_map* map) {
  std::lock_guard<std::mutex> lock(g_foveationCurrentMutex);

  if (map != nullptr)
    *map = g_foveationCurrent;

  return g_foveationCurrentOn ? 1 : 0;
}


extern "C" int skyrim_foveation_last_frame(skyrim_foveation_counts* counts) {
  std::lock_guard<std::mutex> lock(g_foveationCurrentMutex);

  if (counts != nullptr)
    *counts = g_foveationLastFrame;

  return g_foveationCurrentOn ? 1 : 0;
}


#if defined(__APPLE__)
extern "C" void skyrim_present_tap_set(skyrim_present_tap_fn fn, void* user) {
  g_presentTapUser.store(user, std::memory_order_release);
  g_presentTapFn.store(fn, std::memory_order_release);
}


extern "C" void skyrim_present_tap_wait(void* state, uint64_t frame_id) {
  auto* s = static_cast<dxvk::D3D9PresentTapState*>(state);
  s->fence->wait(frame_id);
}


extern "C" void skyrim_present_tap_release(void* state, uint32_t slot) {
  auto* s = static_cast<dxvk::D3D9PresentTapState*>(state);
  s->busy[slot].store(false, std::memory_order_release);
  if (s->decRef() == 0)
    delete s;
}


extern "C" int skyrim_present_tap_ready(void* state, uint64_t frame_id) {
  auto* s = static_cast<dxvk::D3D9PresentTapState*>(state);
  return s->fence->value() >= frame_id ? 1 : 0;
}


extern "C" void skyrim_present_tap_set_depth(void* surface) {
  g_presentTapDepth.store(static_cast<IDirect3DSurface9*>(surface), std::memory_order_release);
}


extern "C" void skyrim_present_tap_set_exclusive(int on) {
  g_presentTapExclusive.store(on != 0, std::memory_order_release);
}
#endif
