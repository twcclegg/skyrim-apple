/* Skyrim Apple port: MetalFX's temporal scaler for the immersive compositor
 * (apple/Sources/App/ImmersiveScreen.swift), reached by name.
 *
 * The visionOS SDK (27.2) marks MTLFXTemporalScaler and its descriptor API_UNAVAILABLE(visionos), so neither Swift
 * nor this file can name them. Whether the headset's system has the classes anyway is not known yet (branch
 * immersive/metalfx-probe logs it). So the descriptor class is looked up when the compositor asks, and the parts of
 * MetalFX's interface used here are declared again below so the calls are typed. Anything missing comes back as a
 * reason, and the compositor falls back to the spatial scaler.
 *
 * Written on Linux; docs/handoff/temporal-upscale.md says what has been compiled and run since. A Mac can run it:
 *
 *     xcrun clang++ -std=c++17 -fobjc-arc -DSKYRIM_TEMPORAL_SCALER_SELFTEST ipad/metalfx_temporal.mm \
 *         -framework Metal -framework Foundation -framework MetalFX -o /tmp/temporal_scaler && /tmp/temporal_scaler
 */
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

/* MTLFXTemporalScalerDescriptor and MTLFXTemporalScaler (MetalFX/MTLFXTemporalScaler.h), as far as they are used. */
@protocol SkyrimTemporalDescriptor <NSObject>
@property (nonatomic) MTLPixelFormat colorTextureFormat;
@property (nonatomic) MTLPixelFormat depthTextureFormat;
@property (nonatomic) MTLPixelFormat motionTextureFormat;
@property (nonatomic) MTLPixelFormat outputTextureFormat;
@property (nonatomic) NSUInteger inputWidth;
@property (nonatomic) NSUInteger inputHeight;
@property (nonatomic) NSUInteger outputWidth;
@property (nonatomic) NSUInteger outputHeight;
@property (nonatomic, getter=isAutoExposureEnabled) BOOL autoExposureEnabled;
+ (BOOL)supportsDevice:(id<MTLDevice>)device;
- (id)newTemporalScalerWithDevice:(id<MTLDevice>)device;
@end

@protocol SkyrimTemporalScaler <NSObject>
@property (nonatomic, readonly) MTLTextureUsage colorTextureUsage;
@property (nonatomic, readonly) MTLTextureUsage depthTextureUsage;
@property (nonatomic, readonly) MTLTextureUsage motionTextureUsage;
@property (nonatomic, readonly) MTLTextureUsage outputTextureUsage;
@property (nonatomic) NSUInteger inputContentWidth;
@property (nonatomic) NSUInteger inputContentHeight;
@property (nonatomic, retain) id<MTLTexture> colorTexture;
@property (nonatomic, retain) id<MTLTexture> depthTexture;
@property (nonatomic, retain) id<MTLTexture> motionTexture;
@property (nonatomic, retain) id<MTLTexture> outputTexture;
@property (nonatomic) float jitterOffsetX;
@property (nonatomic) float jitterOffsetY;
@property (nonatomic) float motionVectorScaleX;
@property (nonatomic) float motionVectorScaleY;
@property (nonatomic) BOOL reset;
@property (nonatomic, getter=isDepthReversed) BOOL depthReversed;
- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer;
@end

extern "C" {
/* Keep in step with apple/Sources/Native/SkyrimNativeBridge.h. */
typedef struct skyrim_temporal_scaler_setup {
    uint32_t input_width, input_height, output_width, output_height;
    uint32_t color_format, depth_format, motion_format, output_format;
    int auto_exposure;
    uint32_t color_usage, depth_usage, motion_usage, output_usage;
} skyrim_temporal_scaler_setup;
typedef struct skyrim_temporal_scaler_frame {
    void *color, *depth, *motion, *output;
    float jitter_x, jitter_y, motion_scale_x, motion_scale_y;
    int reset;
    int depth_reversed;
} skyrim_temporal_scaler_frame;
void *skyrim_temporal_scaler_make(void *device, skyrim_temporal_scaler_setup *setup, char *why, size_t why_size);
int skyrim_temporal_scaler_encode(void *scaler, const skyrim_temporal_scaler_frame *frame, void *command_buffer);
void skyrim_temporal_scaler_free(void *scaler);
}

namespace {
void *no(char *why, size_t size, NSString *text) {
    if (why && size) snprintf(why, size, "%s", text.UTF8String);
    return nullptr;
}
}  // namespace

void *skyrim_temporal_scaler_make(void *device_pointer, skyrim_temporal_scaler_setup *setup, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_pointer;
    if (!device || !setup) return no(why, why_size, @"no device");
    dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY);
    Class found = NSClassFromString(@"MTLFXTemporalScalerDescriptor");
    if (!found) return no(why, why_size, @"this system has no MetalFX temporal scaler (no MTLFXTemporalScalerDescriptor class)");
    @try {
        if ([found respondsToSelector:@selector(supportsDevice:)]
            && ![(Class<SkyrimTemporalDescriptor>)found supportsDevice:device])
            return no(why, why_size, @"MetalFX temporal scaling is not supported on this device");
        id<SkyrimTemporalDescriptor> description = [[found alloc] init];
        description.inputWidth = setup->input_width;
        description.inputHeight = setup->input_height;
        description.outputWidth = setup->output_width;
        description.outputHeight = setup->output_height;
        description.colorTextureFormat = (MTLPixelFormat)setup->color_format;
        description.depthTextureFormat = (MTLPixelFormat)setup->depth_format;
        description.motionTextureFormat = (MTLPixelFormat)setup->motion_format;
        description.outputTextureFormat = (MTLPixelFormat)setup->output_format;
        description.autoExposureEnabled = setup->auto_exposure != 0;
        id<SkyrimTemporalScaler> scaler = [description newTemporalScalerWithDevice:device];
        if (!scaler)
            return no(why, why_size, [NSString stringWithFormat:@"MetalFX made no temporal scaler for %ux%u -> %ux%u",
                                      setup->input_width, setup->input_height, setup->output_width, setup->output_height]);
        setup->color_usage = (uint32_t)scaler.colorTextureUsage;
        setup->depth_usage = (uint32_t)scaler.depthTextureUsage;
        setup->motion_usage = (uint32_t)scaler.motionTextureUsage;
        setup->output_usage = (uint32_t)scaler.outputTextureUsage;
        return (__bridge_retained void *)scaler;
    } @catch (NSException *thrown) {
        return no(why, why_size, [NSString stringWithFormat:@"MetalFX's temporal scaler threw: %@", thrown.reason]);
    }
}

/* 1 when the upscale was encoded; 0 when MetalFX threw (logged), and the compositor should stop using it. */
int skyrim_temporal_scaler_encode(void *scaler_pointer, const skyrim_temporal_scaler_frame *frame, void *command_buffer) {
    id<SkyrimTemporalScaler> scaler = (__bridge id<SkyrimTemporalScaler>)scaler_pointer;
    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)command_buffer;
    if (!scaler || !frame || !commands) return 0;
    @try {
        id<MTLTexture> color = (__bridge id<MTLTexture>)frame->color;
        scaler.colorTexture = color;
        scaler.depthTexture = (__bridge id<MTLTexture>)frame->depth;
        scaler.motionTexture = (__bridge id<MTLTexture>)frame->motion;
        scaler.outputTexture = (__bridge id<MTLTexture>)frame->output;
        scaler.inputContentWidth = color.width;
        scaler.inputContentHeight = color.height;
        scaler.jitterOffsetX = frame->jitter_x;
        scaler.jitterOffsetY = frame->jitter_y;
        scaler.motionVectorScaleX = frame->motion_scale_x;
        scaler.motionVectorScaleY = frame->motion_scale_y;
        scaler.depthReversed = frame->depth_reversed != 0;
        scaler.reset = frame->reset != 0;
        [scaler encodeToCommandBuffer:commands];
        return 1;
    } @catch (NSException *thrown) {
        fprintf(stderr, "immersive: temporal: MetalFX threw while encoding: %s\n", thrown.reason.UTF8String);
        return 0;
    }
}

void skyrim_temporal_scaler_free(void *scaler_pointer) {
    if (scaler_pointer) (void)(__bridge_transfer id)scaler_pointer;
}

#ifdef SKYRIM_TEMPORAL_SCALER_SELFTEST
#import <objc/runtime.h>
/* Makes a scaler by name with the compositor's formats, pushes two frames through it and waits for the GPU. */
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "no Metal device\n");
            return 1;
        }
        skyrim_temporal_scaler_setup setup = {};
        setup.input_width = 960;
        setup.input_height = 540;
        setup.output_width = 1920;
        setup.output_height = 1080;
        setup.color_format = MTLPixelFormatRGBA16Float;
        setup.depth_format = MTLPixelFormatDepth32Float;
        setup.motion_format = MTLPixelFormatRG16Float;
        setup.output_format = MTLPixelFormatRGBA16Float;
        char why[256];
        void *scaler = skyrim_temporal_scaler_make((__bridge void *)device, &setup, why, sizeof why);
        if (!scaler) {
            fprintf(stderr, "no scaler: %s\n", why);
            return 1;
        }
        printf("scaler made by name: %s; usage colour %u depth %u motion %u output %u\n",
               object_getClassName((__bridge id)scaler), setup.color_usage, setup.depth_usage, setup.motion_usage,
               setup.output_usage);
        id<MTLTexture> (^texture)(MTLPixelFormat, uint32_t, uint32_t, uint32_t) =
            ^id<MTLTexture>(MTLPixelFormat format, uint32_t width, uint32_t height, uint32_t usage) {
                MTLTextureDescriptor *wanted = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                                   width:width
                                                                                                  height:height
                                                                                               mipmapped:NO];
                wanted.usage = usage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                wanted.storageMode = MTLStorageModePrivate;
                return [device newTextureWithDescriptor:wanted];
            };
        id<MTLTexture> color = texture(MTLPixelFormatRGBA16Float, 960, 540, setup.color_usage);
        id<MTLTexture> depth = texture(MTLPixelFormatDepth32Float, 960, 540, setup.depth_usage);
        id<MTLTexture> motion = texture(MTLPixelFormatRG16Float, 960, 540, setup.motion_usage);
        id<MTLTexture> output = texture(MTLPixelFormatRGBA16Float, 1920, 1080, setup.output_usage);
        if (!color || !depth || !motion || !output) {
            fprintf(stderr, "no textures\n");
            return 1;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        for (int count = 0; count < 2; count++) {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            skyrim_temporal_scaler_frame frame = {};
            frame.color = (__bridge void *)color;
            frame.depth = (__bridge void *)depth;
            frame.motion = (__bridge void *)motion;
            frame.output = (__bridge void *)output;
            frame.jitter_x = count ? 0.25f : -0.25f;
            frame.jitter_y = count ? -0.25f : 0.25f;
            frame.motion_scale_x = 1;
            frame.motion_scale_y = 1;
            frame.reset = count == 0;
            if (!skyrim_temporal_scaler_encode(scaler, &frame, (__bridge void *)commands)) {
                fprintf(stderr, "frame %d was not encoded\n", count);
                return 1;
            }
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "frame %d: command buffer status %lu: %s\n", count, (unsigned long)commands.status,
                        commands.error.localizedDescription.UTF8String);
                return 1;
            }
        }
        skyrim_temporal_scaler_free(scaler);
        printf("two frames encoded and completed\n");
    }
    return 0;
}
#endif
