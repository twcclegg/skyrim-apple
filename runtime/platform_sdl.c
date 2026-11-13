/* Host platform layer on SDL2: window (Vulkan surface for DXVK), event pump.
 * The same code runs on desktop Linux and on Android. */
#include "game.h"
#include "rt.h"
#include <SDL2/SDL.h>
#ifndef __ANDROID__
#include <fcntl.h>
#endif
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread.h>
#include <pthread/qos.h>
/* Apple input policy (input.c, dinput.c) on macOS, iOS, iPadOS and the iPad
 * Simulator: DirectInput keyboard and mouse are tracked from the events pumped
 * here, and controllers attached at launch are XInput pads from the first
 * poll. Touches stay touches (the LB_TOUCH overlay only). */
#define LB_APPLE_INPUT 1
#if !TARGET_OS_IPHONE
#include <pthread.h>
/* AppKit only processes events on the process main thread. Skyrim presents
 * from its loading-screen thread too, so other threads drain SDL's queue
 * without pumping, and the main thread (which presents in menus and play)
 * pumps. Android and the iOS targets keep their existing behaviour. */
#define LB_MAC_MAIN_PUMP 1
#elif !TARGET_OS_SIMULATOR || TARGET_OS_VISION
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
/* Physical iPhone/iPad and Vision Pro (and the visionOS Simulator, which
 * has no separate frontend): the game runs inside the launcher app on its main
 * thread (ipad/native_bridge.mm). The launcher pauses it (input gate),
 * suspends it while in the background (no GPU work is allowed there) and can
 * end it; all three take effect at the next presented frame. */
#define LB_IOS_DEVICE 1
#endif
#endif

static SDL_Window *g_window;
static int g_frames;
static void load_key_script(void);

#ifdef LB_APPLE_INPUT
void dinput_handle_event(const SDL_Event *e);
void input_open_attached(void);
#endif
#if defined(LB_MAC_MAIN_PUMP) || defined(LB_IOS_DEVICE)
/* The game reads relative DirectInput motion and draws its own menu cursor:
 * capture the pointer once the window has keyboard focus. SDL releases it on
 * Cmd-Tab and restores it on return (on iPadOS it locks the pointer through
 * GCMouse). Main thread only. LB_MOUSE_CAPTURE=0 leaves the pointer free. */
static void capture_mouse(void) {
    static int wanted = -1;
    if (wanted < 0) {
        const char *v = getenv("LB_MOUSE_CAPTURE");
        wanted = !(v && *v == '0');
        if (!wanted) LOG("mouse: capture disabled (LB_MOUSE_CAPTURE=0)");
    }
    if (!wanted || !g_window || SDL_GetRelativeMouseMode() || SDL_GetKeyboardFocus() != g_window) return;
    if (SDL_SetRelativeMouseMode(SDL_TRUE) == 0) LOG("mouse: captured for the game window");
    else { LOG("mouse: relative mode unavailable: %s", SDL_GetError()); wanted = 0; }
}
#endif

/* Mouse id of the motion events edge_turn synthesizes (SDL_TOUCH_MOUSEID is (Uint32)-1). */
#define LB_EDGE_TURN_MOUSEID ((Uint32)-2)

#if defined(LB_IOS_DEVICE) && TARGET_OS_VISION
/* A spatial click on the immersive layer (an indirect pinch, which is also how a trackpad click can arrive):
 * a left button, unless GCMouse pressed a button within 0.25 s (the same click arriving both ways). A press that
 * was skipped skips its release too. Any thread. */
double skyrim_xr_last_mouse_press(void);
void dinput_inject_button(int b, int down);
void skyrim_spatial_click(int down) {
    static atomic_int pressed;
    if (down) {
        if (atomic_load(&pressed)) return;  /* an active pinch reports every update */
        extern double CACurrentMediaTime(void);
        if (CACurrentMediaTime() - skyrim_xr_last_mouse_press() < 0.25) return;
        atomic_store(&pressed, 1);
        dinput_inject_button(0, 1);
        LOG("pointer: spatial click");
    } else if (atomic_exchange(&pressed, 0)) {
        dinput_inject_button(0, 0);
    }
}

/* The SDL overlay's edge turn (ipad/visionos/sdl/SDL_uikit_xros.m): while the pointer is pushed against a side of
 * the window, feed the game the motion it can no longer get. LB_EDGE_TURN is the full rate in mouse counts per
 * second (0 turns it off). Main thread only. */
float skyrim_xr_pointer_edge(void);
static void edge_turn(void) {
    static double rate = -1, last, carry;
    if (rate < 0) {
        const char *v = getenv("LB_EDGE_TURN");
        rate = v && *v ? atof(v) : 2400.0;
        LOG("mouse: edge turn at %.0f counts/s", rate);
    }
    double now = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    double dt = last > 0 && now - last < 0.1 ? now - last : 0;
    last = now;
    float edge = skyrim_xr_pointer_edge();
    if (!rate || !edge || !g_window || !SDL_GetRelativeMouseMode()) { carry = 0; return; }
    carry += edge * rate * dt;
    int dx = (int)carry;
    if (!dx) return;
    carry -= dx;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = SDL_GetWindowID(g_window);
    e.motion.which = LB_EDGE_TURN_MOUSEID;  /* synthetic: head steering must not read it as the player's mouse */
    e.motion.xrel = dx;
    SDL_PushEvent(&e);
}
#endif

#if defined(__APPLE__)
/* Head steering (docs/HEAD_TRACKING.md): turns the game's view toward the head's direction with synthetic mouse
 * look, closed-loop on the camera the game last presented, so nothing drifts. The immersive compositor publishes
 * the head's yaw and pitch (radians; yaw clockwise from the starting direction, pitch up) through
 * skyrim_head_set. LB_HEAD_TEST=<yaw deg>,<pitch deg> or LB_HEAD_TEST=sweep fakes a head for the Mac build.
 * With a pad connected (the game is then in gamepad mode and ignores mouse look) it deflects the right stick
 * instead: LB_HEAD_STICK_GAIN = full deflection per radian of error, LB_HEAD_STICK_DEAD = the game's dead zone.
 * Without one: LB_HEAD_COUNTS = mouse counts per radian of game turn, LB_HEAD_GAIN = fraction sent per frame.
 * Off while a menu is open. Real mouse motion takes over and re-anchors the head's straight-ahead to wherever the game then points.
 * Main thread only, except skyrim_head_set. */
#include <math.h>
#include <stdatomic.h>
int skyrim_frame_camera(float out[18]);  /* runtime/d3d9_bridge.cpp */
int skyrim_menu_open(void);
static _Atomic float g_head_yaw, g_head_pitch;
static atomic_int g_head_on;
void skyrim_head_set(float yaw, float pitch, int on) {
    atomic_store(&g_head_yaw, yaw);
    atomic_store(&g_head_pitch, pitch);
    atomic_store(&g_head_on, on);
}
static int g_head_external;  /* frames left during which real mouse motion owns the view */
static _Atomic float g_head_offset;  /* game heading minus head yaw: where the head's straight-ahead points */
static atomic_int g_head_anchored;
/* For the compositor: the game heading (radians) that the head's yaw 0 maps to, or 0 with *valid = 0. */
float skyrim_head_offset(int *valid) { *valid = atomic_load(&g_head_anchored); return atomic_load(&g_head_offset); }
static void head_note_event(const SDL_Event *e) {
    if (e->type == SDL_MOUSEMOTION && e->motion.which != SDL_TOUCH_MOUSEID
        && e->motion.which != LB_EDGE_TURN_MOUSEID && (e->motion.xrel || e->motion.yrel))
        g_head_external = 15;
}
static float head_wrap(float a) { return a - 6.2831853f * floorf((a + 3.1415927f) / 6.2831853f); }
static void head_steer(void) {
    static int init, test, logged;
    static float counts, gain, stick_gain, stick_dead, test_yaw, test_pitch, offset, carry_x, carry_y;
    static int anchored, frame;
    if (!init) {
        init = 1;
        const char *t = getenv("LB_HEAD_TEST");
        if (t && !strcmp(t, "sweep")) test = 2;
        else if (t && sscanf(t, "%f,%f", &test_yaw, &test_pitch) >= 1) test = 1;
        counts = getenv("LB_HEAD_COUNTS") ? (float)atof(getenv("LB_HEAD_COUNTS")) : 1000.0f;
        gain = getenv("LB_HEAD_GAIN") ? (float)atof(getenv("LB_HEAD_GAIN")) : 0.2f;
        stick_gain = getenv("LB_HEAD_STICK_GAIN") ? (float)atof(getenv("LB_HEAD_STICK_GAIN")) : 7.0f;
        stick_dead = getenv("LB_HEAD_STICK_DEAD") ? (float)atof(getenv("LB_HEAD_STICK_DEAD")) : 0.25f;
        if (test) LOG("head: test %s, %.0f counts/rad, gain %.2f", t, counts, gain);
    }
    frame++;
    float head_yaw, head_pitch;
    if (test == 1) { head_yaw = test_yaw * 0.017453293f; head_pitch = test_pitch * 0.017453293f; }
    else if (test == 2) { float t = frame / 60.0f; head_yaw = 0.6f * sinf(t * 0.8f); head_pitch = 0.25f * sinf(t * 0.5f); }
    else if (atomic_load(&g_head_on)) { head_yaw = atomic_load(&g_head_yaw); head_pitch = atomic_load(&g_head_pitch); }
    else { if (anchored) { void input_head_stick(float, float); input_head_stick(0, 0); } anchored = 0; atomic_store(&g_head_anchored, 0); return; }
    float cam[18];
    if (!g_window) return;
    void input_head_stick(float x, float y);
    /* Menus, loading screens and any frame without the world camera: let go of the stick (a held deflection would
     * scroll the menu) and re-anchor once the world is back. A menu frame carries no camera either. */
    if (skyrim_menu_open() || !skyrim_frame_camera(cam)) {
        anchored = 0;
        atomic_store(&g_head_anchored, 0);
        input_head_stick(0, 0);
        return;
    }
    /* Columns of the camera rotation: forward (cam[0], cam[3], cam[6]); Skyrim heading is clockwise from +Y. */
    float game_yaw = atan2f(cam[0], cam[3]);
    float game_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, cam[6])));
    if (!anchored || g_head_external > 0) {
        offset = head_wrap(game_yaw - head_yaw);
        atomic_store(&g_head_offset, offset);
        atomic_store(&g_head_anchored, 1);
        anchored = 1;
        carry_x = carry_y = 0;
        if (g_head_external > 0) g_head_external--;
        return;
    }
    float err_yaw = head_wrap(head_yaw + offset - game_yaw);
    float err_pitch = fmaxf(-1.1f, fminf(1.1f, head_pitch)) - game_pitch;
    if (fabsf(game_pitch) > 1.2f) err_yaw = 0;  /* looking nearly straight up or down: the heading is meaningless */
    void input_head_stick(float x, float y);
    int input_pad_connected(void), input_right_stick_moved(void);
    if (input_right_stick_moved()) { g_head_external = 15; input_head_stick(0, 0); return; }
    if (input_pad_connected()) {
        /* Gamepad mode: the stick sets a turn rate. Proportional, starting just past the game's dead zone. */
        float sx = err_yaw * stick_gain, sy = err_pitch * stick_gain;
        float mx = fabsf(sx) > 0.003f ? copysignf(stick_dead + (1 - stick_dead) * fminf(1, fabsf(sx)), sx) : 0;
        float my = fabsf(sy) > 0.003f ? copysignf(stick_dead + (1 - stick_dead) * fminf(1, fabsf(sy)), sy) : 0;
        input_head_stick(mx, my);
        if (test && frame - logged >= 30) {
            logged = frame;
            LOG("head: frame %d target %.1f/%.1f game %.1f/%.1f err %.2f/%.2f deg stick %.2f,%.2f", frame,
                (head_yaw + offset) * 57.29578f, head_pitch * 57.29578f, game_yaw * 57.29578f, game_pitch * 57.29578f,
                err_yaw * 57.29578f, err_pitch * 57.29578f, mx, my);
        }
        return;
    }
    input_head_stick(0, 0);
    /* Mouse path. How far the game turns per count depends on the player's sensitivity setting, so the gain is
     * kept low (stable up to several times the default sensitivity; on the Mac at the default it follows a 27
     * deg/s sweep about 10 deg behind) and each frame's motion is capped. Y inversion isn't handled. */
    static float prev_err, ysign = 1, win_dy, win_pitch0;
    static int diverging, win_n;
    const float kx = counts, ky = counts;
    /* Inverted Y (the game's Invert Y Axis option): over 20-frame windows, if the pitch moved against the
     * vertical motion sent, flip the sign. Uninverted, mouse down (dy > 0) looks down. */
    if (win_n == 0) { win_pitch0 = game_pitch; win_dy = 0; }
    if (++win_n >= 20) {
        float tilted = game_pitch - win_pitch0;
        /* expected: dy > 0 tilts down (tilted < 0) when ysign = 1, up when ysign = -1 */
        if (fabsf(win_dy) > 40 && fabsf(tilted) > 0.02f && (tilted > 0) == ((win_dy > 0) == (ysign > 0))) {
            ysign = -ysign;
            LOG("head: vertical mouse looks inverted; flipping (now %s)", ysign > 0 ? "normal" : "inverted");
        }
        win_n = 0;
    }
    /* Safety: an error that keeps growing past 40 degrees means the loop is fighting the game; stop and re-anchor. */
    float err_abs = fabsf(err_yaw) + fabsf(err_pitch);
    diverging = err_abs > 0.7f && err_abs > prev_err ? diverging + 1 : 0;
    prev_err = err_abs;
    if (diverging > 30) {
        LOG("head: mouse steering diverged (error %.0f deg); re-anchoring", err_abs * 57.3f);
        diverging = 0; anchored = 0; atomic_store(&g_head_anchored, 0);
        return;
    }
    /* The gain is per 60 Hz frame: scale by this frame's length so a frame cap (LB_MAX_FPS=30/45) responds as fast. */
    static double prev_t;
    double t_now = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    float frame_scale = prev_t > 0 ? (float)((t_now - prev_t) * 60.0) : 1.0f;
    prev_t = t_now;
    frame_scale = fmaxf(0.5f, fminf(3.0f, frame_scale));
    carry_x += err_yaw * kx * gain * frame_scale;
    carry_y -= err_pitch * ky * gain * ysign * frame_scale;   /* mouse down = look down, unless inverted */
    float cap = 40;
    carry_x = fmaxf(-cap, fminf(cap, carry_x));
    carry_y = fmaxf(-cap, fminf(cap, carry_y));
    int dx = (int)carry_x, dy = (int)carry_y;
    carry_x -= dx;
    carry_y -= dy;
    win_dy += dy;
    if (frame - logged >= (test ? 30 : 600)) {
        logged = frame;
        LOG("head: mouse target %.1f/%.1f game %.1f/%.1f err %.2f/%.2f deg sent %d,%d (%.0f counts/rad/frame)",
            (head_yaw + offset) * 57.29578f, head_pitch * 57.29578f, game_yaw * 57.29578f, game_pitch * 57.29578f,
            err_yaw * 57.29578f, err_pitch * 57.29578f, dx, dy, kx * gain);
    }
    if (!dx && !dy) return;
    void dinput_inject_mouse(int dx, int dy);
    dinput_inject_mouse(dx, dy);
}
#endif

/* Frame pacing for the device log, every 300 frames: whole-frame time, and the part spent inside D3D9 Present
 * (where the game blocks when the GPU is behind). Long frames with short Presents point at the CPU. */
#if defined(LB_IOS_DEVICE) || defined(LB_MAC_MAIN_PUMP)
#define LB_PACE_LOG 1
static double g_present_began, g_present_sum, g_present_max, g_present_last, g_slept_last;
static int g_draws_last;
void platform_present_slept(double ms) { g_slept_last = ms; }
void platform_present_draws(int draws) { g_draws_last = draws; }
static double pace_now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
void platform_present_begin(void) { g_present_began = pace_now_ms(); }
void platform_present_end(void) {
    double took = pace_now_ms() - g_present_began;
    g_present_last = took;
    g_present_sum += took;
    if (took > g_present_max) g_present_max = took;
}
/* Benchmark runs (tools/bench): LB_FRAMETIMES=1 logs every frame as
 * "ft <frame> <frame ms> <Present ms> <work ms> <draw calls>", where work is the frame time without the 60 fps
 * pacing sleep and without Present (the game's own CPU time);
 * LB_EXIT_FRAME=N ends the game cleanly once frame N is presented. */
#ifdef LB_IOS_DEVICE
void platform_native_request_quit(void);
#else
/* the Mac build (macos/, used for benchmarking the lifted code): no launcher to ask, leave directly */
static void platform_native_request_quit(void) { fflush(NULL); _exit(0); }
#endif
static void bench_frame(double took) {
    static int frametimes = -1, exit_frame = -1;
    if (frametimes < 0) {
        const char *v = getenv("LB_FRAMETIMES");
        frametimes = v && *v == '1';
        v = getenv("LB_EXIT_FRAME");
        exit_frame = v && *v ? atoi(v) : 0;
        if (frametimes || exit_frame) LOG("bench: frame times %s, exit at frame %d", frametimes ? "on" : "off", exit_frame);
    }
    if (frametimes) {
        double work = took - g_present_last - g_slept_last;
        LOG("ft %d %.2f %.2f %.2f %d", g_frames, took, g_present_last, work > 0 ? work : 0, g_draws_last);
    }
    g_slept_last = 0;
    if (exit_frame > 0 && g_frames == exit_frame) {
        LOG("bench: exit frame %d reached", g_frames);
        platform_native_request_quit();
    }
}
static void pace_frame(void) {
    static double last, sum, worst;
    static int n, over33, over50;
    double now = pace_now_ms();
    if (last > 0) {
        double took = now - last;
        sum += took;
        if (took > worst) worst = took;
        n++;
        over33 += took > 33.4;
        over50 += took > 50.0;
        bench_frame(took);
    } else {
        bench_frame(0);
    }
    last = now;
    if (n >= 300) {
        LOG("pacing: frame %.1f ms avg (%.0f fps), worst %.1f, %d over 33 ms, %d over 50 ms; Present %.1f ms avg, worst %.1f",
            sum / n, 1000.0 * n / sum, worst, over33, over50, g_present_sum / n, g_present_max);
        n = over33 = over50 = 0;
        sum = worst = g_present_sum = g_present_max = 0;
    }
}
#else
void platform_present_begin(void) {}
void platform_present_end(void) {}
void platform_present_slept(double ms) { (void)ms; }
void platform_present_draws(int draws) { (void)draws; }
#endif

#ifdef LB_IOS_DEVICE
static atomic_int g_native_pause, g_native_suspend, g_native_quit, g_main_holding;
/* ipad/native_bridge.mm: give the launcher's own window the keyboard while the game's is hidden. */
void skyrim_native_game_window_hidden(void);
void platform_native_set_paused(int on) { atomic_store(&g_native_pause, on); }
void platform_native_set_suspended(int on) { atomic_store(&g_native_suspend, on); }
void platform_native_request_quit(void) { atomic_store(&g_native_quit, 1); }

static void native_quit_if_requested(void) {
    if (!atomic_load(&g_native_quit)) return;
    LOG("quit requested by the launcher");
    fflush(NULL);
    _exit(0);  /* skip static teardown under running guest threads, as on the Mac */
}

/* Current protections across the 4 GiB guest reservation, as evidence that no
 * guest page is ever executable. */
static void native_log_guest_memory(void) {
    vm_address_t addr = (vm_address_t)MEM, end = addr + (1ull << 32);
    unsigned regions = 0;
    unsigned long long none = 0, rw = 0, other = 0, exec = 0;
    while (addr < end) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        vm_address_t probe = addr;
        if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS || addr >= end) break;
        if (addr < probe) {  /* a region reaching into the guest range from below */
            size -= probe - addr;
            addr = probe;
        }
        vm_size_t span = addr + size > end ? end - addr : size;
        regions++;
        if (info.protection & VM_PROT_EXECUTE) exec += span;
        else if (info.protection == VM_PROT_NONE) none += span;
        else if (info.protection == (VM_PROT_READ | VM_PROT_WRITE)) rw += span;
        else other += span;
        addr += size;
    }
    LOG("guest memory: %u regions in [%p, +4 GiB): none %llu, read/write %llu, other %llu, executable %llu bytes",
        regions, (void *)MEM, none, rw, other, exec);
}

/* Holds frames while the launcher has the game paused or the app is inactive
 * (no GPU work is allowed in the background). On the main thread a pause hides
 * the game window and releases the pointer, so the launcher's own Paused
 * screen (with Resume) shows and gets the keyboard, and keeps UIKit's run loop
 * turning. Other presenting threads always wait out a suspension, but wait
 * out a pause only once the main thread holds too: before then, Resume isn't
 * reachable and the main thread may be waiting on them. */
static int native_should_hold(int main_thread) {
    if (atomic_load(&g_native_suspend)) return 1;
    return atomic_load(&g_native_pause) && (main_thread || atomic_load(&g_main_holding));
}

static void native_hold_frames(void) {
    native_quit_if_requested();
    int main_thread = pthread_main_np(), hidden = 0;
    if (!native_should_hold(main_thread)) return;
    LOG("platform: frames held on the %s thread (%s)", main_thread ? "main" : "secondary",
        atomic_load(&g_native_suspend) ? "app inactive" : "paused by the launcher");
    if (main_thread) atomic_store(&g_main_holding, 1);
    while (native_should_hold(main_thread)) {
        native_quit_if_requested();
        if (!main_thread) {
            usleep(20000);
            continue;
        }
        if (atomic_load(&g_native_pause) && !hidden && g_window) {
            SDL_SetRelativeMouseMode(SDL_FALSE);  /* capture_mouse takes it again after the show */
            SDL_HideWindow(g_window);
            skyrim_native_game_window_hidden();
            hidden = 1;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }
    if (main_thread) atomic_store(&g_main_holding, 0);
    /* SDL_ShowWindow makes the window key and visible again. (SDL_RaiseWindow must not
     * be used here: on UIKit it rebinds a GL context, and this build has no GL.) */
    if (hidden) SDL_ShowWindow(g_window);
    /* Buttons released while the window was hidden never reached the game: release
     * everything the way a focus loss does, then drop the input queued meanwhile. */
    SDL_Event lost = {0};
    lost.type = SDL_WINDOWEVENT;
    lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    dinput_handle_event(&lost);
    SDL_FlushEvents(SDL_KEYDOWN, SDL_MOUSEWHEEL);
    LOG("platform: frames resumed on the %s thread", main_thread ? "main" : "secondary");
}
#endif

void platform_init(void) {
#if defined(__APPLE__)
    /* LB_QOS=1: the game's main thread (it also presents) in the user-interactive QoS class; see kernel32.c. */
    if (getenv("LB_QOS") && (*getenv("LB_QOS") >= '1' && *getenv("LB_QOS") <= '3')) {
        int r = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        LOG("qos: main thread user-interactive (%d)", r);
    }
#endif
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    int w = 1280, h = 720;
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
#ifdef LB_IOS_DEVICE
    flags |= SDL_WINDOW_BORDERLESS;  /* no status bar over the game */
#endif
    if (getenv("LB_WINDOW")) sscanf(getenv("LB_WINDOW"), "%dx%d", &w, &h);  /* desktop testing */
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
#endif
    g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
#ifdef LB_APPLE_INPUT
    input_open_attached();
#endif
    extern void user32_set_screen(int, int);
    user32_set_screen(w, h);
    LOG("platform: window %dx%d", w, h);
    load_key_script();
}

void *platform_window(void) { return g_window; }

/* Scripted input for automated runs: LB_KEYS="frame:Key[:frames],..." holds
 * the SDL-named key (e.g. Return, Space, Left) from that frame on. */
typedef struct { int frame, len; SDL_Scancode sc; int text; } KeyEv;
static KeyEv g_script[256];
static int g_nscript;
static Uint8 g_keys[SDL_NUM_SCANCODES];
static int g_padonly;  /* LB_PADONLY=1: scripted keys drive only the virtual XInput pad */

static void load_key_script(void) {
    const char *s = getenv("LB_KEYS");
    g_padonly = getenv("LB_PADONLY") && *getenv("LB_PADONLY") == '1';
    while (s && *s && g_nscript < 256) {
        char name[32] = "";
        int fr = 0, len = 10, n = 0;
        if (sscanf(s, "%d:%31[^:,]%n", &fr, name, &n) < 2) break;
        s += n;
        if (*s == ':') { s++; len = (int)strtol(s, (char **)&s, 10); }
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc != SDL_SCANCODE_UNKNOWN) g_script[g_nscript++] = (KeyEv){fr, len, sc, 0};
        else LOG("LB_KEYS: unknown key %s", name);
        if (*s == ',') s++;
    }
}

/* SDL scancode -> Windows virtual-key code */
static uint32_t vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    default: return 0;
    }
}

static void key_event(SDL_Scancode sc, int down) {
    extern void user32_key_event(uint32_t vk, uint32_t sc, int down);
    uint32_t vk = vk_of(sc);
    /* PC scan code: approximate with the DirectInput code, which matches set 1 */
    extern uint32_t dinput_dik_of(int sdl_scancode);
    user32_key_event(vk, dinput_dik_of(sc), down);
}

/* Emits key events for scripted keys whose state changed this frame. */
static void script_events(void) {
    static Uint8 prev[SDL_NUM_SCANCODES];
    if (!g_nscript) return;
    Uint8 cur[SDL_NUM_SCANCODES] = {0};
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) cur[g_script[i].sc] = 1;
    for (int i = 0; i < g_nscript; i++) {
        SDL_Scancode sc = g_script[i].sc;
        if (cur[sc] != prev[sc]) {
            if (!g_padonly || g_script[i].text) key_event(sc, cur[sc]);
            if (!g_script[i].text) {  /* typed text drives the keyboard only, not the virtual pad */
                SDL_Event e = {0};
                e.type = cur[sc] ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.keysym.scancode = sc;
                e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
                extern void input_handle_event(const SDL_Event *e);
                input_handle_event(&e);
            }
            prev[sc] = cur[sc];
        }
    }
    /* drop typed keys that are over (their release was just sent) */
    int n = 0;
    for (int i = 0; i < g_nscript; i++)
        if (!g_script[i].text || g_frames < g_script[i].frame + g_script[i].len) g_script[n++] = g_script[i];
    g_nscript = n;
}

/* Phone keyboard: the keyboard button (touch.cpp) shows or hides it. The game
 * reads keys only through DirectInput (and turns them into characters with
 * ToUnicode), so each typed character becomes a short scripted key press,
 * with Shift held around capitals and symbols, one after another. */
static int g_text_on, g_text_hidden_frames, g_text_next;
#ifdef LB_MAC_MAIN_PUMP
static volatile int g_text_apply;
#endif
int platform_text_input(int toggle) {
    if (toggle < 0) return g_text_on;
    g_text_on = !g_text_on;
    g_text_hidden_frames = 0;
#ifdef LB_MAC_MAIN_PUMP
    if (!pthread_main_np()) {
        g_text_apply = 1;  /* AppKit text input changes on the main thread's next present */
        LOG("keyboard %s (deferred to main thread)", g_text_on ? "shown" : "hidden");
        return g_text_on;
    }
#endif
    if (g_text_on) SDL_StartTextInput(); else SDL_StopTextInput();
    LOG("keyboard %s", g_text_on ? "shown" : "hidden");
    return g_text_on;
}
static void type_key(SDL_Scancode sc, int shift) {
    if (g_nscript > 250) return;
    if (g_text_next < g_frames + 1) g_text_next = g_frames + 1;
    if (shift) {
        g_script[g_nscript++] = (KeyEv){g_text_next, 4, SDL_SCANCODE_LSHIFT, 1};
        g_script[g_nscript++] = (KeyEv){g_text_next + 1, 2, sc, 1};
        g_text_next += 5;
    } else {
        g_script[g_nscript++] = (KeyEv){g_text_next, 2, sc, 1};
        g_text_next += 3;
    }
}
/* US layout */
static void type_char(unsigned ch) {
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct { char c, s; SDL_Scancode sc; } sym[] = {
        {' ', 0, SDL_SCANCODE_SPACE}, {'-', 0, SDL_SCANCODE_MINUS}, {'_', 1, SDL_SCANCODE_MINUS},
        {'=', 0, SDL_SCANCODE_EQUALS}, {'+', 1, SDL_SCANCODE_EQUALS}, {'\'', 0, SDL_SCANCODE_APOSTROPHE},
        {'"', 1, SDL_SCANCODE_APOSTROPHE}, {',', 0, SDL_SCANCODE_COMMA}, {'<', 1, SDL_SCANCODE_COMMA},
        {'.', 0, SDL_SCANCODE_PERIOD}, {'>', 1, SDL_SCANCODE_PERIOD}, {'/', 0, SDL_SCANCODE_SLASH},
        {'?', 1, SDL_SCANCODE_SLASH}, {';', 0, SDL_SCANCODE_SEMICOLON}, {':', 1, SDL_SCANCODE_SEMICOLON},
        {'[', 0, SDL_SCANCODE_LEFTBRACKET}, {']', 0, SDL_SCANCODE_RIGHTBRACKET}, {'`', 0, SDL_SCANCODE_GRAVE},
        {'~', 1, SDL_SCANCODE_GRAVE}, {'\n', 0, SDL_SCANCODE_RETURN},
    };
    if (ch >= 'a' && ch <= 'z') { type_key((SDL_Scancode)(SDL_SCANCODE_A + ch - 'a'), 0); return; }
    if (ch >= 'A' && ch <= 'Z') { type_key((SDL_Scancode)(SDL_SCANCODE_A + ch - 'A'), 1); return; }
    if (ch == '0') { type_key(SDL_SCANCODE_0, 0); return; }
    if (ch >= '1' && ch <= '9') { type_key((SDL_Scancode)(SDL_SCANCODE_1 + ch - '1'), 0); return; }
    for (int i = 0; i < 10; i++)
        if (ch == (unsigned char)shifted_digits[i]) { type_key(i ? (SDL_Scancode)(SDL_SCANCODE_1 + i - 1) : SDL_SCANCODE_0, 1); return; }
    for (unsigned i = 0; i < sizeof sym / sizeof sym[0]; i++)
        if (ch == (unsigned char)sym[i].c) { type_key(sym[i].sc, sym[i].s); return; }
}
/* While the phone keyboard is up: characters arrive as SDL_TEXTINPUT (the
 * key events SDL makes up for them are pressed and released at once, too
 * fast for DirectInput polling), Backspace and Enter as key presses. Returns
 * 1 when the event was used. */
static int text_event(const SDL_Event *e) {
    if (!g_text_on) return 0;
    if (e->type == SDL_TEXTINPUT) {
        for (const unsigned char *p = (const unsigned char *)e->text.text; *p; p++)
            if (*p < 0x80) type_char(*p);
        return 1;
    }
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
        SDL_Scancode sc = e->key.keysym.scancode;
        int special = sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_ESCAPE ||
                      sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_DELETE ||
                      (sc >= SDL_SCANCODE_RIGHT && sc <= SDL_SCANCODE_UP);
        if (special && e->type == SDL_KEYDOWN && !e->key.repeat) type_key(sc, 0);
        return special || SDL_GetKeyFromScancode(sc) < 0x80 || sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT;
    }
    return 0;
}
const Uint8 *platform_keys(int *n);
/* Shift as the game should see it (ToUnicode gets the game's own key state). */
int platform_shift_down(void) {
    const Uint8 *ks = platform_keys(NULL);
    return ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
}

const Uint8 *platform_keys(int *n) {
    int nk;
    const Uint8 *ks = SDL_GetKeyboardState(&nk);
    if (!g_nscript || g_padonly) { if (n) *n = nk; return ks; }
    memcpy(g_keys, ks, nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES);
    for (int i = 0; i < g_nscript; i++)
        if (g_frames >= g_script[i].frame && g_frames < g_script[i].frame + g_script[i].len) g_keys[g_script[i].sc] = 1;
    if (n) *n = SDL_NUM_SCANCODES;
    return g_keys;
}

/* Interactive test input: LB_KEYFIFO=path, lines "Key[:frames]" are queued
 * at the current frame (desktop testing only). */
static void poll_key_fifo(void) {
#ifndef __ANDROID__
    static FILE *fifo;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *p = getenv("LB_KEYFIFO");
        if (p) {
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) fifo = fdopen(fd, "r");
        }
    }
    if (!fifo) return;
    char line[64];
    while (fgets(line, sizeof line, fifo)) {
        char name[32] = "";
        int len = 8;
        if (!strncmp(line, "dump ", 5)) {  /* "dump <file>": save the exe's data sections */
            line[strcspn(line, "\n")] = 0;
            FILE *d = fopen(line + 5, "wb");
            if (d) { fwrite(MEM + 0x934000, 1, 0x20E2490, d); fclose(d); }
            LOG("keyfifo: dumped data to %s at frame %d", line + 5, g_frames);
            continue;
        }
        if (sscanf(line, "%31[^:\n]:%d", name, &len) < 1) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        if (sc == SDL_SCANCODE_UNKNOWN || g_nscript >= 256) continue;
        g_script[g_nscript++] = (KeyEv){g_frames + 1, len, sc, 0};
        LOG("keyfifo: %s at frame %d", name, g_frames + 1);
    }
    clearerr(fifo);
#endif
}

/* SDL_PollEvent pumps the OS event loop; SDL_PeepEvents only reads SDL's
 * locked queue, which is safe from any thread. */
static int next_event(SDL_Event *e) {
#ifdef LB_MAC_MAIN_PUMP
    if (!pthread_main_np()) return SDL_PeepEvents(e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0;
    if (g_text_apply) {
        g_text_apply = 0;
        if (g_text_on) SDL_StartTextInput(); else SDL_StopTextInput();
    }
#endif
    return SDL_PollEvent(e);
}

void platform_present_hook(void) {
    SDL_Event e;
    poll_key_fifo();
    while (next_event(&e)) {
        if (e.type == SDL_QUIT) {
            LOG("quit requested");
            fflush(NULL);
#if defined(LB_MAC_MAIN_PUMP) || defined(LB_IOS_DEVICE)
            _exit(0);  /* any thread may drain SDL_QUIT; skip static teardown under running guest threads */
#else
            exit(0);
#endif
        }
#ifdef LB_APPLE_INPUT
        dinput_handle_event(&e);
#endif
#if defined(__APPLE__)
        head_note_event(&e);
#endif
        if (text_event(&e)) continue;
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
            key_event(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
        extern void input_handle_event(const SDL_Event *e);
        input_handle_event(&e);
        extern void touch_handle_event(const SDL_Event *e);
        touch_handle_event(&e);
    }
    script_events();
#if defined(__APPLE__)
    if (pthread_main_np()) head_steer();
#endif
#ifdef LB_MAC_MAIN_PUMP
    if (pthread_main_np()) capture_mouse();
    pace_frame();
#endif
#ifdef LB_IOS_DEVICE
    if (pthread_main_np()) capture_mouse();
#if TARGET_OS_VISION
    if (pthread_main_np()) edge_turn();
#endif
    pace_frame();
    static atomic_int first_frame_logged;
    if (!atomic_exchange(&first_frame_logged, 1)) {
        LOG("platform: first frame presented (%s thread)", pthread_main_np() ? "main" : "secondary");
        native_log_guest_memory();
    }
    native_hold_frames();
#endif
#ifdef __ANDROID__
    /* closed with the phone's Back key or its own button: follow it */
    if (g_text_on) {
        g_text_hidden_frames = SDL_IsScreenKeyboardShown(g_window) ? 0 : g_text_hidden_frames + 1;
        if (g_text_hidden_frames > 60) platform_text_input(1);
    }
#endif
    g_frames++;
#ifdef GAME_SKYRIM
    /* bGamepadEnable (Controls menu toggle, not read from the INI at boot):
     * always on, the phone's pad and touch overlay are XInput pads. */
    M8(0x12C3650) = 1;
#endif
    if (g_frames % 300 == 0) LOG("frame %d", g_frames);
    {   /* canary: the main thread's static TLS pointer (TEB slot 0 +0x2C) */
        static uint32_t last;
        uint32_t v = M32(GUEST_TEB_BASE + 0x2C);
        if (last && v != last)
            fprintf(stderr, "frame %d: main TEB+0x2C changed %08x -> %08x (TEB+0x24 tid %u, +0x1000 %08x)\n",
                    g_frames, last, v, M32(GUEST_TEB_BASE + 0x24), M32(GUEST_TEB_BASE + 0x1000));
        last = v;
    }
}
void platform_window_size(int *w, int *h) { if (g_window) SDL_GetWindowSize(g_window, w, h); }
int platform_frame_count(void) { return g_frames; }
