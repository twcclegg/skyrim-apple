#!/usr/bin/env python3
"""Create an owned SDL2 source overlay for the modern UIKit scene lifecycle.

The pinned vendor source stays untouched. These are native UIKit platform
adaptations, not a dynamic code translator or game patch.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
import fetch_sdl  # noqa: E402
SOURCE=fetch_sdl.SOURCE


def replace_once(text,before,after):
    if text.count(before)!=1:
        raise ValueError("Pinned SDL scene adaptation no longer matches its expected source")
    return text.replace(before,after,1)


XROS_SHIM=ROOT/"ipad/visionos/sdl"
XROS_INCLUDE='#include "../../SDL_internal.h"'
XROS_SYSURL_OPEN="return [[UIApplication sharedApplication] openURL:nsurl] ? 0 : -1;"
XROS_FRAME_SWAP="if (fullscreen && (landscape != (frame.size.width > frame.size.height))) {"
XROS_ALERT_WINDOW="alertwindow = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];"


def apply_visionos(output):
    """visionOS has no UIScreen or status bar: route SDL's UIKit backend through SDL_uikit_xros.h."""
    uikit=output/"src/video/uikit"
    changed={}
    for name in ("SDL_uikit_xros.h","SDL_uikit_xros.m"):
        shutil.copy2(XROS_SHIM/name,uikit/name)
    for path in sorted(uikit.glob("SDL_uikit*.m")):
        if path.name=="SDL_uikit_xros.m":
            continue
        before=path.read_text()
        path.write_text(replace_once(before,XROS_INCLUDE,XROS_INCLUDE+'\n#include "SDL_uikit_xros.h"'))
        changed[str(path.relative_to(output))]=before
    sysurl=output/"src/misc/ios/SDL_sysurl.m"
    before=sysurl.read_text()
    sysurl.write_text(replace_once(before,XROS_SYSURL_OPEN,'''#if TARGET_OS_VISION
        [[UIApplication sharedApplication] openURL:nsurl options:@{} completionHandler:nil];
        return 0;
#else
        '''+XROS_SYSURL_OPEN+'''
#endif'''))
    changed[str(sysurl.relative_to(output))]=before
    # A window is its own "screen" here: never swap a fullscreen frame for an orientation.
    video=uikit/"SDL_uikitvideo.m"
    before=video.read_text()
    video.write_text(replace_once(before,XROS_FRAME_SWAP,"if (!TARGET_OS_VISION && "+XROS_FRAME_SWAP[len("if ("):]))
    changed[str(video.relative_to(output))]=before
    # A UIWindow without a scene never shows on visionOS; attach the alert window to the game's scene.
    box=uikit/"SDL_uikitmessagebox.m"
    before=box.read_text()
    box.write_text(replace_once(before,XROS_ALERT_WINDOW,'''#if TARGET_OS_VISION
        alertwindow = [[UIWindow alloc] initWithWindowScene:SDLXR_ActiveScene()];
#else
        '''+XROS_ALERT_WINDOW+'''
#endif'''))
    changed[str(box.relative_to(output))]=before
    # Bind SDL's UIWindow to the game's own window scene. With UIApplicationSupportsMultipleScenes (needed for an
    # ImmersiveSpace) every scene matches the shim's single screen, so the scene loop below must not decide.
    window=uikit/"SDL_uikitwindow.m"
    text=window.read_text()
    window.write_text(replace_once(text,"""        UIWindowScene *window_scene = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {""","""        UIWindowScene *window_scene = SDLXR_GameScene();
        if (window_scene) {
            SDL_Log("SDL visionOS: game window attached to the registered game scene %s", window_scene.session.persistentIdentifier.UTF8String);
        } else {
            SDL_Log("SDL visionOS: no registered game scene; using the first application window scene");
        }
        if (!window_scene) for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene.session.role isEqualToString:UIWindowSceneSessionRoleApplication]) continue;"""))
    # Game controllers: on visionOS the system turns controller input into pinches aimed at the gazed-at view unless
    # that view carries a GCEventInteraction (GameController/GCEventInteraction.h). Every SDL_uikitview gets one,
    # including the Metal view that hosts the CAMetalLayer and becomes the root view, so the GameController
    # framework (SDL's MFi joystick driver) receives the gamepad.
    view=uikit/"SDL_uikitview.m"
    before=view.read_text()
    text=replace_once(before,'#include "SDL_uikitwindow.h"\n','#include "SDL_uikitwindow.h"\n#if TARGET_OS_VISION\n#import <GameController/GameController.h>\n#endif\n')
    text=replace_once(text,"""            [self addInteraction:[[UIPointerInteraction alloc] initWithDelegate:self]];
        }
#endif""","""            [self addInteraction:[[UIPointerInteraction alloc] initWithDelegate:self]];
        }
#endif
#if TARGET_OS_VISION
        {
            GCEventInteraction *controllerEvents = [[GCEventInteraction alloc] init];
            controllerEvents.handledEventTypes = GCUIEventTypeGamepad;
            /* receivesEventsInView defaults to NO (visionOS 26+): gamepad input goes only to GameController. */
            [self addInteraction:controllerEvents];
        }
#endif""")
    # The trackpad pointer: Skyrim draws its own cursor, so the system's dot is hidden over the game, and its place
    # across the window feeds the edge turn (SDL_uikit_xros.m).
    text=replace_once(text,"    if (request != nil && !SDL_GCMouseRelativeMode()) {","""#if TARGET_OS_VISION
    if (request != nil && self.bounds.size.width > 0) {
        SDLXR_PointerMoved((float)((request.location.x - self.bounds.origin.x) / self.bounds.size.width * 2.0 - 1.0));
    }
#endif
    if (request != nil && !SDL_GCMouseRelativeMode()) {""")
    text=replace_once(text,"    if (SDL_ShowCursor(-1)) {","""#if TARGET_OS_VISION
    return [UIPointerStyle hiddenPointerStyle];
#endif
    if (SDL_ShowCursor(-1)) {""")
    view.write_text(replace_once(text,"#endif /* !TARGET_OS_TV && __IPHONE_13_4 */","""#if TARGET_OS_VISION
- (void)pointerInteraction:(UIPointerInteraction *)interaction willExitRegion:(UIPointerRegion *)region animator:(id<UIPointerInteractionAnimating>)animator
{
    SDLXR_PointerLeft();
}
#endif
#endif /* !TARGET_OS_TV && __IPHONE_13_4 */"""))
    changed[str(view.relative_to(output))]=before
    # GCKeyboard/GCMouse: SDL enables them only behind __IPHONE_OS_VERSION_MAX_ALLOWED >= 140000. Make sure the
    # visionOS build takes that path whether or not the xros SDK defines the iOS version macro.
    events=uikit/"SDL_uikitevents.m"
    text=events.read_text()
    events.write_text(replace_once(replace_once(text,"#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 140000) || (__MAC_OS_VERSION_MAX_ALLOWED > 1500000)",
        "#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 140000) || (__MAC_OS_VERSION_MAX_ALLOWED > 1500000) || TARGET_OS_VISION"),
        "        if (SDL_GCMouseRelativeMode()) {\n            SDL_SendMouseMotion(SDL_GetMouseFocus(), mouseID, 1, (int)deltaX, -(int)deltaY);",
        "#if TARGET_OS_VISION\n        SDLXR_MouseDelta();\n#endif\n        if (SDL_GCMouseRelativeMode()) {\n            SDL_SendMouseMotion(SDL_GetMouseFocus(), mouseID, 1, (int)deltaX, -(int)deltaY);"))
    text2=events.read_text()
    text2=replace_once(text2,"    SDL_SendMouseButton(SDL_GetMouseFocus(), mouseID, pressed ? SDL_PRESSED : SDL_RELEASED, button);",
        "#if TARGET_OS_VISION\n    SDLXR_MouseButton(pressed ? 1 : 0);\n#endif\n    SDL_SendMouseButton(SDL_GetMouseFocus(), mouseID, pressed ? SDL_PRESSED : SDL_RELEASED, button);")
    text2=replace_once(text2,"        SDL_SendMouseWheel(SDL_GetMouseFocus(), mouseID, horizontal, vertical, mouse_scroll_direction);",
        "#if TARGET_OS_VISION\n        SDLXR_MouseScroll();\n#endif\n        SDL_SendMouseWheel(SDL_GetMouseFocus(), mouseID, horizontal, vertical, mouse_scroll_direction);")
    events.write_text(text2)
    changed.setdefault(str(events.relative_to(output)),text)
    return changed


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("--platform",choices=("ios","visionos"),default="ios")
    args=parser.parse_args()
    output=args.output.resolve()
    if ROOT not in output.parents or "build" not in output.relative_to(ROOT).parts:
        parser.error("The SDL overlay must stay in this checkout's ignored build tree")
    fetch_sdl.ensure()
    shutil.copytree(SOURCE,output,dirs_exist_ok=True,copy_function=shutil.copy2)
    delegate=output/"src/video/uikit/SDL_uikitappdelegate.m"
    window=output/"src/video/uikit/SDL_uikitwindow.m"
    original_delegate=delegate.read_text()
    original_window=window.read_text()
    text=original_delegate
    scene_code=r'''
/* Skyrim Apple port overlay: scene lifecycle required by the current iOS SDK. */
static BOOL skyrim_scene_main_started;
#include "SDL_uikitevents.h"
@interface SDLUIKitSceneDelegate : NSObject<UIWindowSceneDelegate>
@end
@implementation SDLUIKitSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)scene; (void)session; (void)options;
    [[SDLUIKitDelegate sharedAppDelegate] performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    SDL_OnApplicationDidBecomeActive();
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    SDL_OnApplicationWillResignActive();
}
- (void)sceneWillEnterForeground:(UIScene *)scene {
    (void)scene;
    SDL_OnApplicationWillEnterForeground();
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    SDL_OnApplicationDidEnterBackground();
}
@end
'''
    text=replace_once(text,"static int exit_status;","static int exit_status;\n"+scene_code)
    text=replace_once(text,"- (void)postFinishLaunch\n{",'''- (void)postFinishLaunch
{
    if (skyrim_scene_main_started) return;
    BOOL scene_ready = NO;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class]) scene_ready = YES;
    if (!scene_ready) return;
    skyrim_scene_main_started = YES;''')
    text=replace_once(text,"- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions",'''- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    (void)application; (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"Skyrim SDL Scene" sessionRole:session.role];
    configuration.delegateClass = SDLUIKitSceneDelegate.class;
    return configuration;
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions''')
    text=text.replace("#ifdef SDL_IPHONE_LAUNCHSCREEN","#if defined(SDL_IPHONE_LAUNCHSCREEN) && !defined(SKYRIM_UIKIT_SCENE_LIFECYCLE)")
    delegate.write_text(text)
    text=replace_once(original_window,"UIWindow *uiwindow = [[SDL_uikitwindow alloc] initWithFrame:data.uiscreen.bounds];",'''/* Skyrim Apple port overlay: attach SDL's real window to its UIKit scene. */
        UIWindowScene *window_scene = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if ([scene isKindOfClass:UIWindowScene.class] && ((UIWindowScene *)scene).screen == data.uiscreen) {
                window_scene = (UIWindowScene *)scene;
                break;
            }
        }
        if (!window_scene) return SDL_SetError("No connected UIKit window scene for SDL");
        UIWindow *uiwindow = [[SDL_uikitwindow alloc] initWithWindowScene:window_scene];''')
    window.write_text(text)
    originals={"src/video/uikit/SDL_uikitappdelegate.m":original_delegate,"src/video/uikit/SDL_uikitwindow.m":original_window}
    if args.platform=="visionos":
        for name,before in apply_visionos(output).items():
            originals.setdefault(name,before)
    receipt={"scope":"native-uikit-scene-overlay","vendor_modified":False,"files":{}}
    if args.platform!="ios":
        receipt["platform"]=args.platform
    for name,before in originals.items():
        receipt["files"][name]={"original_sha256":hashlib.sha256(before.encode()).hexdigest(),"overlay_sha256":hashlib.sha256((output/name).read_bytes()).hexdigest()}
    (output.parent/"sdl-scene-overlay.json").write_text(json.dumps(receipt,indent=2)+"\n")


if __name__=="__main__":
    main()
