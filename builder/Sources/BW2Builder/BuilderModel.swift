import AppKit
import BuilderCore

/// Owns the rows, runs the pipeline commands and drives the build, the install and the clean.
final class BuilderModel: ObservableObject {
    enum Phase: Equatable {
        case checklist
        /// `title` is the current step's (or "Installing the app", "Removing the build files").
        case building(title: String, fraction: Double)
        case finished(URL)
        case failed(step: String, message: String)
    }

    @Published private(set) var states: [CheckID: CheckState]
    @Published private(set) var phase: Phase = .checklist
    @Published private(set) var choices: BuilderChoices
    /// The log of the current or last build (all of it; the views show the tail).
    @Published private(set) var log: [String] = []
    /// True once Cancel was pressed or the install started, so Cancel can no longer act.
    @Published private(set) var cancelDisabled = false
    /// Set when the app was installed but removing the build files failed.
    @Published private(set) var cleanNote: String?
    @Published private(set) var buildFilesBytes: Int64?
    @Published private(set) var buildToolsBytes: Int64?
    @Published private(set) var buildFilesRemoved = false
    @Published private(set) var removingBuildFiles = false
    @Published private(set) var freedBytes: Int64?
    @Published var removeToolsToo = false
    /// A plain message shown as an alert over the checklist.
    @Published var notice: String?

    let paths: BuilderPaths
    let version: String
    let sources: URL?
    private let tables: InputTables?

    private var live: [ObjectIdentifier: PipelineRunner] = [:]
    private var idleHandlers: [() -> Void] = []
    private(set) var shuttingDown = false
    private var workspaceReady = false
    private var installingTools = false
    private var pythonTask: URLSessionDownloadTask?
    private var pythonProgress: NSKeyValueObservation?
    private var cltTimer: Timer?

    private var discs: [Int: URL] = [:]
    private var discToken = UUID()
    /// What the disc and patch rows were last checked against (paths, sizes, dates): hashing gigabytes of bins
    /// again each time the window comes to the front is not needed when nothing changed.
    private var discKey: [String]?
    private var patchKeys: [CheckID: [String]] = [:]
    private var patchTokens: [CheckID: UUID] = [:]
    private var downloads: [CheckID: PipelineRunner] = [:]
    /// The last failed download of a patch row, shown until that row's file is chosen or downloaded again.
    private var downloadErrors: [CheckID: String] = [:]

    private var buildRunner: PipelineRunner?
    private var stepID = ""
    private var stepTitle = ""
    private var stepPart = 0.0
    private var buildError: String?
    private var built: URL?
    private var userCancelled = false
    private var flushScheduled = false
    /// True while installApp runs: a quit waits for it, so the old app is never left moved aside.
    private var installing = false
    private var measurementToken = UUID()

    init(paths: BuilderPaths, version: String, sources: URL?) {
        self.paths = paths
        self.version = version
        self.sources = sources
        tables = sources.flatMap { try? InputTables.load(sources: $0) }
        choices = BuilderChoices.load(paths)
        states = Dictionary(uniqueKeysWithValues: CheckID.allCases.map { ($0, CheckState.checking("Checking...")) })
    }

    // MARK: - Rows

    var toolsReady: Bool { states[.buildTools]?.isReady == true }
    var buildEnabled: Bool { phase == .checklist && workspaceReady && canBuild(states) && downloads.isEmpty && !installingTools && !removingBuildFiles }
    var destination: URL { choices.destinationURL }
    var existingApp: InstalledApp? { InstalledApp.find(in: destination) }
    func isDownloading(_ id: CheckID) -> Bool { downloads[id] != nil }

    var buildTitle: String {
        guard let old = existingApp else { return "Build Black & White 2" }
        return "Build and replace the app in \(abbreviated(old.url.deletingLastPathComponent()))"
    }

    var phaseName: String {
        switch phase {
        case .checklist: return "checklist"
        case .building: return "building"
        case .finished: return "finished"
        case .failed: return "failed"
        }
    }

    func start() {
        refresh()
        prepare(sweep: true) { [self] _ in refreshBuildTools() }
    }

    /// Checks every row again; the discs and patches only when their files changed or on request.
    func refresh() {
        guard phase == .checklist, !shuttingDown else { return }
        states[.mac] = currentMacCheck()
        refreshCommandLineTools()
        refreshBuildTools()
        refreshDiscs()
        CheckID.patchRows.forEach(refreshPatch)
        refreshPlaces()
    }

    private func saveChoices() {
        do { try choices.save(paths) } catch { notice = "Could not save your choices: \(error.localizedDescription)" }
    }

    private func refreshCommandLineTools() {
        DispatchQueue.global(qos: .userInitiated).async {
            let state = CommandLineTools().state()
            DispatchQueue.main.async { [self] in
                states[.commandLineTools] = state
                if state.isReady { cltTimer?.invalidate(); cltTimer = nil }
            }
        }
    }

    /// Apple's installer for the Command Line Tools; the row is checked again every 3 s until they are there.
    func installCommandLineTools() {
        DispatchQueue.global(qos: .userInitiated).async { CommandLineTools().startInstall() }
        cltTimer?.invalidate()
        cltTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refreshCommandLineTools()
        }
    }

    private func refreshBuildTools() {
        guard sources != nil, tables != nil else { states[.buildTools] = .blocked(incompleteBuilderText); return }
        if workspaceReady && !installingTools {
            states[.buildTools] = buildToolsCheck(paths: paths, version: version)
        }
    }

    private func refreshPlaces() {
        states[.destination] = destinationCheck(destination)
        states[.diskSpace] = diskSpaceState(paths: paths, version: version)
        objectWillChange.send()
    }

    private func refreshDiscs() {
        guard let tables = tables else { states[.discs] = .blocked(incompleteBuilderText); return }
        let urls = choices.discURLs
        let key = fingerprint(urls)
        if key == discKey { return }
        discKey = key
        let token = UUID()
        discToken = token
        discs = [:]
        states[.discs] = urls.isEmpty ? discsCheck([], zips: tables.zips, known: tables.known).0
            : .checking("Checking your discs...")
        guard !urls.isEmpty else { return }
        DispatchQueue.global(qos: .userInitiated).async {
            let (state, found) = discsCheck(urls, zips: tables.zips, known: tables.known)
            DispatchQueue.main.async { [self] in
                guard discToken == token else { return }
                states[.discs] = state
                discs = found
            }
        }
    }

    private func refreshPatch(_ id: CheckID) {
        guard let tables = tables, let kind = id.patchKind else { states[id] = .blocked(incompleteBuilderText); return }
        if downloads[id] != nil { return }
        if let error = downloadErrors[id] { states[id] = .blocked(error); patchKeys[id] = nil; return }
        let url = choices.patch(id)
        let key = fingerprint(url.map { [$0] } ?? [])
        if key == patchKeys[id] { return }
        patchKeys[id] = key
        let token = UUID()
        patchTokens[id] = token
        guard let file = url else { states[id] = patchCheck(nil, kind: kind, known: tables.known); return }
        states[id] = .checking("Checking \(file.lastPathComponent)...")
        DispatchQueue.global(qos: .userInitiated).async {
            let state = patchCheck(file, kind: kind, known: tables.known)
            DispatchQueue.main.async { [self] in
                guard patchTokens[id] == token else { return }
                states[id] = state
            }
        }
    }

    func setDiscs(_ urls: [URL]) {
        guard phase == .checklist, !urls.isEmpty else { return }
        choices.discs = urls.map { $0.path }
        saveChoices()
        discKey = nil
        refreshDiscs()
    }

    func setPatch(_ id: CheckID, _ url: URL) {
        guard phase == .checklist, id.patchKind != nil, downloads[id] == nil else { return }
        choices.setPatch(id, url)
        downloadErrors[id] = nil
        saveChoices()
        patchKeys[id] = nil
        refreshPatch(id)
    }

    func setDestination(_ url: URL) {
        guard phase == .checklist else { return }
        choices.destination = url.path
        saveChoices()
        refreshPlaces()
    }

    /// Files dropped on the window: exe files go to the patch row they are (or the first empty one), the rest
    /// to the disc row.
    func choose(_ urls: [URL]) {
        guard phase == .checklist, !urls.isEmpty else { return }
        let exes = urls.filter { $0.pathExtension.lowercased() == "exe" }
        let rest = urls.filter { $0.pathExtension.lowercased() != "exe" }
        if !rest.isEmpty { setDiscs(rest) }
        guard !exes.isEmpty, let tables = tables else { return }
        DispatchQueue.global(qos: .userInitiated).async {
            let matched = exes.map { url in
                (url, CheckID.patchRows.first { patchCheck(url, kind: $0.patchKind!, known: tables.known).isReady })
            }
            DispatchQueue.main.async { [self] in
                for (url, id) in matched {
                    if let id = id { setPatch(id, url) }
                    else { notice = "\(url.lastPathComponent) is not patch 1.1, patch 1.2 or the fan patch 1.42." }
                }
            }
        }
    }

    // MARK: - Build tools

    /// Copies the bundled sources into this version's workspace and links the shared venv. `sweep` (on start
    /// only) first deletes what interrupted work left and, once the workspace is ready, other versions' ones.
    private func prepare(sweep: Bool = false, _ completion: @escaping (Bool) -> Void) {
        if workspaceReady { return completion(true) }
        guard let sources = sources, tables != nil else {
            states[.buildTools] = .blocked(incompleteBuilderText)
            return completion(false)
        }
        states[.buildTools] = .checking("Preparing the build files...")
        let (paths, version) = (paths, version)
        DispatchQueue.global(qos: .userInitiated).async {
            if sweep { sweepLeftovers(paths) }
            let result = Result {
                try prepareWorkspace(sources: sources, destination: paths.workspace(version: version), version: version)
                try linkSharedVenv(paths, workspace: paths.workspace(version: version))
            }
            if sweep, case .success = result {
                DispatchQueue.global(qos: .utility).async { removeOldWorkspaces(paths, keeping: version) }
            }
            DispatchQueue.main.async { [self] in
                if case let .failure(error) = result {
                    states[.buildTools] = .blocked("Could not copy the build files: \(error.localizedDescription)")
                } else {
                    workspaceReady = true
                }
                completion(workspaceReady)
            }
        }
    }

    /// Set Up: the private Python (when it is missing), then `builder_cli setup`.
    func setUpBuildTools() {
        guard !installingTools, !shuttingDown, phase == .checklist else { return }
        installingTools = true
        states[.buildTools] = .checking("Setting up...")
        prepare { [self] ready in
            guard ready else { installingTools = false; return }
            if PythonRuntime.isInstalled(paths) { runSetup() } else { downloadPython() }
        }
    }

    private func downloadPython() {
        states[.buildTools] = .checking("Downloading Python...")
        PythonRuntime.install(paths, download: { url, done in
            let task = URLSession.shared.downloadTask(with: url) { file, response, error in
                if let file = file, (response as? HTTPURLResponse)?.statusCode == 200 {
                    done(.success(file))
                } else {
                    done(.failure(error ?? URLError(.badServerResponse)))
                }
            }
            var shown = -1
            pythonProgress = task.progress.observe(\.fractionCompleted) { progress, _ in
                let percent = Int(progress.fractionCompleted * 100)
                guard percent != shown else { return }
                shown = percent
                DispatchQueue.main.async { [self] in
                    guard pythonTask === task else { return }
                    states[.buildTools] = .checking(percent >= 100 ? "Unpacking Python..." : "Downloading Python, \(percent)%")
                }
            }
            pythonTask = task
            task.resume()
        }, completion: { [self] result in
            pythonTask = nil
            pythonProgress = nil
            switch result {
            case .success:
                runSetup()
            case let .failure(error):
                toolsFailed(error is URLError ? "Could not download Python. Check your internet connection and try again."
                            : Self.plain(error.localizedDescription))
            }
        })
    }

    private func runSetup() {
        states[.buildTools] = .checking("Installing the build's packages...")
        var error: String?
        var output: [String] = []
        run(.setup(paths: paths, version: version), onEvent: { event in
            switch event {
            case let .log(line): output.append(line)
            case let .error(message): error = message
            default: break
            }
        }, onExit: { [self] status in
            installingTools = false
            guard status == 0 else {
                return toolsFailed(error.map(Self.plain) ?? "Setting up the build tools failed (exit \(status)): "
                                   + (output.last ?? "no output"))
            }
            refreshBuildTools()
            if !toolsReady { toolsFailed("The build tools are incomplete. Try again.") }
        })
    }

    private func toolsFailed(_ message: String) {
        installingTools = false
        states[.buildTools] = .blocked(message)
    }

    // MARK: - Downloads

    /// Download for a patch row, through `builder_cli download` (pinned by SHA-256; PCGamingWiki's terms are
    /// accepted for the user). The file goes to the support folder's inputs and becomes the row's choice.
    func download(_ id: CheckID) {
        guard phase == .checklist, toolsReady, let kind = id.patchKind, downloads[id] == nil, !shuttingDown else { return }
        downloadErrors[id] = nil
        patchTokens[id] = UUID()
        states[id] = .checking("Downloading...")
        let runner = PipelineRunner.download(paths: paths, version: version, kind: kind)
        downloads[id] = runner
        var error: String?
        var file: URL?
        var lastLine = ""
        run(runner, onEvent: { [self] event in
            switch event {
            case let .progress(f): states[id] = .checking("Downloading, \(Int(f * 100))%")
            case let .error(message): error = message
            case let .done(url): file = url
            case let .log(line): if !line.isEmpty { lastLine = line }
            case .step: break
            }
        }, onExit: { [self] status in
            downloads[id] = nil
            if status == 0, error == nil, let file = file {
                choices.setPatch(id, file)
                saveChoices()
            } else if !shuttingDown {
                downloadErrors[id] = "Download failed: " + (error ?? "exit \(status), \(lastLine)")
            }
            patchKeys[id] = nil
            refreshPatch(id)
        })
    }

    // MARK: - Build, install, clean

    func build() {
        guard buildEnabled, !shuttingDown else { return }
        if isAppRunning(gameBundleID) {
            notice = "Quit Black & White 2 first, then try again."
            return
        }
        let inputs = buildInputs(discs: discs, choices: choices)
        let runner = PipelineRunner.build(paths: paths, version: version, inputs: inputs)
        (log, stepID, stepTitle, stepPart, buildError, built, cleanNote) = ([], "", "Getting ready", 0, nil, nil, nil)
        (userCancelled, cancelDisabled) = (false, false)
        (buildFilesRemoved, removeToolsToo, freedBytes) = (false, false, nil)
        measurementToken = UUID()
        phase = .building(title: stepTitle, fraction: 0)
        buildRunner = runner
        appendLog("Building from: " + inputs.map { $0.path }.joined(separator: ", "))
        run(runner, onEvent: { [self] event in
            switch event {
            case let .step(id, title):
                appendLog("== \(title)")
                (stepID, stepTitle, stepPart) = (id, title, 0)
                flushLog()
            case let .progress(part): stepPart = part
            case let .log(line): appendLog(line)
            case let .error(message):
                buildError = message
                appendLog("Error: \(message)")
            case let .done(app): built = app
            }
        }, onExit: { [self] status in
            buildRunner = nil
            if status != 0 { appendLog("The build exited with status \(status).") }
            if userCancelled || shuttingDown {
                phase = .checklist
                return refresh()
            }
            if status == 0, buildError == nil, let built = built {
                install(built)
            } else {
                fail(step: stepTitle, message: buildError ?? "The build stopped unexpectedly (exit \(status)).")
            }
        })
    }

    func cancelBuild() {
        guard let runner = buildRunner, !cancelDisabled else { return }
        userCancelled = true
        cancelDisabled = true
        appendLog("Cancelling...")
        runner.cancel()
    }

    private func install(_ built: URL) {
        cancelDisabled = true
        installing = true
        stepTitle = "Installing the app"
        appendLog("== Installing \(gameAppName) into \(destination.path)")
        phase = .building(title: stepTitle, fraction: 1)
        let directory = destination
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try installApp(built: built, into: directory, isRunning: isAppRunning) }
            DispatchQueue.main.async { [self] in
                installing = false
                if shuttingDown { return notifyIfIdle() }
                switch result {
                case let .success(app):
                    clean(then: app)
                case let .failure(error):
                    if let details = (error as? InstallError)?.details { appendLog(details) }
                    fail(step: stepTitle, message: error.localizedDescription)
                }
            }
        }
    }

    /// After a good install the workspace's gamefiles/, gen/ and out/ (but out/tools/) go; the app has all it needs.
    private func clean(then app: URL) {
        stepTitle = "Removing the build files"
        appendLog("== \(stepTitle)")
        phase = .building(title: stepTitle, fraction: 1)
        var error: String?
        run(.clean(paths: paths, version: version), onEvent: { [self] event in
            switch event {
            case let .log(line): appendLog(line)
            case let .error(message): error = message; appendLog("Error: \(message)")
            default: break
            }
        }, onExit: { [self] status in
            if status != 0 {
                cleanNote = "The build files could not be removed: " + (error ?? "exit \(status)")
                    + ". They are in \(abbreviated(paths.workspace(version: version)))."
            }
            phase = .finished(app)
            measureBuildFiles()
        })
    }

    private func measureBuildFiles() {
        (buildFilesBytes, buildToolsBytes) = (nil, nil)
        let token = UUID()
        measurementToken = token
        let (paths, version) = (paths, version)
        DispatchQueue.global(qos: .utility).async {
            let sizes = BuilderCore.measureBuildFiles(paths: paths, version: version)
            DispatchQueue.main.async { [self] in
                guard measurementToken == token else { return }
                (buildFilesBytes, buildToolsBytes) = (sizes.temporary, sizes.tools)
            }
        }
    }

    /// Cleanup runs to completion before quitting or returning to the checklist can start another build.
    func removeBuildFiles() {
        guard case let .finished(app) = phase, !hasRunningWork, !shuttingDown else { return }
        removingBuildFiles = true
        workspaceReady = false
        measurementToken = UUID()
        let (paths, version, includingTools) = (paths, version, removeToolsToo)
        DispatchQueue.global(qos: .utility).async {
            let result = Result {
                try BuilderCore.removeBuildFiles(paths: paths, version: version, includingTools: includingTools, preservingApp: app)
            }
            DispatchQueue.main.async { [self] in
                removingBuildFiles = false
                discKey = nil
                patchKeys = [:]
                switch result {
                case let .success(bytes):
                    buildFilesRemoved = true
                    freedBytes = bytes
                    cleanNote = nil
                case let .failure(error):
                    notice = "Could not delete all temporary files: \(error.localizedDescription)"
                    measureBuildFiles()
                }
                notifyIfIdle()
            }
        }
    }

    private func fail(step: String, message: String) {
        flushScheduled = false
        phase = .failed(step: step, message: Self.plain(message))
    }

    func backToChecklist() {
        guard !removingBuildFiles, !shuttingDown else { return }
        phase = .checklist
        refresh()
        prepare { [self] _ in refreshBuildTools() }
    }

    func play(_ app: URL) {
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        NSWorkspace.shared.openApplication(at: app, configuration: configuration) { [weak self] _, error in
            guard let error = error else { return }
            DispatchQueue.main.async { self?.notice = "Could not open \(gameAppName): \(error.localizedDescription)" }
        }
    }

    /// Lines arrive faster than SwiftUI should redraw, so the view is updated at most four times a second.
    private func appendLog(_ line: String) {
        log.append(line)
        guard !flushScheduled else { return }
        flushScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [self] in flushLog() }
    }

    private func flushLog() {
        flushScheduled = false
        guard case .building = phase else { return }
        let fraction = buildSteps.contains { $0.id == stepID } ? progressFraction(in: stepID, part: stepPart) : 0
        phase = .building(title: stepTitle, fraction: fraction)
    }

    // MARK: - Processes and quitting

    var hasRunningWork: Bool { !live.isEmpty || pythonTask != nil || installing || removingBuildFiles }

    private func run(_ runner: PipelineRunner, onEvent: @escaping (PipelineEvent) -> Void,
                     onExit: @escaping (Int32) -> Void) {
        guard !shuttingDown else { return }
        let key = ObjectIdentifier(runner)
        live[key] = runner
        do {
            try runner.start(onEvent: onEvent, onExit: { [self] status in
                live[key] = nil
                onExit(status)
                notifyIfIdle()
            })
        } catch {
            live[key] = nil
            DispatchQueue.main.async {
                onEvent(.error(error.localizedDescription))
                onExit(-1)
            }
        }
    }

    /// Cancels every running command (SIGTERM, then SIGKILL after 5 s) and calls `completion` once all have exited.
    func stopAll(then completion: @escaping () -> Void) {
        shuttingDown = true
        cltTimer?.invalidate()
        pythonTask?.cancel()
        pythonTask = nil
        idleHandlers.append(completion)
        live.values.forEach { $0.cancel() }
        notifyIfIdle()
    }

    private func notifyIfIdle() {
        guard !hasRunningWork, !idleHandlers.isEmpty else { return }
        let handlers = idleHandlers
        idleHandlers = []
        handlers.forEach { $0() }
    }

    /// Each path with the size and modification date of the file, or of a folder and the entries directly in it.
    private func fingerprint(_ urls: [URL]) -> [String] {
        func stamp(_ path: String) -> String {
            var info = stat()
            guard stat(path, &info) == 0 else { return "\(path) missing" }
            return "\(path) \(info.st_size) \(info.st_mtimespec.tv_sec).\(info.st_mtimespec.tv_nsec)"
        }
        return urls.flatMap { url -> [String] in
            let children = (try? FileManager.default.contentsOfDirectory(atPath: url.path))?.sorted() ?? []
            return [stamp(url.path)] + children.map { stamp(url.appendingPathComponent($0).path) }
        }
    }

    /// Pipeline messages start in lower case; the window shows them as sentences.
    static func plain(_ message: String) -> String {
        message.prefix(1).uppercased() + message.dropFirst()
    }
}
