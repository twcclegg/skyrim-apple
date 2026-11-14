// The fixed rate map for foveating the game's own render (docs/handoff/foveated-render.md), for testing on the
// Mac build without a headset:
//
//   tools/foveation_map.py rates > /tmp/rates.json
//   LB_GAME_FOVEATION=1 LB_GAME_FOVEATION_MAP=/tmp/rates.json [LB_GAME_FOVEATION_LUT=/tmp/lut.json] ...
//
// DXVK asks for a map each time the swap chain's back buffers are made (skyrim_foveation.h). This makes one
// MTLRasterizationRateMap for the game's size from the rates in the file and, if asked, writes the map Metal
// really made as a table for tools/foveation_map.py unwarp --lut.
//
// NOT PART OF ANY BUILD YET, and never compiled: written on Linux without a Metal SDK. To try it, add this
// file to the target that links DXVK (macos/CMakeLists.txt: OBJCXX language, -fobjc-arc, the include path
// android/app/jni/dxvk/include/native, -framework Metal -framework Foundation). It registers itself; without
// LB_GAME_FOVEATION_MAP it does nothing. The map only reaches Metal with the MoltenVK patch in the note.
//
// The map is made on the system default device. MoltenVK must be drawing on the same one, which holds on
// Apple silicon Macs and the headset (one GPU).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdlib.h>

#include "skyrim_foveation.h"

namespace {

id<MTLRasterizationRateMap> g_map;  // what DXVK was last given; kept until the next call

NSArray<NSNumber *> *rates(NSDictionary *json, NSString *key) {
    NSArray *list = [json[key] isKindOfClass:NSArray.class] ? json[key] : nil;
    for (id rate in list) {
        if (![rate isKindOfClass:NSNumber.class]) return nil;
    }
    return list.count >= 1 ? list : nil;
}

void write_table(id<MTLRasterizationRateMap> map, MTLSize screen, MTLSize physical, const char *path) {
    NSMutableArray<NSNumber *> *x = [NSMutableArray array], *y = [NSMutableArray array];
    for (NSUInteger i = 0; i <= physical.width; i++) {
        MTLCoordinate2D at = MTLCoordinate2DMake((float)i, physical.height * 0.5f);
        [x addObject:@([map mapPhysicalToScreenCoordinates:at forLayer:0].x)];
    }
    for (NSUInteger i = 0; i <= physical.height; i++) {
        MTLCoordinate2D at = MTLCoordinate2DMake(physical.width * 0.5f, (float)i);
        [y addObject:@([map mapPhysicalToScreenCoordinates:at forLayer:0].y)];
    }
    NSDictionary *table = @{@"screen": @[@(screen.width), @(screen.height)],
                            @"physical": @[@(physical.width), @(physical.height)], @"x": x, @"y": y};
    NSData *data = [NSJSONSerialization dataWithJSONObject:table options:0 error:nil];
    if (![data writeToFile:@(path) atomically:YES]) NSLog(@"foveation: cannot write %s", path);
}

int provide(void *, skyrim_foveation_map *out) {
    @autoreleasepool {
        const char *path = getenv("LB_GAME_FOVEATION_MAP");
        NSData *file = path ? [NSData dataWithContentsOfFile:@(path)] : nil;
        NSDictionary *json = file ? [NSJSONSerialization JSONObjectWithData:file options:0 error:nil] : nil;
        if (![json isKindOfClass:NSDictionary.class]) {
            NSLog(@"foveation: cannot read %s", path ? path : "LB_GAME_FOVEATION_MAP");
            return 0;
        }
        NSArray<NSNumber *> *horizontal = rates(json, @"horizontal"), *vertical = rates(json, @"vertical");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!horizontal || !vertical || !device || ![device supportsRasterizationRateMapWithLayerCount:1]) {
            NSLog(@"foveation: no rates in %s, or the device does not take rate maps", path);
            return 0;
        }

        MTLRasterizationRateLayerDescriptor *layer = [[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake(horizontal.count, vertical.count, 1)];
        for (NSUInteger i = 0; i < horizontal.count; i++) layer.horizontalSampleStorage[i] = horizontal[i].floatValue;
        for (NSUInteger i = 0; i < vertical.count; i++) layer.verticalSampleStorage[i] = vertical[i].floatValue;
        MTLSize screen = MTLSizeMake(out->screen_width, out->screen_height, 0);
        MTLRasterizationRateMapDescriptor *descriptor =
            [MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:screen layer:layer];
        descriptor.label = @"game foveation";
        id<MTLRasterizationRateMap> map = [device newRasterizationRateMapWithDescriptor:descriptor];
        if (!map) {
            NSLog(@"foveation: the device refused the map");
            return 0;
        }

        MTLSize physical = [map physicalSizeForLayer:0];
        g_map = map;
        out->physical_width = (uint32_t)physical.width;
        out->physical_height = (uint32_t)physical.height;
        out->mtl_rate_map = (__bridge void *)map;
        NSLog(@"foveation: %ux%u through a %lux%lu map (%.2f of the pixels)", out->screen_width, out->screen_height,
              (unsigned long)physical.width, (unsigned long)physical.height,
              (double)(physical.width * physical.height) / ((double)out->screen_width * out->screen_height));
        if (const char *table = getenv("LB_GAME_FOVEATION_LUT")) write_table(map, screen, physical, table);
        return 1;
    }
}

__attribute__((constructor)) void register_provider() {
    if (getenv("LB_GAME_FOVEATION_MAP")) skyrim_foveation_set_provider(provide, nullptr);
}

}  // namespace
