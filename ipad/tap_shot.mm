/* Skyrim Apple port: LB_TAP_SHOT=F[,F...] (visionOS, immersive play) writes, for those frames, each eye's picture
 * exactly as the immersive compositor is handed it: the Metal views of the present tap image that
 * apple/Sources/App/ImmersiveScreen.swift samples, layer 0 (left eye) and layer 1 (right eye). Files go to the save
 * folder beside LB_SHOT's: tap-<frame>.bmp and, in stereo, tap-<frame>-layer1.bmp (24-bit BMP, same layout as
 * shot-*.bmp, raw bytes as stored, i.e. sRGB-encoded like LB_SHOT's). Only BGRA8 frames are captured; any other
 * format is logged and skipped.
 *
 * Why: LB_SHOT's per-layer back-buffer read returned layer 0 for both eyes on the headset (twcclegg/skyrim-apple
 * #42) while the headset showed the right eye correctly. This reads what is shown instead of a copy made
 * for the capture. Frames are numbered as LB_SHOT numbers them, so LB_SHOT and LB_TAP_SHOT with the same list
 * capture the same frame both ways. A wanted frame that the compositor never shows (replaced in its mailbox) is
 * written from the next frame it does show, still named after the wanted frame, and the log line names both
 * numbers. When several wanted frames fall into one shown frame, the files take the last one's number and each
 * earlier one gets its own log line.
 *
 * Cost: one blit of both layers and a wait for it on the compositor thread, a brief stall not yet measured on the
 * headset; each log line gives the wait. The files are written on a background queue. Off unless the variable is
 * set. */
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

extern "C" char g_save_dir[1024];  /* runtime/main.c */

namespace {

std::vector<int> &pending() {
    static std::vector<int> frames = [] {
        std::vector<int> out;
        for (const char *e = getenv("LB_TAP_SHOT"); e && *e;) {
            int f = atoi(e);
            if (f > 0) out.push_back(f);
            e = strchr(e, ',');
            if (e) e++;
        }
        return out;
    }();
    return frames;
}

/* False if the file could not be opened or fully written (errno says why). */
bool write_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, size_t pitch) {
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t header[54] = {'B', 'M'};
    auto put = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
    put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h);
    header[26] = 1; header[28] = 24; put(34, row * h);
    bool ok = fwrite(header, 1, 54, out) == 54;
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = h; ok && y-- > 0;) {  // BMP rows run bottom-up
        const uint8_t *src = bgra + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x++) memcpy(&line[x * 3], src + x * 4, 3);  // BGRA -> BGR
        ok = fwrite(line.data(), 1, row, out) == row;
    }
    return fclose(out) == 0 && ok;
}

bool bgra8(MTLPixelFormat f) { return f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB; }

}  // namespace

/* Called by ipad/immersive_bridge.mm on the compositor thread for each newly shown frame, with the tap image's
 * VkFormat and the 2D views it hands the compositor (eye2 nil in mono). The frame's GPU work is finished (the bridge
 * only takes ready frames) and the frame is not given back to DXVK before this returns. */
void tap_shot_shown(int frame, uint32_t vk_format, id<MTLTexture> eye0, id<MTLTexture> eye1) {
    std::vector<int> &wanted = pending();
    if (wanted.empty() || !eye0) return;
    std::vector<int> folded;  // wanted frames up to this one, in list order; the files take the last one's number
    for (size_t i = 0; i < wanted.size();) {
        if (wanted[i] <= frame) { folded.push_back(wanted[i]); wanted.erase(wanted.begin() + i); } else i++;
    }
    if (folded.empty()) return;
    const int asked = folded.back();
    for (size_t i = 0; i + 1 < folded.size(); i++)
        fprintf(stderr, "LB_TAP_SHOT frame %d: not shown on its own; captured with frame %d as tap-%05d\n", folded[i],
                frame, asked);

    /* The readback below assumes 4 bytes per pixel in B, G, R, A order: check the tap image itself, not only the
     * view (the bridge asks for a BGRA8 sRGB view whatever the image holds). */
    if ((vk_format != VK_FORMAT_B8G8R8A8_UNORM && vk_format != VK_FORMAT_B8G8R8A8_SRGB) || !bgra8(eye0.pixelFormat) ||
        (eye1 && !bgra8(eye1.pixelFormat))) {
        fprintf(stderr, "LB_TAP_SHOT frame %d (asked %d): VkFormat %u, Metal format %lu, not BGRA8; not captured\n",
                frame, asked, vk_format, (unsigned long)eye0.pixelFormat);
        return;
    }

    static id<MTLCommandQueue> queue;
    if (!queue) queue = [eye0.device newCommandQueue];
    const uint64_t start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    id<MTLTexture> eyes[2] = {eye0, eye1};
    id<MTLBuffer> buffers[2] = {nil, nil};
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (int e = 0; e < 2; e++) {
        id<MTLTexture> t = eyes[e];
        if (!t) continue;
        const NSUInteger pitch = t.width * 4, bytes = pitch * t.height;
        buffers[e] = [eye0.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:buffers[e] destinationOffset:0
      destinationBytesPerRow:pitch destinationBytesPerImage:bytes];
    }
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const double wait_ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start_ns) / 1e6;
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "LB_TAP_SHOT frame %d: blit failed (%s)\n", frame, cb.error.localizedDescription.UTF8String);
        return;
    }

    static dispatch_queue_t writer = dispatch_queue_create("skyrim.tap_shot", DISPATCH_QUEUE_SERIAL);
    for (int e = 0; e < 2; e++) {
        if (!buffers[e]) continue;
        id<MTLBuffer> buffer = buffers[e];
        const uint32_t w = (uint32_t)eyes[e].width, h = (uint32_t)eyes[e].height;
        char path[1200];
        if (e) snprintf(path, sizeof path, "%s/tap-%05d-layer1.bmp", g_save_dir, asked);
        else snprintf(path, sizeof path, "%s/tap-%05d.bmp", g_save_dir, asked);
        std::string file(path);
        dispatch_async(writer, ^{
            const bool ok = write_bmp(file.c_str(), (const uint8_t *)buffer.contents, w, h, (size_t)w * 4);
            const char *failed = ok ? nullptr : strerror(errno);
            fprintf(stderr, "LB_TAP_SHOT frame %d (asked %d) %s %ux%u, blit wait %.2f ms -> %s%s%s%s\n", frame, asked,
                    e ? "layer 1 (right eye)" : "layer 0 (left eye)", w, h, wait_ms, file.c_str(),
                    failed ? " (write failed: " : "", failed ? failed : "", failed ? ")" : "");
        });
    }
    if (!eye1) fprintf(stderr, "LB_TAP_SHOT frame %d: mono frame, layer 0 only\n", frame);
}
