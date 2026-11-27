import SwiftUI
import UniformTypeIdentifiers

/// The single-title library screen shared by Mac, iPhone, iPad and Vision Pro. Compact iPhone
/// layouts size the card to fit the screen; larger windows center the Dark Eco style card.
struct LibraryView: View {
    static let windowID = "library"
    @ObservedObject var model: LauncherModel
    @State private var showSettings = false
    @State private var picker: PickerRequest?
    @State private var confirmQuit = false
    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    #endif
    #endif

    enum PickerRequest { case folder, artwork }

    private var accent: Color { SmokeTheme.accent(model.preferences.accent) }

    var body: some View {
        GeometryReader { proxy in
            let layout = Layout(size: proxy.size)
            ZStack {
                SmokeBackground(reduceSmoke: model.preferences.reduceSmoke).ignoresSafeArea()
                VStack(spacing: 0) {
                    header(compact: layout.isCompact)
                    Spacer(minLength: layout.isCompact ? 6 : 16)
                    if layout.isLandscapePhone {
                        HStack(alignment: .center, spacing: 18) {
                            card(coverWidth: layout.coverWidth, horizontal: true, infoWidth: layout.infoWidth)
                        }
                    } else {
                        VStack(spacing: layout.isCompact ? 10 : 16) {
                            card(coverWidth: layout.coverWidth, horizontal: false, infoWidth: 0)
                            Text(model.detail)
                                .font(.system(size: layout.isCompact ? 12 : 13))
                                .foregroundStyle(SmokeTheme.text.opacity(0.66))
                                .multilineTextAlignment(.center)
                                .lineLimit(3)
                                .frame(maxWidth: 420)
                                .padding(.horizontal, 24)
                        }
                    }
                    Spacer(minLength: layout.isCompact ? 6 : 20)
                    footer(compact: layout.isCompact)
                }
                if case .paused(let reason) = model.session {
                    PauseOverlay(model: model, reason: reason, accent: accent) { confirmQuit = true }
                        .transition(.opacity)
                }
            }
        }
        .foregroundStyle(SmokeTheme.text)
        .tint(accent)
        .animation(.easeOut(duration: 0.2), value: model.session)
        .sheet(isPresented: $showSettings) {
            SettingsView(model: model)
        }
        .fileImporter(isPresented: Binding(get: { picker != nil }, set: { if !$0 { picker = nil } }),
                      allowedContentTypes: picker == .artwork ? [.image] : [.folder]) { result in
            let request = picker
            picker = nil
            switch result {
            case .success(let url): request == .artwork ? model.importArtwork(url) : model.selectFolder(url)
            case .failure(let error): model.errorMessage = error.localizedDescription
            }
        }
        .alert("Couldn't Complete That Step", isPresented: Binding(
            get: { model.errorMessage != nil && !showSettings },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("OK", role: .cancel) { model.errorMessage = nil }
        } message: {
            Text(model.errorMessage ?? "")
        }
        .confirmationDialog("Quit Skyrim?", isPresented: $confirmQuit, titleVisibility: .visible) {
            Button("Quit Game", role: .destructive) { model.quitGame() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Progress since your last in-game save will be lost.")
        }
        .task {
            // Screenshot verification: `-SkyrimLauncherOpenSettings YES` opens Settings at
            // launch. It is read from the launch arguments and never stored.
            if UserDefaults.standard.bool(forKey: "SkyrimLauncherOpenSettings") { showSettings = true }
            await model.saves.refresh()
        }
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        .task { await benchReenter() }
        #endif
    }

    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
    /// Test hook: `-SkyrimBenchReenter <seconds>` on a bench run that plays immersive (`-SkyrimBenchAutoPlay YES
    /// -SkyrimBenchImmersive YES`). That long after the game and the immersive layer are both running, it closes
    /// the space, waits for the layer to end, and opens the space again, so the second attach of the compositor
    /// to a running game can be checked from the log without anyone wearing the headset. An unattended run is
    /// never paused for leaving the space (ImmersivePolicy), so the game keeps producing frames throughout.
    private func benchReenter() async {
        let after = UserDefaults.standard.double(forKey: "SkyrimBenchReenter")
        guard LauncherModel.benchImmersive, after > 0 else { return }
        while !(model.session == .running && model.immersiveSpaceRunning) {
            do { try await Task.sleep(for: .milliseconds(250)) } catch { return }
        }
        do { try await Task.sleep(for: .seconds(after)) } catch { return }
        fputs("bench: re-entry: closing the immersive space\n", stderr)
        await dismissImmersiveSpace()
        while model.immersiveSpaceRunning {
            do { try await Task.sleep(for: .milliseconds(250)) } catch { return }
        }
        fputs("bench: re-entry: the layer ended, game session \(model.session)\n", stderr)
        do { try await Task.sleep(for: .seconds(3)) } catch { return }
        let result = await openImmersiveSpace(id: ImmersiveScreen.spaceID)
        fputs("bench: re-entry: immersive space reopened: \(result)\n", stderr)
        while !model.immersiveSpaceRunning {
            do { try await Task.sleep(for: .milliseconds(250)) } catch { return }
        }
        fputs("bench: re-entry: the layer is running again\n", stderr)
    }
    #endif

    // MARK: Layout

    struct Layout {
        let size: CGSize
        var isLandscapePhone: Bool { size.height < 500 && size.width > size.height }
        var isCompact: Bool { size.width < 600 || size.height < 640 }
        var coverWidth: CGFloat {
            if isLandscapePhone { return min(220, max(110, (size.height - 120) * 2 / 3)) }
            // Header, card details, detail text and footer need roughly this much height.
            let chrome: CGFloat = isCompact ? 360 : 410
            let byHeight = (size.height - chrome) * 2 / 3
            let byWidth = size.width - (isCompact ? 96 : 160)
            let largest: CGFloat = isCompact ? 250 : (size.height > 1000 ? 340 : 280)
            return min(largest, max(140, min(byHeight, byWidth)))
        }
        var infoWidth: CGFloat { min(340, max(220, size.width - coverWidth - 140)) }
    }

    // MARK: Header and footer

    private func header(compact: Bool) -> some View {
        ZStack {
            HStack {
                Text("Library")
                    .font(.system(size: compact ? 15 : 17, weight: .semibold))
                    .foregroundStyle(SmokeTheme.text.opacity(0.74))
                Spacer()
                Button { showSettings = true } label: {
                    Image(systemName: "gearshape")
                        .font(.system(size: 17, weight: .medium))
                        .frame(width: 44, height: 44)
                        .background(.white.opacity(0.035), in: Circle())
                        .contentShape(Circle())
                }
                .buttonStyle(.plain)
                .launcherHover()
                .help("Settings")
                .accessibilityLabel("Settings")
            }
            Menu {
                Button("Settings…", systemImage: "gearshape") { showSettings = true }
                Button("Choose Game Folder…", systemImage: "folder") { picker = .folder }
                    .disabled(model.isBusy || model.isGameActive)
                Button("Check Game Files", systemImage: "arrow.clockwise", action: model.refresh)
                    .disabled(model.isBusy || model.isGameActive)
            } label: {
                CapsuleLabel(accent: accent, large: !compact)
            }
            .buttonStyle(.plain)
            .menuIndicator(.hidden)
            .fixedSize()
            .accessibilityLabel("Skyrim main menu")
        }
        .padding(.horizontal, compact ? 18 : 30)
        .padding(.top, compact ? 8 : 26)
        .padding(.bottom, 6)
    }

    private func footer(compact: Bool) -> some View {
        HStack(spacing: 12) {
            Label("Original Windows edition", systemImage: "opticaldisc")
                .lineLimit(1)
            Spacer(minLength: 8)
            Text(model.folderName ?? LauncherPlatform.name)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .font(.system(size: compact ? 10 : 11))
        .foregroundStyle(SmokeTheme.text.opacity(0.58))
        .padding(.horizontal, compact ? 18 : 30)
        .padding(.bottom, compact ? 8 : 20)
    }

    // MARK: Card

    @ViewBuilder
    private func card(coverWidth: CGFloat, horizontal: Bool, infoWidth: CGFloat) -> some View {
        let shape = RoundedRectangle(cornerRadius: 16)
        Group {
            if horizontal {
                HStack(alignment: .center, spacing: 14) {
                    cover(width: coverWidth)
                    info(compact: true)
                        .frame(width: infoWidth)
                        .padding(.trailing, 6)
                }
            } else {
                VStack(alignment: .leading, spacing: 0) {
                    cover(width: coverWidth)
                    info(compact: coverWidth < 220)
                        .padding(.horizontal, 6)
                        .padding(.top, 12)
                        .padding(.bottom, 8)
                }
                .frame(width: coverWidth + 12)
            }
        }
        .padding(6)
        .background(SmokeTheme.surface.opacity(0.94), in: shape)
        .overlay { shape.strokeBorder(accent.opacity(0.32), lineWidth: 1) }
        .shadow(color: .black.opacity(0.32), radius: 26, y: 14)
    }

    private func cover(width: CGFloat) -> some View {
        ZStack(alignment: .bottomTrailing) {
            Group {
                if let image = model.artworkImage {
                    Image(platformImage: image).resizable().scaledToFill()
                } else {
                    DefaultCover(accent: accent)
                }
            }
            .frame(width: width, height: width * 1.5)
            .clipped()
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .overlay { RoundedRectangle(cornerRadius: 12).strokeBorder(accent.opacity(0.26), lineWidth: 1) }
            .accessibilityLabel("Skyrim cover, \(model.headline)")
            Menu {
                Button("Change Artwork…", systemImage: "photo") { picker = .artwork }
                if model.artworkImage != nil {
                    Button("Use Default Artwork", systemImage: "arrow.uturn.backward", action: model.useDefaultArtwork)
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 14, weight: .semibold))
                    .frame(width: 34, height: 34)
                    .background(.black.opacity(0.5), in: Circle())
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .menuIndicator(.hidden)
            .fixedSize()
            .padding(8)
            .accessibilityLabel("Artwork options")
        }
    }

    private func info(compact: Bool) -> some View {
        VStack(alignment: .leading, spacing: compact ? 8 : 10) {
            VStack(alignment: .leading, spacing: 4) {
                Text("The Elder Scrolls V: Skyrim")
                    .font(.system(size: compact ? 13 : 15, weight: .semibold))
                    .lineLimit(2)
                    .minimumScaleFactor(0.85)
                HStack(spacing: 5) {
                    if model.isBusy {
                        ProgressView().controlSize(.mini)
                    } else {
                        Image(systemName: statusSymbol).font(.system(size: 10))
                    }
                    Text(model.headline).font(.system(size: 11, weight: .medium)).lineLimit(2)
                }
                .foregroundStyle(accent.opacity(0.88))
            }
            primaryButton(compact: compact)
            #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
            if model.primaryAction == .play && model.canPlay {
                Button {
                    Task {
                        if case .opened = await openImmersiveSpace(id: ImmersiveScreen.spaceID) { startPlay() }
                    }
                } label: {
                    Label("Play Immersive", systemImage: "visionpro")
                        .font(.system(size: 12, weight: .semibold))
                        .frame(maxWidth: .infinity, minHeight: 30)
                }
                .buttonStyle(.bordered)
                .task {
                    guard LauncherModel.benchImmersive, !LauncherModel.benchImmersiveStarted else { return }
                    LauncherModel.benchImmersiveStarted = true
                    // A bench run asked for the immersive space: say what happened, and retry a few times (it can
                    // fail while another space or the previous run's is still closing). Never fall back to the flat
                    // window, which would give a run that looks fine but measures something else.
                    for attempt in 1...4 {
                        let result = await openImmersiveSpace(id: ImmersiveScreen.spaceID)
                        if case .opened = result {
                            fputs("bench: immersive space opened (attempt \(attempt))\n", stderr)
                            startPlay()
                            return
                        }
                        fputs("bench: immersive space did not open (attempt \(attempt)): \(result)\n", stderr)
                        try? await Task.sleep(for: .seconds(2))
                    }
                    fputs("bench: giving up on the immersive space; the game is not started\n", stderr)
                }
            }
            #endif
            if model.primaryAction == .running {
                #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
                // A game running in the flat window: started there, or resumed there after leaving the space.
                if ImmersivePolicy.offersReturn(gameActive: model.isGameActive, spaceRunning: model.immersiveSpaceRunning) {
                    ReturnToImmersiveButton(model: model)
                }
                #endif
                Button { model.pause(reason: "Paused from the launcher") } label: {
                    Label("Pause", systemImage: "pause.fill")
                        .font(.system(size: 12, weight: .semibold))
                        .frame(maxWidth: .infinity, minHeight: 30)
                }
                .buttonStyle(.bordered)
            }
            InputGateRow(model: model, accent: accent, compact: compact)
        }
    }

    private var statusSymbol: String {
        if model.runtimeReady || model.filesVerified { return "checkmark.circle" }
        return model.folderName == nil ? "tray" : "exclamationmark.circle"
    }

    /// Starts the game: directly, or (LB_GAME_SCENE=1 on visionOS) by opening the game's own window, which starts
    /// it once its scene is registered, and closing this one.
    private func startPlay() {
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        if LauncherModel.gameSceneMode {
            model.playWhenGameWindowReady = true
            openWindow(id: GameSurfaceView.windowID)
            dismissWindow(id: LibraryView.windowID)
            return
        }
        #endif
        model.play()
    }

    private func primaryButton(compact: Bool) -> some View {
        let action = model.primaryAction
        let enabled: Bool = switch action {
        case .play: model.canPlay
        case .busy, .running, .resume: false
        case .chooseFolder, .prepare: !model.isBusy
        }
        return Button {
            switch action {
            case .chooseFolder: picker = .folder
            case .prepare: model.prepare()
            case .play: startPlay()
            case .busy, .running, .resume: break
            }
        } label: {
            HStack(spacing: 7) {
                Image(systemName: symbol(for: action)).font(.system(size: 11, weight: .semibold))
                Text(title(for: action)).font(.system(size: compact ? 12 : 13, weight: .semibold)).lineLimit(1)
                Spacer(minLength: 0)
                if action == .play && !model.canPlay {
                    Image(systemName: "lock.fill").font(.system(size: 10))
                }
            }
            .padding(.horizontal, 12)
            .frame(height: compact ? 36 : 40)
            .foregroundStyle(Color.black.opacity(enabled ? 0.85 : 0.55))
            .background(accent.opacity(enabled ? 1 : 0.42), in: RoundedRectangle(cornerRadius: 9))
            .contentShape(RoundedRectangle(cornerRadius: 9))
        }
        .buttonStyle(.plain)
        .launcherHover()
        .disabled(!enabled)
        .accessibilityHint(action == .play ? (model.playBlockedReason ?? "Starts Skyrim") : "")
    }

    private func title(for action: LauncherModel.PrimaryAction) -> String {
        switch action {
        case .chooseFolder: return model.folderName == nil ? "Choose Game Folder" : "Choose Another Folder"
        case .prepare: return "Prepare Skyrim"
        case .play: return "Play Skyrim"
        case .busy(let label): return label
        case .running: return "Running"
        case .resume: return "Paused"
        }
    }

    private func symbol(for action: LauncherModel.PrimaryAction) -> String {
        switch action {
        case .chooseFolder: return "plus"
        case .prepare: return "hammer"
        case .play: return "play.fill"
        case .busy: return "hourglass"
        case .running: return "gamecontroller.fill"
        case .resume: return "pause.fill"
        }
    }
}

/// Shows which input satisfies the Play gate, or why it is locked.
struct InputGateRow: View {
    @ObservedObject var model: LauncherModel
    let accent: Color
    var compact = false

    var body: some View {
        let input = model.input
        let blocked = model.primaryAction == .play ? model.playBlockedReason : nil
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Image(systemName: input.connectedDualSense ? "gamecontroller.fill"
                      : input.canLaunch ? "keyboard" : "exclamationmark.triangle")
                    .font(.system(size: 11))
                Text(input.canLaunch ? input.activeInputName : input.status)
                    .font(.system(size: 11, weight: .semibold))
                Spacer(minLength: 0)
            }
            .foregroundStyle(input.canLaunch ? accent : SmokeTheme.text.opacity(0.72))
            if let blocked {
                Text(blocked)
                    .font(.system(size: 10))
                    .foregroundStyle(SmokeTheme.text.opacity(0.6))
                    .lineLimit(compact ? 3 : 4)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(8)
        .background(.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 8))
        .accessibilityElement(children: .combine)
    }
}

/// Covers the library while a session is paused. Reconnecting never resumes by itself.
struct PauseOverlay: View {
    @ObservedObject var model: LauncherModel
    let reason: String
    let accent: Color
    let requestQuit: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.62).ignoresSafeArea()
            TimelineView(.periodic(from: .now, by: 0.5)) { context in
                let assessment = model.input.resumeAssessment(now: context.date)
                VStack(spacing: 14) {
                    Image(systemName: "pause.circle").font(.system(size: 38, weight: .light)).foregroundStyle(accent)
                    Text("Skyrim Is Paused").font(.system(size: 20, weight: .semibold))
                    Text(reason).font(.system(size: 13, weight: .medium)).foregroundStyle(accent)
                    Text(assessment.reason)
                        .font(.system(size: 12))
                        .foregroundStyle(SmokeTheme.text.opacity(0.7))
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                    if assessment.isUnstable {
                        Label("Unstable connection: \(ResumePolicy.recentDisconnects(model.input.disconnectDates, now: context.date)) drops in 30 seconds",
                              systemImage: "exclamationmark.triangle")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundStyle(.orange)
                    }
                    Button(action: model.resume) {
                        Label("Resume", systemImage: "play.fill")
                            .font(.system(size: 14, weight: .semibold))
                            .frame(maxWidth: .infinity, minHeight: 40)
                            .foregroundStyle(Color.black.opacity(assessment.canResume ? 0.85 : 0.5))
                            .background(accent.opacity(assessment.canResume ? 1 : 0.4), in: RoundedRectangle(cornerRadius: 9))
                    }
                    .buttonStyle(.plain)
                    .disabled(!assessment.canResume)
                    #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
                    // Paused outside the space (the usual case: the Digital Crown closed it). Resume above carries
                    // on in the flat window; this goes back into the headset view.
                    if ImmersivePolicy.offersReturn(gameActive: model.isGameActive, spaceRunning: model.immersiveSpaceRunning) {
                        ReturnToImmersiveButton(model: model)
                    }
                    #endif
                    #if os(macOS)
                    Button("Quit Game", role: .destructive, action: requestQuit)
                        .buttonStyle(.plain)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(SmokeTheme.text.opacity(0.7))
                    #endif
                }
                .padding(24)
                .frame(maxWidth: 360)
                .background(SmokeTheme.surface, in: RoundedRectangle(cornerRadius: 18))
                .overlay { RoundedRectangle(cornerRadius: 18).strokeBorder(accent.opacity(0.35), lineWidth: 1) }
                .padding(24)
            }
        }
    }
}

#if os(visionOS) && SKYRIM_NATIVE_RUNTIME
/// Opens the immersive space again for a game that already exists; a new compositor then attaches to it. A paused
/// game resumes once the space is open, if the input policy allows it; otherwise it stays paused inside the space
/// with the Paused screen visible, and Resume unlocks there.
struct ReturnToImmersiveButton: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @State private var opening = false

    var body: some View {
        Button {
            guard !opening else { return }
            opening = true
            Task {
                let result = await openImmersiveSpace(id: ImmersiveScreen.spaceID)
                opening = false
                switch result {
                case .opened:
                    fputs("immersive: space reopened for the running game\n", stderr)
                    model.resume()
                case .userCancelled:
                    break
                default:
                    fputs("immersive: space did not reopen: \(result)\n", stderr)
                    model.errorMessage = "The immersive view could not be opened. The game is still here; try again."
                }
            }
        } label: {
            Label("Return to Immersive", systemImage: "visionpro")
                .font(.system(size: 12, weight: .semibold))
                .frame(maxWidth: .infinity, minHeight: 30)
        }
        .buttonStyle(.bordered)
        .disabled(opening)
    }
}
#endif
