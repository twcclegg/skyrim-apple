import Combine
import Foundation
import SwiftUI
#if canImport(UIKit)
import UIKit
typealias PlatformImage = UIImage
#else
import AppKit
typealias PlatformImage = NSImage
#endif

extension Image {
    init(platformImage: PlatformImage) {
        #if canImport(UIKit)
        self.init(uiImage: platformImage)
        #else
        self.init(nsImage: platformImage)
        #endif
    }
}

enum LauncherPlatform {
    #if os(macOS)
    static let name = "Mac"
    #elseif os(visionOS)
    static let name = "Apple Vision Pro"
    #else
    static var name: String { UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone" }
    #endif
}

/// One shared launcher model. The Play gate is strict on every platform: a linked runtime that
/// has verified the user's files, plus a DualSense or a physical keyboard and mouse. There is no
/// simulated success, touch path or generic-controller bypass.
@MainActor
final class LauncherModel: ObservableObject {
    enum Session: Equatable {
        case idle
        case launching
        case running
        case paused(reason: String)
    }

    enum PrimaryAction: Equatable {
        case chooseFolder
        case prepare
        case play
        case busy(String)
        case running
        case resume
    }

    enum Runtime: Equatable {
        /// macOS: the local checkout's bridge prepares, verifies and launches the native build.
        case macBridge
        /// macOS wrapper built without access to its checkout.
        case bridgeMissing
        /// iPhone, iPad and Vision Pro: this build links no game runtime yet.
        case notLinked
        /// Physical iPad: the native runtime is linked into this app and plays the original
        /// files in its own Documents/Skyrim.
        case nativeDevice
    }

    @Published private(set) var folderName: String?
    @Published private(set) var folderPath: String?
    @Published private(set) var headline = "Game files not added"
    @Published private(set) var detail = "Choose your original Windows Skyrim installation when you're ready."
    @Published private(set) var isBusy = false
    @Published private(set) var operation = ""
    @Published private(set) var filesVerified = false
    @Published private(set) var canPrepare = false
    @Published private(set) var runtimeReady = false
    @Published private(set) var session: Session = .idle
    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    /// True while the immersive layer is drawing (ImmersiveCompositor reports its start and end).
    @Published private(set) var immersiveSpaceRunning = false
    #endif
    @Published private(set) var artworkImage: PlatformImage?
    @Published private(set) var logURL: URL?
    @Published var errorMessage: String?

    let runtime: Runtime
    let preferences: LauncherPreferences
    let input: InputDeviceMonitor
    let saveSync: SaveSyncService
    let saves: SaveLibrary

    private let defaults = UserDefaults.standard
    // Keys shared with the earlier Mac and Vision launchers so updating keeps their choices.
    private let macFolderKey = "SkyrimJitlessLauncher.gameDirectory"
    private let macArtworkKey = "SkyrimJitlessLauncher.artworkURL"
    private let macLogKey = "SkyrimJitlessLauncher.lastLog"
    private let bookmarkKey = "SkyrimVisionLauncher.folderBookmark"
    private let artworkKey = "SkyrimVisionLauncher.artworkFile"
    private var folderURL: URL?
    /// Why the native runtime can't use this app's Documents/Skyrim, once checked.
    private var nativeProblem: String?
    private var subscriptions: Set<AnyCancellable> = []
    #if os(macOS)
    private let checkout: URL?
    private var gameProcess: MacGameProcess?
    private var suspendTask: Task<Void, Never>?
    #endif

    init() {
        let preferences = LauncherPreferences()
        self.preferences = preferences
        input = InputDeviceMonitor()
        saveSync = SaveSyncService(preferences: preferences)
        saves = SaveLibrary()
        #if os(macOS)
        checkout = MacRuntimeBridge.checkoutDirectory()
        runtime = checkout == nil ? .bridgeMissing : .macBridge
        #elseif SKYRIM_NATIVE_RUNTIME
        runtime = NativeRuntime.isLinked ? .nativeDevice : .notLinked
        #else
        runtime = .notLinked
        #endif
        loadArtwork()
        // Forward nested changes so views observing only the model stay current.
        for publisher in [preferences.objectWillChange, input.objectWillChange,
                          saveSync.objectWillChange, saves.objectWillChange] {
            publisher.sink { [weak self] _ in self?.objectWillChange.send() }.store(in: &subscriptions)
        }
        input.$disconnectGeneration.dropFirst().removeDuplicates()
            .sink { [weak self] _ in
                // @Published delivers during the setter, on the main actor.
                MainActor.assumeIsolated { self?.supportedInputDisconnected() }
            }
            .store(in: &subscriptions)
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        NotificationCenter.default.publisher(for: .skyrimImmersiveRunning)
            .sink { [weak self] note in
                // ImmersiveCompositor posts it on the main queue.
                let running = (note.object as? Bool) ?? false
                MainActor.assumeIsolated { self?.immersiveSpaceChanged(running: running) }
            }
            .store(in: &subscriptions)
        #endif
        #if SKYRIM_NATIVE_RUNTIME
        if runtime == .nativeDevice {
            objectWillChange.sink { [weak self] _ in
                MainActor.assumeIsolated { self?.scheduleNativeStatus() }
            }
            .store(in: &subscriptions)
        }
        #endif
        restoreFolder()
        #if SKYRIM_NATIVE_RUNTIME
        scheduleNativeStatus()
        #endif
    }

    #if SKYRIM_NATIVE_RUNTIME
    private var nativeStatusScheduled = false

    /// Coalesces state changes into one write of Documents/SkyrimNative/status.json, after the change.
    private func scheduleNativeStatus() {
        guard runtime == .nativeDevice, !nativeStatusScheduled else { return }
        nativeStatusScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.nativeStatusScheduled = false
            self.writeNativeStatus()
        }
    }

    private func writeNativeStatus() {
        let info = Bundle.main.infoDictionary ?? [:]
        let sessionName: String
        switch session {
        case .idle: sessionName = "idle"
        case .launching: sessionName = "launching"
        case .running: sessionName = "running"
        case .paused(let reason): sessionName = "paused: \(reason)"
        }
        var status: [String: Any] = [
            "schema": 1,
            "updated_utc": ISO8601DateFormatter().string(from: Date()),
            "app_version": "\(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?"))",
            "runtime": "nativeDevice",
            "runtime_summary": runtimeSummary,
            "runtime_ready": runtimeReady,
            "files_verified": filesVerified,
            "checking": isBusy,
            "headline": headline,
            "detail": detail,
            "session": sessionName,
            "primary_action": "\(primaryAction)",
            "can_play": canPlay,
            "game_directory": NativeRuntime.gameDirectory.path,
            "input": [
                "can_launch": input.canLaunch,
                "status": input.status,
                "detail": input.detail,
                "active_input": input.activeInputName,
                "dualsense": input.connectedDualSense,
                "keyboard": input.hasKeyboard,
                "mouse": input.hasMouse,
            ] as [String: Any],
        ]
        if let reason = playBlockedReason { status["play_blocked_reason"] = reason }
        if let problem = nativeProblem { status["native_problem"] = problem }
        if let identity = NativeRuntime.identity { status["native_identity"] = identity }
        NativeRuntime.writeStatus(status)
    }
    #endif

    // MARK: Play gate

    var primaryAction: PrimaryAction {
        switch session {
        case .launching: return .busy("Launching…")
        case .running: return .running
        case .paused: return .resume
        case .idle: break
        }
        if isBusy { return .busy(operation.isEmpty ? "Please wait…" : operation + "…") }
        switch runtime {
        case .macBridge:
            // The bridge can report an already verified private build before a folder is chosen.
            if runtimeReady { return .play }
            return canPrepare ? .prepare : .chooseFolder
        case .bridgeMissing:
            return .chooseFolder
        case .notLinked:
            return filesVerified && folderURL != nil ? .play : .chooseFolder
        case .nativeDevice:
            // The folder is this app's own Documents/Skyrim; playBlockedReason explains any problem.
            return .play
        }
    }

    /// Why Play is unavailable right now, or nil when the game can start.
    var playBlockedReason: String? {
        switch runtime {
        case .notLinked:
            return "The Skyrim runtime isn't installed on this \(LauncherPlatform.name) yet. This launcher can't start the game here."
        case .bridgeMissing:
            return "This Mac build can't find its Skyrim workspace. Rebuild it from the checkout."
        case .macBridge:
            if !runtimeReady { return "Prepare and verify Skyrim before playing." }
        case .nativeDevice:
            if !runtimeReady { return nativeProblem ?? "Checking Skyrim's files in this app…" }
        }
        if !input.canLaunch && !Self.simulatorAutoPlay { return input.detail }
        return nil
    }

    var canPlay: Bool { primaryAction == .play && playBlockedReason == nil }

    /// Test hook: `simctl launch … -SkyrimSimulatorAutoPlay YES` presses Play once the files verify,
    /// without the input gate (the Simulator has no controller and no one to tap). On a Vision Pro,
    /// `devicectl device process launch … -SkyrimBenchAutoPlay YES` does the same for tools/bench runs;
    /// only a developer launching the app from a paired Mac can pass it.
    static var simulatorAutoPlay: Bool {
        #if targetEnvironment(simulator) && SKYRIM_NATIVE_RUNTIME
        UserDefaults.standard.bool(forKey: "SkyrimSimulatorAutoPlay")
        #elseif os(visionOS) && SKYRIM_NATIVE_RUNTIME
        UserDefaults.standard.bool(forKey: "SkyrimBenchAutoPlay")
        #else
        false
        #endif
    }

    /// Test hook: with `-SkyrimBenchImmersive YES` as well, autoplay goes through Play Immersive (LibraryView).
    static var benchImmersive: Bool {
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        simulatorAutoPlay && UserDefaults.standard.bool(forKey: "SkyrimBenchImmersive")
        #else
        false
        #endif
    }
    static var benchImmersiveStarted = false

    var runtimeSummary: String {
        switch runtime {
        case .macBridge: return runtimeReady ? "Native build verified on this Mac" : "Native build not ready"
        case .bridgeMissing: return "Workspace not found"
        case .notLinked: return "Not installed on this device"
        case .nativeDevice: return runtimeReady ? "Native runtime ready on this \(LauncherPlatform.name)" : "Native runtime installed"
        }
    }

    var isGameActive: Bool { session != .idle }

    var isPaused: Bool {
        if case .paused = session { return true }
        return false
    }

    // MARK: Game folder

    func selectFolder(_ url: URL) {
        guard !isBusy, !isGameActive else { return }
        #if os(macOS)
        folderURL = url.standardizedFileURL
        defaults.set(folderURL?.path, forKey: macFolderKey)
        #else
        if runtime == .nativeDevice {
            #if SKYRIM_NATIVE_RUNTIME
            // The native runtime plays only the copy inside this app, which needs no bookmark.
            if url.standardizedFileURL.resolvingSymlinksInPath().path
                != NativeRuntime.gameDirectory.standardizedFileURL.resolvingSymlinksInPath().path {
                errorMessage = "On this \(LauncherPlatform.name), Skyrim plays the Skyrim folder inside this app. In Files it's On My \(LauncherPlatform.name) › Skyrim › Skyrim."
                return
            }
            #endif
            restoreFolder()
            return
        }
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
            defaults.set(data, forKey: bookmarkKey)
            folderURL = url
        } catch {
            errorMessage = "This folder couldn't be linked. Choose it again in Files."
            return
        }
        #endif
        updateFolderLabels()
        refresh()
    }

    /// Forgets the selection only; the installation itself is never modified or deleted.
    func unlinkFolder() {
        guard !isBusy, !isGameActive else { return }
        defaults.removeObject(forKey: macFolderKey)
        defaults.removeObject(forKey: bookmarkKey)
        if runtime == .nativeDevice {
            // Nothing is linked for the native runtime: it always uses this app's own folder.
            restoreFolder()
            return
        }
        folderURL = nil
        updateFolderLabels()
        filesVerified = false
        runtimeReady = false
        canPrepare = false
        headline = "Game files not added"
        detail = "Choose your original Windows Skyrim installation when you're ready."
    }

    func refresh() {
        guard !isBusy, !isGameActive else { return }
        #if os(macOS)
        runBridge("status", label: "Checking", message: "Checking your local Skyrim files…")
        #else
        guard let folder = folderURL else { return }
        isBusy = true
        operation = "Checking"
        filesVerified = false
        headline = "Checking game files"
        detail = "Reading your selected installation."
        #if SKYRIM_NATIVE_RUNTIME
        if runtime == .nativeDevice {
            detail = "Checking the original files in this app against the native runtime."
            runtimeReady = false
            nativeProblem = nil
            let note = "Your original files match the native runtime on this \(LauncherPlatform.name)."
            Task { [weak self] in
                let (result, problem) = await Task.detached(priority: .userInitiated) {
                    let result = GameInstallValidator.validate(folder, runtimeNote: note)
                    return (result, result.coreFilesFound ? NativeRuntime.validate() : result.detail)
                }.value
                guard let self else { return }
                self.isBusy = false
                self.operation = ""
                self.filesVerified = result.coreFilesFound && problem == nil
                self.runtimeReady = self.filesVerified
                self.nativeProblem = problem
                self.headline = problem == nil ? "Ready to play" : "Game files need attention"
                self.detail = problem ?? result.detail
                if Self.simulatorAutoPlay, !Self.benchImmersive, self.canPlay { self.play() }
            }
            return
        }
        #endif
        let note = "Your original files are linked. The Skyrim runtime isn't installed on this \(LauncherPlatform.name) yet, so it can't start here."
        Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                GameInstallValidator.validate(folder, runtimeNote: note)
            }.value
            guard let self else { return }
            self.isBusy = false
            self.operation = ""
            self.filesVerified = result.coreFilesFound
            self.headline = result.headline
            self.detail = result.detail
        }
        #endif
    }

    func prepare() {
        #if os(macOS)
        guard canPrepare, !isGameActive else { return }
        runBridge("prepare", label: "Preparing", message: "Preparing your private macOS copy. This can take a while.")
        #endif
    }

    // MARK: Session

    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    /// LB_GAME_SCENE=1 in Documents/SkyrimNative/launch-env.json: the game plays in its own window, which is also the
    /// trackpad's pointer surface, and the launcher window closes (docs/VISIONOS_POINTER_PLAN.md). Read at Play,
    /// before the runtime applies the same file.
    static var gameSceneMode: Bool {
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SkyrimNative/launch-env.json")
        guard let data = try? Data(contentsOf: url),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return (json["LB_GAME_SCENE"] as? String) == "1"
    }
    /// Set when Play opened the game window; the window starts the game once it has registered its scene.
    @Published var playWhenGameWindowReady = false
    #endif

    func play() {
        guard canPlay, session == .idle else { return }
        #if os(macOS)
        guard let checkout else { return }
        // Seal saves before the runtime can open them; it stays sealed through pauses.
        saveSync.setGameRunning(true)
        session = .launching
        headline = "Starting Skyrim"
        detail = "Launching the native build from your local workspace."
        let game = folderURL
        let executable = MacRuntimeBridge.gameExecutable(in: checkout)
        let onStarted: @MainActor @Sendable (Int32) -> Void = { [weak self] pid in
            guard let self, self.session != .idle else { return }
            self.gameProcess = MacGameProcess(pid: pid, executable: executable)
            if case .paused = self.session { self.applySuspend() } else {
                self.session = .running
                self.headline = "Skyrim is running"
                self.detail = "Disconnecting your controls pauses the game until you choose Resume."
            }
        }
        Task { [weak self] in
            let outcome = await Task.detached(priority: .userInitiated) {
                MacRuntimeBridge.run(checkout: checkout, command: "launch", game: game,
                                     saves: SkyrimSaveLocations.runtimeRoot, onStarted: onStarted)
            }.value
            guard let self else { return }
            self.suspendTask?.cancel()
            self.suspendTask = nil
            self.gameProcess = nil
            self.session = .idle
            self.saveSync.setGameRunning(false)
            self.apply(outcome)
            await self.saves.refresh()
        }
        #elseif SKYRIM_NATIVE_RUNTIME
        guard runtime == .nativeDevice else { return }
        // Seal saves before the runtime can open them; it stays sealed through pauses.
        saveSync.setGameRunning(true)
        session = .launching
        headline = "Starting Skyrim"
        detail = "Starting the native game on this \(LauncherPlatform.name)."
        #if os(visionOS)
        Task { [weak self] in
            await NativeRuntime.shapeWindow(forGame: true)
            self?.startNative()
        }
        #else
        startNative()
        #endif
        #endif
    }

    #if SKYRIM_NATIVE_RUNTIME
    private func startNative() {
        NativeRuntime.start(saveRoot: SkyrimSaveLocations.runtimeRoot, started: { [weak self] in
            // A disconnect while launching already paused it; the game then holds its first frame.
            guard let self, case .launching = self.session else { return }
            self.session = .running
            self.headline = "Skyrim is running"
            self.detail = "Disconnecting your controls pauses the game until you choose Resume."
        }, finished: { [weak self] problem in
            guard let self else { return }
            NativeRuntime.setPaused(false)
            self.session = .idle
            self.saveSync.setGameRunning(false)
            self.headline = problem == nil ? "Skyrim closed" : "Skyrim stopped"
            self.detail = problem ?? "Your saves are in Saved Games."
            if let problem { self.errorMessage = problem }
            Task {
                #if os(visionOS)
                await NativeRuntime.shapeWindow(forGame: false)
                #endif
                await self.saves.refresh()
            }
        })
    }
    #endif

    func pause(reason: String = "Paused") {
        switch session {
        case .running, .launching: break
        default: return
        }
        session = .paused(reason: reason)
        #if os(macOS)
        applySuspend()
        #elseif SKYRIM_NATIVE_RUNTIME
        if runtime == .nativeDevice {
            NativeRuntime.setPaused(true)
            detail = "Skyrim is paused. Choose Resume when your controls are ready."
        }
        #endif
    }

    /// Resuming is always explicit and requires the input policy to allow it right now.
    func resume() {
        guard case .paused = session, input.resumeAssessment().canResume else { return }
        #if os(macOS)
        suspendTask?.cancel()
        suspendTask = nil
        guard let process = gameProcess else {
            // Paused before the game finished starting; nothing was suspended yet.
            session = .launching
            return
        }
        if process.resume() {
            session = .running
            headline = "Skyrim is running"
            detail = "Resumed with \(input.activeInputName)."
        } else {
            errorMessage = "The game process could not be resumed. It may have closed."
        }
        #elseif SKYRIM_NATIVE_RUNTIME
        guard runtime == .nativeDevice else { return }
        NativeRuntime.setPaused(false)
        session = .running
        headline = "Skyrim is running"
        detail = "Resumed with \(input.activeInputName)."
        #endif
    }

    func quitGame() {
        #if os(macOS)
        guard isGameActive else { return }
        if let process = gameProcess, !process.terminate() {
            errorMessage = "The game process could not be found. It may already have closed."
        }
        #elseif SKYRIM_NATIVE_RUNTIME
        guard isGameActive, runtime == .nativeDevice else { return }
        // The game runs in this app's process, so ending it closes the app too.
        NativeRuntime.requestQuit()
        detail = "Closing Skyrim. The app closes with it."
        #endif
    }

    private func supportedInputDisconnected() {
        switch session {
        case .running, .launching:
            pause(reason: "Controls disconnected")
        default:
            break
        }
    }

    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    /// The immersive layer started or ended. When it ends under a running game (Digital Crown, the system closing
    /// the space) the game pauses instead of carrying on unwatched in the flat window; the Paused screen then
    /// offers the way back in. ImmersivePolicy says when that pause would strand the game instead.
    private func immersiveSpaceChanged(running: Bool) {
        let ended = immersiveSpaceRunning && !running
        immersiveSpaceRunning = running
        guard ended else { return }
        let gameRunning = session == .running || session == .launching
        if ImmersivePolicy.pausesOnLeave(gameRunning: gameRunning, unattended: Self.simulatorAutoPlay,
                                         launcherPresent: !Self.gameSceneMode) {
            pause(reason: ImmersivePolicy.leftReason)
        }
    }
    #endif

    #if os(macOS)
    /// Suspends the game once `run.sh` has exec'd into it, retrying while it is still starting.
    private func applySuspend() {
        suspendTask?.cancel()
        suspendTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, case .paused = self.session else { return }
                if let process = self.gameProcess, process.suspend() {
                    self.detail = "Game process suspended. Choose Resume when your controls are ready."
                    return
                }
                try? await Task.sleep(for: .milliseconds(250))
            }
        }
    }

    private func runBridge(_ command: String, label: String, message: String) {
        guard !isBusy else { return }
        guard let checkout else {
            headline = "Launcher setup is incomplete"
            detail = "This Mac build can't find its local Skyrim workspace. Rebuild it from the checkout."
            return
        }
        isBusy = true
        runtimeReady = false
        canPrepare = false
        operation = label
        detail = message
        let game = folderURL
        Task { [weak self] in
            let outcome = await Task.detached(priority: .userInitiated) {
                MacRuntimeBridge.run(checkout: checkout, command: command, game: game,
                                     saves: SkyrimSaveLocations.runtimeRoot, onStarted: { _ in })
            }.value
            guard let self else { return }
            self.isBusy = false
            self.operation = ""
            self.apply(outcome)
        }
    }

    private func apply(_ outcome: MacRuntimeBridge.Outcome) {
        if let response = outcome.response {
            headline = response.headline
            detail = response.detail
            runtimeReady = response.isReady
            canPrepare = response.canPrepare
            filesVerified = response.isReady || response.canPrepare
            if let path = response.logPath {
                logURL = URL(fileURLWithPath: path)
                defaults.set(path, forKey: macLogKey)
            }
            if !response.success || outcome.exitCode != 0 {
                errorMessage = response.error ?? "The operation could not finish. Open the log for details."
            }
        } else {
            headline = "Skyrim needs attention"
            detail = "The operation could not finish. Your original game files were preserved."
            runtimeReady = false
            canPrepare = false
            errorMessage = "The launcher helper could not complete its check. Open the log for details."
            if let path = outcome.fallbackLogPath { logURL = URL(fileURLWithPath: path) }
        }
    }
    #endif

    // MARK: Artwork

    func importArtwork(_ url: URL) {
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        do {
            let values = try url.resourceValues(forKeys: [.fileSizeKey])
            guard (values.fileSize ?? 0) <= 25 * 1024 * 1024 else { throw CocoaError(.fileReadTooLarge) }
            let data = try Data(contentsOf: url)
            guard let image = PlatformImage(data: data) else { throw CocoaError(.fileReadCorruptFile) }
            try FileManager.default.createDirectory(at: Self.artworkDirectory, withIntermediateDirectories: true)
            let ext = url.pathExtension.isEmpty ? "png" : url.pathExtension.lowercased()
            let name = "cover-\(UUID().uuidString).\(ext)"
            try data.write(to: Self.artworkDirectory.appendingPathComponent(name), options: .atomic)
            defaults.set(name, forKey: artworkKey)
            artworkImage = image
        } catch {
            errorMessage = "Choose a readable image smaller than 25 MB. Your source image hasn't been changed."
        }
    }

    func useDefaultArtwork() {
        defaults.removeObject(forKey: artworkKey)
        defaults.removeObject(forKey: macArtworkKey)
        artworkImage = nil
    }

    private static var artworkDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("LauncherArtwork", isDirectory: true)
    }

    private func loadArtwork() {
        if let name = defaults.string(forKey: artworkKey),
           let image = PlatformImage(contentsOfFile: Self.artworkDirectory.appendingPathComponent(name).path) {
            artworkImage = image
        } else if let path = defaults.string(forKey: macArtworkKey) {
            artworkImage = PlatformImage(contentsOfFile: path)
        }
    }

    // MARK: Restoration

    private func restoreFolder() {
        #if os(macOS)
        if let path = defaults.string(forKey: macFolderKey) {
            folderURL = URL(fileURLWithPath: path, isDirectory: true)
        }
        if let path = defaults.string(forKey: macLogKey) { logURL = URL(fileURLWithPath: path) }
        updateFolderLabels()
        if runtime == .bridgeMissing {
            headline = "Launcher setup is incomplete"
            detail = "This Mac build can't find its local Skyrim workspace. Rebuild it from the checkout."
        } else {
            refresh()
        }
        #else
        #if SKYRIM_NATIVE_RUNTIME
        if runtime == .nativeDevice {
            folderURL = NativeRuntime.gameDirectory
            updateFolderLabels()
            refresh()
            return
        }
        #endif
        guard let data = defaults.data(forKey: bookmarkKey) else { return }
        do {
            var stale = false
            let url = try URL(resolvingBookmarkData: data, options: .withoutUI, relativeTo: nil, bookmarkDataIsStale: &stale)
            folderURL = url
            updateFolderLabels()
            if stale, let fresh = try? url.bookmarkData(options: .minimalBookmark,
                                                        includingResourceValuesForKeys: nil, relativeTo: nil) {
                defaults.set(fresh, forKey: bookmarkKey)
            }
            refresh()
        } catch {
            headline = "Choose your game folder again"
            detail = "The previous folder is no longer available. Your original files haven't been changed."
        }
        #endif
    }

    private func updateFolderLabels() {
        folderName = folderURL?.lastPathComponent
        folderPath = folderURL?.path
    }
}
