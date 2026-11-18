/* Skyrim Apple port: hands the DXVK present tap's frames to the visionOS compositor
 * (apple/Sources/App/ImmersiveScreen.swift).
 *
 * The tap callback runs on the game thread and must not block, so it only queues the frame in a two-entry mailbox,
 * releasing the oldest if it is full. The compositor takes the newest queued frame the GPU has finished, releasing
 * older ones, and re-presents the frame it already has otherwise. Keeping two matters: with one entry, a game running
 * faster than the GPU finishes its frames kept replacing a frame that was about to be ready with one that wasn't, and
 * only about 47 game frames a second reached the display at 60 and at 120 fps (headset, 2026-11-10). A frame the
 * compositor stops showing is given back after the last command buffer that read it. */
#define VK_USE_PLATFORM_METAL_EXT 1
#import <Metal/Metal.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unordered_map>
#include "dxvk_present_tap.h"

extern "C" {
typedef struct skyrim_immersive_frame {
    void *texture;
    void *retired_state;
    uint32_t retired_slot;
    int retired;
    int is_new;
    int needs_srgb_decode;
    uint32_t width;
    uint32_t height;
    void *depth_texture;
    float camera[18];
    int has_camera;
    void *texture_eye2;        /* stereo (LB_STEREO_PRESENT): the right eye's picture and depth; NULL when mono */
    void *depth_texture_eye2;
    int eye_views;             /* 1: each layer was rendered through that eye's own frustum and pose (below) */
    float eye_tangents[2][4];  /* l r t b at unit distance, per eye */
    float eye_pose[2][16];     /* the eye in the game camera's frame: column-major, metres, x right y up z back */
} skyrim_immersive_frame;
int skyrim_frame_camera(float out[18]);  /* runtime/d3d9_bridge.cpp */
int skyrim_frame_eyes(float tangents[8], float eye_pose[32]);
int skyrim_frame_eye(float *k_out);  /* runtime/d3d9_bridge.cpp: -1 left, 0 none/centre, +1 right; k_out: clip shift */
}

namespace {

struct Entry {
    void *state = nullptr;
    void *device = nullptr;
    uint64_t image = 0;
    uint64_t frame = 0;
    uint64_t depth = 0;
    uint32_t slot = 0, width = 0, height = 0, format = 0, layers = 1, depth_layers = 0;
    float camera[18] = {};
    bool has_camera = false;
    float eye_tangents[8] = {}, eye_pose[32] = {};
    bool eye_views = false;
    bool valid = false;
    uint64_t submitted_ns = 0;  /* monotonic, stamped in on_frame; see gpu_observe */
    uint64_t ready_ns = 0;      /* monotonic, when acquire first saw the frame ready; 0 until then */
};

struct Cached {
    id<MTLTexture> sampled;
    id<MTLTexture> sampled_eye2;  /* layer 1 of a stereo tap image */
    bool decode = false;
};
struct CachedDepth {
    id<MTLTexture> depth;
    id<MTLTexture> depth_eye2;
};

std::mutex g_lock;
Entry g_mailbox[2];  /* oldest first; [1] valid only if [0] is */
bool g_active;

/* Compositor thread only. */
Entry g_shown;
std::atomic<uint32_t> g_mailbox_drops;  /* frames pushed out of a full mailbox by the game thread */
void *g_cache_state;
std::unordered_map<uint64_t, Cached> g_cache;
std::unordered_map<uint64_t, CachedDepth> g_depth_cache;  /* R32Float, sampled as it is */
PFN_vkExportMetalObjectsEXT g_export;

uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

Entry entry_of(const dxvk_present_tap_frame *frame) {
    Entry entry;
    entry.submitted_ns = now_ns();
    entry.state = frame->state;
    entry.device = frame->vk_device;
    entry.image = frame->vk_image;
    entry.frame = frame->frame_id;
    entry.slot = frame->slot;
    entry.width = frame->width;
    entry.height = frame->height;
    entry.format = frame->vk_format;
    entry.depth = frame->depth_vk_image;
    entry.layers = frame->layers ? frame->layers : 1;
    entry.depth_layers = frame->depth_layers;
    entry.has_camera = skyrim_frame_camera(entry.camera) != 0;  /* called inside this frame's Present */
    entry.eye_views = entry.has_camera && entry.layers >= 2 && skyrim_frame_eyes(entry.eye_tangents, entry.eye_pose) != 0;
    entry.valid = true;
    return entry;
}

void on_frame(void *, const dxvk_present_tap_frame *frame) {
    Entry drop = entry_of(frame);
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_active) {
            if (!g_mailbox[0].valid) {
                std::swap(drop, g_mailbox[0]);
            } else if (!g_mailbox[1].valid) {
                std::swap(drop, g_mailbox[1]);
            } else {
                g_mailbox_drops.fetch_add(1, std::memory_order_relaxed);
                std::swap(drop, g_mailbox[0]);     /* drop = oldest, [0] = new */
                std::swap(g_mailbox[0], g_mailbox[1]);
            }
        }
    }
    if (drop.valid) dxvk_present_tap_release(drop.state, drop.slot);
}

/* The texture to sample for a frame: exported once per tap image, through an sRGB view because the image holds
 * sRGB-encoded values in a UNORM format. Without the view the shader decodes instead. */
const Cached &cached(const Entry &entry) {
    if (entry.state != g_cache_state) {
        g_cache.clear();
        g_depth_cache.clear();
        g_cache_state = entry.state;
    }
    auto found = g_cache.find(entry.image);
    if (found != g_cache.end()) return found->second;
    Cached result;
    if (!g_export) g_export = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr((VkDevice)entry.device, "vkExportMetalObjectsEXT");
    if (!g_export) {
        fprintf(stderr, "immersive: vkExportMetalObjectsEXT unavailable\n");
    } else {
        VkExportMetalTextureInfoEXT texture = {VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
        texture.image = (VkImage)(uintptr_t)entry.image;
        texture.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
        VkExportMetalObjectsInfoEXT info = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &texture};
        g_export((VkDevice)entry.device, &info);
        id<MTLTexture> exported = texture.mtlTexture;
        if (entry.layers >= 2 && exported.arrayLength >= 2) {
            // Stereo: one 2D view per eye (layer), so the compositor samples plain 2D textures either way.
            auto layer = [&](NSUInteger i, MTLPixelFormat format) {
                return [exported newTextureViewWithPixelFormat:format textureType:MTLTextureType2D
                                                        levels:NSMakeRange(0, 1) slices:NSMakeRange(i, 1)];
            };
            result.sampled = layer(0, MTLPixelFormatBGRA8Unorm_sRGB);
            result.sampled_eye2 = layer(1, MTLPixelFormatBGRA8Unorm_sRGB);
            if (!result.sampled || !result.sampled_eye2) {
                result.sampled = layer(0, exported.pixelFormat);
                result.sampled_eye2 = layer(1, exported.pixelFormat);
                result.decode = true;
            }
        } else {
            result.sampled = [exported newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
            if (!result.sampled) {
                result.sampled = exported;
                result.decode = true;
            }
        }
        fprintf(stderr, "immersive: slot %u exported %lux%lu, VkFormat %u, Metal format %lu, sRGB view %d\n", entry.slot,
                (unsigned long)exported.width, (unsigned long)exported.height, entry.format,
                (unsigned long)exported.pixelFormat, result.decode ? 0 : 1);
    }
    return g_cache[entry.image] = result;
}

const CachedDepth &depth_texture(const Entry &entry) {
    static const CachedDepth none{};
    if (!entry.depth || !g_export) return none;
    auto found = g_depth_cache.find(entry.depth);
    if (found != g_depth_cache.end()) return found->second;
    VkExportMetalTextureInfoEXT texture = {VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    texture.image = (VkImage)(uintptr_t)entry.depth;
    texture.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT info = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &texture};
    g_export((VkDevice)entry.device, &info);
    id<MTLTexture> exported = texture.mtlTexture;
    fprintf(stderr, "immersive: slot %u depth exported %lux%lu, Metal format %lu\n", entry.slot,
            (unsigned long)exported.width, (unsigned long)exported.height, (unsigned long)exported.pixelFormat);
    CachedDepth result;
    if (entry.depth_layers >= 2 && exported.arrayLength >= 2) {
        result.depth = [exported newTextureViewWithPixelFormat:exported.pixelFormat textureType:MTLTextureType2D
                                                        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
        result.depth_eye2 = [exported newTextureViewWithPixelFormat:exported.pixelFormat textureType:MTLTextureType2D
                                                             levels:NSMakeRange(0, 1) slices:NSMakeRange(1, 1)];
    } else {
        result.depth = exported;
    }
    return g_depth_cache[entry.depth] = result;
}

}  // namespace

namespace {
/* MetalFX on this device, logged once when immersive play starts. The visionOS SDK marks the temporal scaler, the
 * temporal denoiser and the frame interpolator unavailable, so they are looked up by name: this says whether the
 * system has them anyway, whether they report support for this GPU, and whether a temporal scaler can be made. */
void metalfx_probe(void) {
    static bool done;
    if (done) return;
    done = true;
    dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    SEL supports = NSSelectorFromString(@"supportsDevice:");
    for (NSString *name in @[@"MTLFXSpatialScalerDescriptor", @"MTLFXTemporalScalerDescriptor",
                             @"MTLFXTemporalDenoisedScalerDescriptor", @"MTLFXFrameInterpolatorDescriptor"]) {
        Class cls = NSClassFromString(name);
        int supported = -1;
        if (cls && device && [cls respondsToSelector:supports])
            supported = ((BOOL (*)(id, SEL, id))objc_msgSend)(cls, supports, device) ? 1 : 0;
        fprintf(stderr, "metalfx probe: %s %s, supportsDevice %d\n", name.UTF8String, cls ? "present" : "absent",
                supported);
    }
    Class temporal = NSClassFromString(@"MTLFXTemporalScalerDescriptor");
    if (!temporal || !device) return;
    @try {
        id descriptor = [[temporal alloc] init];
        [descriptor setValue:@(MTLPixelFormatRGBA16Float) forKey:@"colorTextureFormat"];
        [descriptor setValue:@(MTLPixelFormatDepth32Float) forKey:@"depthTextureFormat"];
        [descriptor setValue:@(MTLPixelFormatRG16Float) forKey:@"motionTextureFormat"];
        [descriptor setValue:@(MTLPixelFormatRGBA16Float) forKey:@"outputTextureFormat"];
        [descriptor setValue:@960 forKey:@"inputWidth"];
        [descriptor setValue:@540 forKey:@"inputHeight"];
        [descriptor setValue:@1920 forKey:@"outputWidth"];
        [descriptor setValue:@1080 forKey:@"outputHeight"];
        SEL make = NSSelectorFromString(@"newTemporalScalerWithDevice:");
        typedef id __attribute__((ns_returns_retained)) (*MakeFn)(id, SEL, id);
        id scaler = [descriptor respondsToSelector:make] ? ((MakeFn)objc_msgSend)(descriptor, make, device) : nil;
        fprintf(stderr, "metalfx probe: temporal scaler 960x540 -> 1920x1080 %s%s\n", scaler ? "created, " : "not created",
                scaler ? object_getClassName(scaler) : "");
    } @catch (NSException *e) {
        fprintf(stderr, "metalfx probe: temporal scaler threw %s\n", e.reason.UTF8String);
    }
}
}  // namespace

extern "C" void skyrim_immersive_start(void) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_active = true;
    }
    metalfx_probe();
    dxvk_present_tap_set(on_frame, nullptr);
    dxvk_present_tap_set_exclusive(1);
    fprintf(stderr, "immersive: tap registered, tap-only presents on\n");
}

extern "C" void skyrim_immersive_stop(void) {
    dxvk_present_tap_set_exclusive(0);
    dxvk_present_tap_set(nullptr, nullptr);
    Entry waiting[2];
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_active = false;
        std::swap(waiting[0], g_mailbox[0]);
        std::swap(waiting[1], g_mailbox[1]);
    }
    for (const Entry &entry : waiting)
        if (entry.valid) dxvk_present_tap_release(entry.state, entry.slot);
    if (g_shown.valid) dxvk_present_tap_release(g_shown.state, g_shown.slot);
    g_shown = Entry();
    g_cache.clear();
    g_depth_cache.clear();
    g_cache_state = nullptr;
    fprintf(stderr, "immersive: tap cleared, window presents back on\n");
}

extern "C" void skyrim_immersive_release(void *state, uint32_t slot) { dxvk_present_tap_release(state, slot); }

namespace {
/* Display cadence: for each game frame, how many compositor refreshes it stayed on screen, and how many frames
 * were replaced before being shown. Logged every 900 refreshes (10 s at 90 Hz); an even 45 fps at 90 Hz shows as
 * nearly every frame held for 2. */
struct Cadence { uint32_t refreshes, held, hist[5], skipped; uint32_t gpu_count; uint64_t gpu_total_ns, gpu_max_ns; };
Cadence g_cadence;

/* Game GPU latency: from on_frame handing a frame over (submitted_ns) to the first refresh whose poll of
 * dxvk_present_tap_ready finds it finished (ready_ns). Both stamps live in the frame's Entry, which moves
 * between the mailbox and g_shown by value, so they always belong to the same frame; submitted_ns is written
 * before the Entry is published under g_lock and ready_ns is written under g_lock on the compositor thread.
 * This is an UPPER BOUND on the game frame's GPU time, not the time itself: on_frame runs on the game thread once
 * the frame is queued to DXVK's command thread, so it includes that thread recording and submitting the frame,
 * the GPU working through whatever the compositor queued ahead of it, and rounding up to the next poll (about 11 ms at
 * 90 Hz). Frames that on_frame pushes out of a full mailbox before any poll are not measured. Called with
 * g_lock held, on the compositor thread, which is the only one that touches g_cadence. */
void gpu_observe(Entry &entry) {
    if (!entry.valid || entry.ready_ns || !dxvk_present_tap_ready(entry.state, entry.frame)) return;
    entry.ready_ns = now_ns();
    uint64_t latency = entry.ready_ns > entry.submitted_ns ? entry.ready_ns - entry.submitted_ns : 0;
    Cadence &c = g_cadence;
    c.gpu_count++;
    c.gpu_total_ns += latency;
    if (latency > c.gpu_max_ns) c.gpu_max_ns = latency;
}

void cadence_refresh(bool fresh, int skipped) {
    Cadence &c = g_cadence;
    if (fresh) {
        if (c.held) c.hist[c.held < 4 ? c.held : 4]++;
        c.held = 0;
    }
    c.held += fresh || g_shown.valid ? 1 : 0;
    c.skipped += (uint32_t)skipped + g_mailbox_drops.exchange(0, std::memory_order_relaxed);
    if (++c.refreshes < 900) return;
    uint32_t frames = c.hist[1] + c.hist[2] + c.hist[3] + c.hist[4];
    if (frames) {
        char gpu[96] = "";
        if (c.gpu_count)
            snprintf(gpu, sizeof gpu, "; game gpu latency mean %.2f ms, max %.2f ms",
                     (double)c.gpu_total_ns / c.gpu_count / 1e6, (double)c.gpu_max_ns / 1e6);
        fprintf(stderr, "immersive cadence: %u game frames over %u refreshes; held 1: %u, 2: %u, 3: %u, 4+: %u; "
                "replaced unseen: %u%s\n", frames, c.refreshes, c.hist[1], c.hist[2], c.hist[3], c.hist[4], c.skipped,
                gpu);
    }
    uint32_t held = c.held;
    c = Cadence();
    c.held = held;
}
}  // namespace

extern "C" int skyrim_immersive_acquire(skyrim_immersive_frame *out) {
    *out = skyrim_immersive_frame();
    Entry taken, skipped;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        gpu_observe(g_mailbox[0]);  /* stamps readiness of both queued frames, for the latency log only */
        gpu_observe(g_mailbox[1]);
        if (g_mailbox[1].valid && g_mailbox[1].ready_ns) {
            std::swap(skipped, g_mailbox[0]);
            std::swap(taken, g_mailbox[1]);
        } else if (g_mailbox[0].valid && g_mailbox[0].ready_ns) {
            std::swap(taken, g_mailbox[0]);
            std::swap(g_mailbox[0], g_mailbox[1]);
        }
    }
    if (skipped.valid) dxvk_present_tap_release(skipped.state, skipped.slot);
    cadence_refresh(taken.valid, skipped.valid ? 1 : 0);
    if (taken.valid) {
        if (g_shown.valid) {
            out->retired = 1;
            out->retired_state = g_shown.state;
            out->retired_slot = g_shown.slot;
        }
        g_shown = taken;
        out->is_new = 1;
    }
    if (!g_shown.valid) return 0;
    const Cached &texture = cached(g_shown);
    if (!texture.sampled) return 0;
    out->texture = (__bridge void *)texture.sampled;
    out->needs_srgb_decode = texture.decode ? 1 : 0;
    out->width = g_shown.width;
    out->height = g_shown.height;
    const CachedDepth &depth = depth_texture(g_shown);
    out->depth_texture = (__bridge void *)depth.depth;
    if (texture.sampled_eye2) {
        out->texture_eye2 = (__bridge void *)texture.sampled_eye2;
        out->depth_texture_eye2 = (__bridge void *)depth.depth_eye2;
    }
    memcpy(out->camera, g_shown.camera, sizeof out->camera);
    out->has_camera = g_shown.has_camera ? 1 : 0;
    if (g_shown.eye_views && out->texture_eye2) {
        out->eye_views = 1;
        memcpy(out->eye_tangents, g_shown.eye_tangents, sizeof out->eye_tangents);
        memcpy(out->eye_pose, g_shown.eye_pose, sizeof out->eye_pose);
    }
    return 1;
}
