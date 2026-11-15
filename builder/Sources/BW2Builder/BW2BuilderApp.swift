import AppKit
import BuilderCore
import SwiftUI

/// What the command line asked for (plan 13 Task 8):
///
///     BW2Builder [--support DIR] [--print-checks]
///
/// `--support DIR` replaces the default support folder (~/Library/Application Support/BW2 Builder) for every use;
/// Foundation ignores HOME for that lookup, so only this or CFFIXED_USER_HOME redirects it. `--print-checks` checks
/// every row once with the saved choices, prints one JSON line per row and exits 0, with no window. Finder's
/// `-psn_...` and AppKit's `-Name value` defaults pairs are passed over.
struct LaunchOptions {
    var support: URL?
    var printChecks = false

    static var current = LaunchOptions()

    static func parse(_ arguments: [String]) -> Result<LaunchOptions, LaunchError> {
        var options = LaunchOptions()
        var rest = arguments[...]
        while let arg = rest.popFirst() {
            switch arg {
            case "--support":
                guard let dir = rest.popFirst(), !dir.isEmpty else { return .failure(LaunchError("--support needs a folder")) }
                options.support = URL(fileURLWithPath: dir).standardizedFileURL
            case "--print-checks":
                options.printChecks = true
            case _ where arg.hasPrefix("-psn_"):
                continue
            case _ where arg.hasPrefix("-") && !arg.hasPrefix("--") && arg.count > 1:
                _ = rest.popFirst()       // an NSUserDefaults argument pair, as Xcode passes
            default:
                return .failure(LaunchError("unknown argument \(arg)"))
            }
        }
        return .success(options)
    }

    var paths: BuilderPaths { support.map { BuilderPaths(support: $0) } ?? BuilderPaths() }

    /// The Builder's own version (BW2BuilderVersion of its Info.plist): it names the workspace.
    static var version: String {
        (Bundle.main.infoDictionary?[builderVersionKey] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "0.0.0-dev"
    }

    /// Contents/Resources/source, when it is a complete copy (bw2.workspace.copy_sources wrote its SOURCES.txt).
    static var sources: URL? {
        guard let source = Bundle.main.resourceURL?.appendingPathComponent("source"),
              FileManager.default.fileExists(atPath: source.appendingPathComponent("SOURCES.txt").path),
              FileManager.default.fileExists(atPath: source.appendingPathComponent("bw2/builder_cli.py").path)
        else { return nil }
        return source
    }
}

struct LaunchError: Error {
    let message: String
    init(_ message: String) { self.message = message }
}

@main
enum BuilderMain {
    static func main() {
        switch LaunchOptions.parse(Array(CommandLine.arguments.dropFirst())) {
        case let .failure(error):
            FileHandle.standardError.write(Data("BW2 Builder: \(error.message)\nusage: BW2Builder [--support DIR] [--print-checks]\n".utf8))
            exit(2)
        case let .success(options):
            LaunchOptions.current = options
        }
        if LaunchOptions.current.printChecks {
            let states = evaluateChecks(paths: LaunchOptions.current.paths, version: LaunchOptions.version,
                                        sources: LaunchOptions.sources, choices: BuilderChoices.load(LaunchOptions.current.paths))
            for line in checkLines(states) { print(line) }
            exit(0)
        }
        BW2BuilderApp.main()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = BuilderModel(paths: LaunchOptions.current.paths, version: LaunchOptions.version,
                             sources: LaunchOptions.sources)
    private var terminateSignal: DispatchSourceSignal?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Needed when started outside a bundle (swift run), where nothing says this is a regular app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        // The pipeline runs in its own process group, so a plain SIGTERM to the Builder would orphan it: take
        // the normal quit path instead, which cancels it first.
        signal(SIGTERM, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        source.setEventHandler { RunLoop.main.perform { NSApp.terminate(nil) } }
        source.resume()
        terminateSignal = source
        model.start()
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        model.refresh()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// Quitting while a command runs cancels it first (SIGTERM, then SIGKILL after 5 s) and quits once it has
    /// exited. NSTerminateCancel, not NSTerminateLater (spec 7.5): the quit is asked for again when idle.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard model.hasRunningWork else { return .terminateNow }
        if !model.shuttingDown {
            model.stopAll { RunLoop.main.perform { NSApp.terminate(nil) } }
        }
        return .terminateCancel
    }
}

struct BW2BuilderApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        WindowGroup("BW2 Builder") {
            RootView(model: delegate.model)
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .appTermination) {
                Button("Quit BW2 Builder") { NSApp.terminate(nil) }.keyboardShortcut("q")
            }
        }
    }
}

struct RootView: View {
    @ObservedObject var model: BuilderModel

    var body: some View {
        Group {
            switch model.phase {
            case .checklist:
                ChecklistView(model: model)
            case let .building(title, fraction):
                BuildView(model: model, title: title, fraction: fraction)
            case let .finished(app):
                ResultView(model: model, outcome: .success(app))
            case let .failed(step, message):
                ResultView(model: model, outcome: .failure(step: step, message: message))
            }
        }
        .alert(isPresented: Binding(get: { model.notice != nil }, set: { if !$0 { model.notice = nil } })) {
            Alert(title: Text(model.notice ?? ""))
        }
        .padding(20)
        .frame(width: 600)
        .frame(minHeight: 560, alignment: .top)
        .background(Color(NSColor.windowBackgroundColor))
    }
}
