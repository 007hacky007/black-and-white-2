import XCTest
import BuilderCore

/// The calls the Builder window makes, in the window's order, against the real discs and patches (plan 13
/// Task 8): the private Python (the real pinned download), setup, the disc and patch rows, the build, the
/// install, the clean. Skipped unless BW2_E2E_INPUTS names the inputs joined with ":" and BW2_E2E_SOURCES a
/// made Builder's Contents/Resources/source (tests/test_make_builder.py test_builder_e2e_window_calls sets both).
/// The support folder and the destination are temporary folders with spaces in their names, as
/// "~/Library/Application Support/BW2 Builder" has; the inputs are only read.
final class EndToEndTests: XCTestCase {
    private let fm = FileManager.default

    func testBuildFromWindowCalls() throws {
        let env = ProcessInfo.processInfo.environment
        guard let inputPaths = env["BW2_E2E_INPUTS"], let sourcePath = env["BW2_E2E_SOURCES"] else {
            throw XCTSkip("set BW2_E2E_INPUTS and BW2_E2E_SOURCES to run the end-to-end build")
        }
        let inputs = inputPaths.split(separator: ":").map { URL(fileURLWithPath: String($0)) }
        let sources = URL(fileURLWithPath: sourcePath)
        let version = try XCTUnwrap(try String(contentsOf: sources.appendingPathComponent("VERSION"), encoding: .utf8)
            .trimmingCharacters(in: .whitespacesAndNewlines))
        let tmp = fm.temporaryDirectory.appendingPathComponent("bw2 e2e \(UUID().uuidString)")
        try fm.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: tmp) }
        let paths = BuilderPaths(support: tmp.appendingPathComponent("BW2 Builder"))
        let destination = tmp.appendingPathComponent("Applications")
        let started = Date()
        var last = started
        func mark(_ what: String) {
            let now = Date()
            print(String(format: "E2E %7.1f s (+%6.1f s): ", now.timeIntervalSince(started), now.timeIntervalSince(last)) + what)
            last = now
        }

        // The window's start: sweep, the workspace, the shared venv link.
        sweepLeftovers(paths)
        try prepareWorkspace(sources: sources, destination: paths.workspace(version: version), version: version)
        try linkSharedVenv(paths, workspace: paths.workspace(version: version))
        mark("workspace prepared")

        // Set Up: the private Python, then setup.
        let installed = expectation(description: "python")
        var installResult: Result<Void, Error>?
        PythonRuntime.install(paths, download: { url, done in
            URLSession.shared.downloadTask(with: url) { file, response, error in
                if let file = file, (response as? HTTPURLResponse)?.statusCode == 200 {
                    done(.success(file))
                } else {
                    done(.failure(error ?? URLError(.badServerResponse)))
                }
            }.resume()
        }, completion: { result in
            installResult = result
            installed.fulfill()
        })
        wait(for: [installed], timeout: 900)
        try XCTUnwrap(installResult).get()
        XCTAssertTrue(PythonRuntime.isInstalled(paths))
        mark("python installed")

        let setup = try run(.setup(paths: paths, version: version), timeout: 900)
        XCTAssertEqual(setup.status, 0, setup.lines.suffix(30).joined(separator: "\n"))
        XCTAssertEqual(buildToolsCheck(paths: paths, version: version), .ready("Installed"))
        mark("setup done")

        // The rows: the inputs are what the window's pickers would have saved.
        let tables = try InputTables.load(sources: sources)
        var choices = BuilderChoices(destination: destination.path)
        var discURLs: [URL] = []
        for url in inputs {
            let lower = url.pathExtension.lowercased()
            if lower == "exe", let id = CheckID.patchRows.first(where: {
                patchCheck(url, kind: $0.patchKind!, known: tables.known).isReady
            }) {
                choices.setPatch(id, url)
            } else {
                discURLs.append(url)
            }
        }
        choices.discs = discURLs.map { $0.path }
        let (discState, discs) = discsCheck(choices.discURLs, zips: tables.zips, known: tables.known)
        XCTAssertEqual(discState, .ready("Discs 1-4 found"))
        for id in CheckID.patchRows {
            XCTAssertTrue(patchCheck(choices.patch(id), kind: id.patchKind!, known: tables.known).isReady, "\(id)")
        }
        var states = evaluateChecks(paths: paths, version: version, sources: sources, choices: choices)
        // BW2_REQUIRED_BYTES (a test switch, never set by the window) lowers the space need on a Mac whose free space
        // is short of the measured 20.1 GB; the row and the build's own check then both use it.
        let spaceNeed = env["BW2_REQUIRED_BYTES"].flatMap { Int64($0) }
        if let need = spaceNeed {
            print("E2E space: BW2_REQUIRED_BYTES=\(need) in place of \(requiredBuildBytes); the row said: \(states[.diskSpace]!.text)")
            states[.diskSpace] = diskSpaceCheck(available: availableBytes(at: paths.support) ?? 0, required: need)
        }
        XCTAssertTrue(canBuild(states), checkLines(states).joined(separator: "\n"))
        mark("rows ready")

        // Build.
        var builder = PipelineRunner.build(paths: paths, version: version, inputs: buildInputs(discs: discs, choices: choices))
        if let need = spaceNeed {
            builder = PipelineRunner(executable: builder.executable, arguments: builder.arguments,
                                     workingDirectory: builder.workingDirectory,
                                     environment: builder.environment.merging(["BW2_REQUIRED_BYTES": String(need)]) { $1 })
        }
        let build = try run(builder, timeout: 3000)
        XCTAssertEqual(build.status, 0, build.lines.suffix(60).joined(separator: "\n"))
        XCTAssertFalse(build.events.contains { if case .error = $0 { return true } else { return false } },
                       build.lines.suffix(60).joined(separator: "\n"))
        let steps = build.events.compactMap { event -> String? in
            if case let .step(id, _) = event { return id }
            return nil
        }
        XCTAssertEqual(steps, buildSteps.map { $0.id })
        for line in build.lines where line.hasPrefix("[builder] ") { print("E2E build: \(line)") }
        let built = try XCTUnwrap(build.events.compactMap { event -> URL? in
            if case let .done(app) = event { return app }
            return nil
        }.last)
        mark("built \(built.path)")

        // Install, then clean.
        let app = try installApp(built: built, into: destination, isRunning: { _ in false })
        XCTAssertEqual(app.lastPathComponent, gameAppName)
        XCTAssertEqual(InstalledApp(app)?.builderVersion, version)
        mark("installed")
        let clean = try run(.clean(paths: paths, version: version), timeout: 600)
        XCTAssertEqual(clean.status, 0, clean.lines.suffix(30).joined(separator: "\n"))
        let workspace = paths.workspace(version: version)
        XCTAssertFalse(fm.fileExists(atPath: workspace.appendingPathComponent("gamefiles").path))
        XCTAssertFalse(fm.fileExists(atPath: workspace.appendingPathComponent("gen").path))
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: workspace.appendingPathComponent("out").path), ["tools"])
        mark("cleaned")

        let verify = SystemCommandRunner().run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
        XCTAssertEqual(verify.status, 0, verify.output)
        print("E2E sizes: support after clean \(allocatedSize(of: paths.support)) bytes, app \(allocatedSize(of: app)) bytes")
        mark("installed and verified: \(app.path)")
    }

    // MARK: - Helpers

    private struct Run {
        var events: [PipelineEvent] = []
        var lines: [String] = []
        var status: Int32 = -1
    }

    private func run(_ runner: PipelineRunner, timeout: TimeInterval) throws -> Run {
        var result = Run()
        let exited = expectation(description: "exit")
        try runner.start(onEvent: { event in
            result.events.append(event)
            switch event {
            case let .log(line): result.lines.append(line)
            case let .step(_, title): result.lines.append(title); print("E2E step: \(title)")
            case let .error(message): result.lines.append("Error: \(message)")
            default: break
            }
        }, onExit: { status in
            result.status = status
            exited.fulfill()
        })
        wait(for: [exited], timeout: timeout)
        if result.status == -1 { runner.cancel() }
        return result
    }
}
