/* The physical iPad's native Skyrim runtime as the launcher sees it.
 *
 * The launcher app links this together with the runtime, the generated game code and the
 * device dependencies (SDL2, DXVK, MoltenVK, FFmpeg). The game runs in the launcher's process
 * on its main thread: SDL attaches its window to the launcher's UIKit scene and keeps the run
 * loop turning, so the launcher's own UI, input monitor and notifications stay live. No CPU
 * translator, JIT or executable guest memory is involved. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include <CommonCrypto/CommonDigest.h>
#include <SDL2/SDL.h>
#include <os/proc.h>
#include <pthread.h>
#include <atomic>
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#import <QuartzCore/QuartzCore.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "skyrim_native_identity.h"  /* generated from the verified gen/ (ipad/generate_identity.py) */

extern "C" {
int skyrim_guest_main(int, char **);
void platform_native_set_paused(int);
void platform_native_set_suspended(int);
void platform_native_request_quit(void);
}

static std::atomic<int> g_state{0};  /* 0 idle, 1 running */

#if TARGET_OS_VISION
/* LB_AUDIO_CHANNELS=6 (runtime/xaudio2.c) makes SDL open a 5.1 audio queue (L R C LFE Ls Rs). Tell the
 * audio session that the app plays multichannel content and how the system should spatialise it, before
 * SDL opens the device. LB_AUDIO_SPATIAL: "head" (default; the bed stays put in the room as the head
 * turns), "fixed" (follows the head) or "bypass" (no spatialisation). LB_AUDIO_STAGE: "small", "medium",
 * "large" (default) or "auto". SDL sets the session category later, when it opens the device. */
static void prepareSurroundSession(void) {
    const char *channels = getenv("LB_AUDIO_CHANNELS");
    if (!channels || strcmp(channels, "6")) return;
    const char *mode = getenv("LB_AUDIO_SPATIAL"), *stage = getenv("LB_AUDIO_STAGE");
    AVAudioSessionSpatialExperience experience = AVAudioSessionSpatialExperienceHeadTracked;
    if (mode && !strcmp(mode, "fixed")) experience = AVAudioSessionSpatialExperienceFixed;
    else if (mode && !strcmp(mode, "bypass")) experience = AVAudioSessionSpatialExperienceBypassed;
    AVAudioSessionSoundStageSize size = AVAudioSessionSoundStageSizeLarge;
    if (stage && !strcmp(stage, "small")) size = AVAudioSessionSoundStageSizeSmall;
    else if (stage && !strcmp(stage, "medium")) size = AVAudioSessionSoundStageSizeMedium;
    else if (stage && !strcmp(stage, "auto")) size = AVAudioSessionSoundStageSizeAutomatic;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *problem = nil;
    BOOL multichannel = [session setSupportsMultichannelContent:YES error:&problem];
    if (!multichannel) fprintf(stderr, "native: audio: multichannel content refused: %s\n", problem.localizedDescription.UTF8String);
    NSDictionary<AVAudioSessionSpatialExperienceOption, id> *options = nil;
    if (experience == AVAudioSessionSpatialExperienceHeadTracked)
        options = @{AVAudioSessionSpatialExperienceOptionSoundStageSize: @(size),
                    AVAudioSessionSpatialExperienceOptionAnchoringStrategy: @(AVAudioSessionAnchoringStrategyAutomatic)};
    else if (experience == AVAudioSessionSpatialExperienceFixed)
        options = @{AVAudioSessionSpatialExperienceOptionSoundStageSize: @(size)};
    problem = nil;
    BOOL spatial = [session setIntendedSpatialExperience:experience options:options error:&problem];
    if (!spatial) fprintf(stderr, "native: audio: spatial experience refused: %s\n", problem.localizedDescription.UTF8String);
    fprintf(stderr, "native: audio: 5.1 session: multichannel %d, spatial experience %ld (set %d), stage %ld; "
            "output channels %ld, maximum %ld\n", multichannel, (long)experience, spatial, (long)size,
            (long)session.outputNumberOfChannels, (long)session.maximumOutputNumberOfChannels);
}
#endif

static NSString *uniqueItem(NSString *directory, NSString *name) {
    NSArray<NSString *> *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:nil];
    NSString *match = nil;
    for (NSString *item in items) {
        if ([item caseInsensitiveCompare:name] != NSOrderedSame) continue;
        if (match) return nil;
        match = [directory stringByAppendingPathComponent:item];
    }
    return match;
}

static NSString *sha256(NSString *path) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (!file) return nil;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    const size_t chunk = 1 << 20;  /* per call: the launcher may validate off the main thread */
    unsigned char *buffer = (unsigned char *)malloc(chunk);
    size_t count;
    while (buffer && (count = fread(buffer, 1, chunk, file)) > 0) CC_SHA256_Update(&context, buffer, (CC_LONG)count);
    bool failed = !buffer || ferror(file);
    free(buffer);
    fclose(file);
    if (failed) return nil;
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(bytes, &context);
    char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (unsigned i = 0; i < sizeof bytes; i++) snprintf(text + i * 2, 3, "%02x", bytes[i]);
    return [NSString stringWithUTF8String:text];
}

static NSString *documents(void) {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

static int fail(char *error, size_t size, NSString *message) {
    if (error && size) snprintf(error, size, "%s", message.UTF8String);
    return 0;
}

/* The in-container installation the runtime needs: Documents/Skyrim with the exact Windows
 * modules and master files the generated code was built from, and its BSA archives. */
static int validate(NSString **gamePath, char *error, size_t error_size) {
    NSString *game = [documents() stringByAppendingPathComponent:@"Skyrim"];
    BOOL directory = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:game isDirectory:&directory] || !directory)
        return fail(error, error_size, @"Add your original Skyrim files to this app's Skyrim folder in Files (On My iPad › Skyrim › Skyrim).");
    for (const auto &module : skyrim_native_modules) {
        NSString *path = uniqueItem(game, @(module.name));
        if (!path || ![sha256(path) isEqualToString:@(module.sha256)])
            return fail(error, error_size, [NSString stringWithFormat:@"Skyrim/%s is missing or isn't the exact file this runtime was built from.", module.name]);
    }
    NSString *data = uniqueItem(game, @"Data");
    for (const auto &master : skyrim_native_essential_data) {
        NSString *path = data ? uniqueItem(data, @(master.name)) : nil;
        if (!path || ![sha256(path) isEqualToString:@(master.sha256)])
            return fail(error, error_size, [NSString stringWithFormat:@"Skyrim/Data/%s is missing or doesn't match this runtime's build inputs.", master.name]);
    }
    BOOL archive = NO;
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:data error:nil])
        if ([name.pathExtension caseInsensitiveCompare:@"bsa"] == NSOrderedSame) archive = YES;
    if (!archive) return fail(error, error_size, @"Skyrim/Data has no BSA archives. Copy the complete Data folder.");
    if (gamePath) *gamePath = game;
    return 1;
}

/* Optional per-launch overrides for verification runs (LB_SHOT, LB_LOG, MVK_*, ...), placed by
 * the developer at Documents/SkyrimNative/launch-env.json. Only these prefixes are accepted. */
static void applyLaunchEnvironment(void) {
    NSString *path = [documents() stringByAppendingPathComponent:@"SkyrimNative/launch-env.json"];
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class]) {
        fprintf(stderr, "native: ignoring %s (not a JSON object)\n", path.UTF8String);
        return;
    }
    for (NSString *key in json) {
        id value = json[key];
        BOOL allowed = [key hasPrefix:@"LB_"] || [key hasPrefix:@"DXVK_"] || [key hasPrefix:@"MVK_"] || [key hasPrefix:@"SDL_"];
        if (!allowed || ![value isKindOfClass:NSString.class]) {
            fprintf(stderr, "native: launch-env.json: ignoring %s\n", key.UTF8String);
            continue;
        }
        setenv(key.UTF8String, [value UTF8String], 1);
        fprintf(stderr, "native: launch-env.json: %s=%s\n", key.UTF8String, [value UTF8String]);
    }
}

static void logIdentity(NSString *game, NSString *saves) {
    struct utsname host;
    uname(&host);
    SDL_version linked;
    SDL_GetVersion(&linked);
    NSProcessInfo *process = NSProcessInfo.processInfo;
    fprintf(stderr, "native: Skyrim iPad runtime %s\n", SKYRIM_NATIVE_IDENTITY_JSON);
    fprintf(stderr, "native: device %s, %s, %llu MiB physical memory, %llu MiB available to this app, "
            "%lu active cores, low power %d\n",
            host.machine, process.operatingSystemVersionString.UTF8String,
            (unsigned long long)(process.physicalMemory >> 20), (unsigned long long)(os_proc_available_memory() >> 20),
            (unsigned long)process.activeProcessorCount, process.lowPowerModeEnabled);
    fprintf(stderr, "native: SDL %d.%d.%d; game %s; saves %s\n", linked.major, linked.minor, linked.patch,
            game.UTF8String, saves.UTF8String);
    /* The game's own main loop runs on this thread; only threads the game creates get the
     * runtime's 8 MiB GUEST_HOST_STACK. */
    fprintf(stderr, "native: game main loop on the %s thread, host stack %zu KiB; guest-created threads 8192 KiB\n",
            pthread_main_np() ? "main" : "secondary", pthread_get_stacksize_np(pthread_self()) >> 10);
}

extern "C" int skyrim_native_runtime_linked(void) { return 1; }

extern "C" const char *skyrim_native_identity(void) { return SKYRIM_NATIVE_IDENTITY_JSON; }

extern "C" int skyrim_native_state(void) { return g_state.load(); }

extern "C" int skyrim_native_validate(char *error, size_t error_size) {
    @autoreleasepool {
        return validate(nil, error, error_size);
    }
}

extern "C" void skyrim_native_set_paused(int paused) { platform_native_set_paused(paused); }

/* Called by the runtime (main thread) once it has hidden the game's window for a pause: the
 * launcher's own window takes the keyboard back, so its Paused screen and Resume work. */
extern "C" void skyrim_native_game_window_hidden(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (window.hidden || [NSStringFromClass(window.class) hasPrefix:@"SDL_"]) continue;
            [window makeKeyWindow];
            return;
        }
    }
}

extern "C" void skyrim_native_request_quit(void) { platform_native_request_quit(); }

/* Runs the game on the calling (main) thread until it ends; normally it ends the process. */
extern "C" int skyrim_native_run(const char *save_root, char *error, size_t error_size) {
    if (!NSThread.isMainThread) return fail(error, error_size, @"The game must start on the main thread.");
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return fail(error, error_size, @"Skyrim is already running.");
    NSString *game = nil, *saves = nil;
    NSString *logPath = nil;
    @autoreleasepool {
        if (!validate(&game, error, error_size)) {
            g_state = 0;
            return 0;
        }
        /* The runtime's first act is reserving 4 GiB of guest address space (read/write,
         * never executable); a refusal there would abort the app. Ask first and report it. */
        void *probe = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (probe == MAP_FAILED) {
            g_state = 0;
            return fail(error, error_size, [NSString stringWithFormat:
                @"This iPad refused the 4 GiB of address space Skyrim needs (%s). The app needs the Extended Virtual Addressing capability.",
                strerror(errno)]);
        }
        munmap(probe, 1ull << 32);
        saves = @(save_root);
        if (![[NSFileManager defaultManager] createDirectoryAtPath:saves withIntermediateDirectories:YES attributes:nil error:nil]) {
            g_state = 0;
            return fail(error, error_size, @"Couldn't create the save folder.");
        }
        NSString *logs = [documents() stringByAppendingPathComponent:@"SkyrimLogs"];
        [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
        NSDateFormatter *stamp = [NSDateFormatter new];
        stamp.dateFormat = @"yyyyMMdd'T'HHmmss'Z'";
        stamp.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
        logPath = [logs stringByAppendingPathComponent:[NSString stringWithFormat:@"game-%@.log", [stamp stringFromDate:NSDate.date]]];
        if (freopen(logPath.fileSystemRepresentation, "a", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);

        setenv("LB_LOG", "1", 0);
        setenv("LB_AUDIO_METER", "1", 0);
        applyLaunchEnvironment();
        NSString *config = [saves stringByAppendingPathComponent:@"dxvk.conf"];
        NSString *settings = @"dxvk.numCompilerThreads = 2\n"
                             // Same legacy D3D9 shader math as macos/runtime.conf (no NaN HDR/DoF output).
                             "d3d9.floatEmulation = True\n";
        if (![settings writeToFile:config atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
            g_state = 0;
            return fail(error, error_size, @"Couldn't write the renderer settings.");
        }
        setenv("DXVK_CONFIG_FILE", config.fileSystemRepresentation, 1);
        setenv("DXVK_STATE_CACHE_PATH", saves.fileSystemRepresentation, 0);
        setenv("DXVK_LOG_PATH", logs.fileSystemRepresentation, 0);
        /* GPU work rejected while the app goes inactive must not leave the device lost for good. */
        setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 0);
#if TARGET_OS_VISION
        setenv("LB_PRESENT_TAP", "1", 0);  /* read once at swapchain creation; idle until the compositor registers */
#endif
        logIdentity(game, saves);
        /* Thermal state, for reading frame times: a throttled run isn't comparable with a cool one. */
        static const char *const thermal[] = {"nominal", "fair", "serious", "critical"};
        NSInteger state = NSProcessInfo.processInfo.thermalState;
        fprintf(stderr, "native: thermal %s\n", state >= 0 && state < 4 ? thermal[state] : "unknown");
        [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil
                                                         queue:nil usingBlock:^(NSNotification *) {
            NSInteger now = NSProcessInfo.processInfo.thermalState;
            fprintf(stderr, "native: thermal %s\n", now >= 0 && now < 4 ? thermal[now] : "unknown");
        }];

        /* Battery, thermal state and the process's CPU use, every 10 s (LB_POWER_LOG_S=1 for every second, for a
         * battery soak), flushed to disk each time so the last state survives a sudden power-off. On a background
         * queue: the game owns the main thread. visionOS may or may not report the external battery; -1 means
         * unknown. */
        UIDevice.currentDevice.batteryMonitoringEnabled = YES;
        const char *every = getenv("LB_POWER_LOG_S");
        double interval = every && atof(every) > 0 ? atof(every) : 10.0;
        static dispatch_source_t power_timer;
        power_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(power_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(interval * NSEC_PER_SEC)),
                                  (uint64_t)(interval * NSEC_PER_SEC), NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(power_timer, ^{
            static const char *states[] = {"unknown", "unplugged", "charging", "full"};
            static double last_cpu, last_wall;
            struct rusage use;
            getrusage(RUSAGE_SELF, &use);
            double cpu = use.ru_utime.tv_sec + use.ru_utime.tv_usec / 1e6 + use.ru_stime.tv_sec + use.ru_stime.tv_usec / 1e6;
            double wall = CACurrentMediaTime();
            double cores = last_wall > 0 && wall > last_wall ? (cpu - last_cpu) / (wall - last_wall) : 0;
            last_cpu = cpu;
            last_wall = wall;
            UIDevice *device = UIDevice.currentDevice;
            NSInteger st = device.batteryState, th = NSProcessInfo.processInfo.thermalState;
            fprintf(stderr, "power: battery %.0f%% (%s), thermal %s, low power %d, cpu %.2f cores\n",
                    device.batteryLevel * 100.0, st >= 0 && st < 4 ? states[st] : "?",
                    th >= 0 && th < 4 ? thermal[th] : "unknown", NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0,
                    cores);
            fflush(stderr);
            fsync(STDERR_FILENO);
        });
        dispatch_resume(power_timer);

        /* The game holds its frames while the launcher is inactive: iOS allows no GPU work in
         * the background, and Control Center or the app switcher also deactivate it. */
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil
                        usingBlock:^(NSNotification *) {
            fprintf(stderr, "native: app resigning active\n");
            platform_native_set_suspended(1);
        }];
        [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil
                        usingBlock:^(NSNotification *) {
            fprintf(stderr, "native: app active\n");
            platform_native_set_suspended(0);
        }];
        /* SDL applies its idle-timer hint only from its own app delegate, which this app doesn't
         * use: keep the screen awake directly while the game runs. */
        UIApplication.sharedApplication.idleTimerDisabled = YES;
#if TARGET_OS_VISION
        prepareSurroundSession();
#endif
        SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetMainReady();
        /* Normally SDL's app delegate turns this on; it also registers SDL's app lifecycle
         * observer, which sends focus and app events (dinput releases held buttons on focus loss). */
        SDL_iPhoneSetEventPump(SDL_TRUE);
    }
    char executable[] = "Skyrim";
    char gameArgument[1024], saveArgument[1024];
    snprintf(gameArgument, sizeof gameArgument, "%s", game.fileSystemRepresentation);
    snprintf(saveArgument, sizeof saveArgument, "%s", saves.fileSystemRepresentation);
    char *arguments[] = {executable, gameArgument, saveArgument, nullptr};
    int status = skyrim_guest_main(3, arguments);
    /* The game's threads, window and device stay alive and SDL allows one window per display,
     * so this process can't start it again: end it as the game's own quit does. */
    fprintf(stderr, "native: game returned %d; ending the app\n", status);
    fflush(NULL);
    _exit(status);
}
