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
#include <vulkan/vulkan.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
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
} skyrim_immersive_frame;
int skyrim_frame_camera(float out[18]);  /* runtime/d3d9_bridge.cpp */
}

namespace {

struct Entry {
    void *state = nullptr;
    void *device = nullptr;
    uint64_t image = 0;
    uint64_t frame = 0;
    uint64_t depth = 0;
    uint32_t slot = 0, width = 0, height = 0, format = 0;
    float camera[18] = {};
    bool has_camera = false;
    bool valid = false;
};

struct Cached {
    id<MTLTexture> sampled;
    bool decode = false;
};

std::mutex g_lock;
Entry g_mailbox[2];  /* oldest first; [1] valid only if [0] is */
bool g_active;

/* Compositor thread only. */
Entry g_shown;
void *g_cache_state;
std::unordered_map<uint64_t, Cached> g_cache;
std::unordered_map<uint64_t, id<MTLTexture>> g_depth_cache;  /* R32Float, sampled as it is */
PFN_vkExportMetalObjectsEXT g_export;

Entry entry_of(const dxvk_present_tap_frame *frame) {
    Entry entry;
    entry.state = frame->state;
    entry.device = frame->vk_device;
    entry.image = frame->vk_image;
    entry.frame = frame->frame_id;
    entry.slot = frame->slot;
    entry.width = frame->width;
    entry.height = frame->height;
    entry.format = frame->vk_format;
    entry.depth = frame->depth_vk_image;
    entry.has_camera = skyrim_frame_camera(entry.camera) != 0;  /* called inside this frame's Present */
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
        result.sampled = [exported newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
        if (!result.sampled) {
            result.sampled = exported;
            result.decode = true;
        }
        fprintf(stderr, "immersive: slot %u exported %lux%lu, VkFormat %u, Metal format %lu, sRGB view %d\n", entry.slot,
                (unsigned long)exported.width, (unsigned long)exported.height, entry.format,
                (unsigned long)exported.pixelFormat, result.decode ? 0 : 1);
    }
    return g_cache[entry.image] = result;
}

id<MTLTexture> depth_texture(const Entry &entry) {
    if (!entry.depth || !g_export) return nil;
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
    return g_depth_cache[entry.depth] = exported;
}

}  // namespace

extern "C" void skyrim_immersive_start(void) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_active = true;
    }
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

extern "C" int skyrim_immersive_acquire(skyrim_immersive_frame *out) {
    *out = skyrim_immersive_frame();
    Entry taken, skipped;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_mailbox[1].valid && dxvk_present_tap_ready(g_mailbox[1].state, g_mailbox[1].frame)) {
            std::swap(skipped, g_mailbox[0]);
            std::swap(taken, g_mailbox[1]);
        } else if (g_mailbox[0].valid && dxvk_present_tap_ready(g_mailbox[0].state, g_mailbox[0].frame)) {
            std::swap(taken, g_mailbox[0]);
            std::swap(g_mailbox[0], g_mailbox[1]);
        }
    }
    if (skipped.valid) dxvk_present_tap_release(skipped.state, skipped.slot);
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
    out->depth_texture = (__bridge void *)depth_texture(g_shown);
    memcpy(out->camera, g_shown.camera, sizeof out->camera);
    out->has_camera = g_shown.has_camera ? 1 : 0;
    return 1;
}
