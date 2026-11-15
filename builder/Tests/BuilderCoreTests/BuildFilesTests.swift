import XCTest
@testable import BuilderCore

final class BuildFilesTests: XCTestCase {
    private var root: URL!
    private let fm = FileManager.default

    override func setUpWithError() throws { root = try makeTempFolder("cleanup") }
    override func tearDownWithError() throws { try? fm.removeItem(at: root) }

    func testMeasuresAllocatedBytesAndCountsHardLinksOnceAcrossRoots() throws {
        let paths = BuilderPaths(support: root)
        let file = try writeFile(Data(repeating: 1, count: 8193), to: paths.workspace(version: "1.0").appendingPathComponent("data"))
        try fm.createDirectory(at: paths.inputs, withIntermediateDirectories: true)
        try fm.linkItem(at: file, to: paths.inputs.appendingPathComponent("link"))
        var info = stat()
        XCTAssertEqual(lstat(file.path, &info), 0)
        let expected = Int64(info.st_blocks) * 512
        let sizes = measureBuildFiles(paths: paths, version: "1.0")
        XCTAssertEqual(sizes.temporary, expected)
        XCTAssertEqual(sizes.tools, 0)
    }

    func testRemovalKeepsChoicesOtherVersionsOriginalsAndOptionalTools() throws {
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        let original = try writeText("disc", to: root.appendingPathComponent("player/disc.iso"))
        try writeText("work", to: paths.workspace(version: "1.0").appendingPathComponent("gen/code.c"))
        try writeText("patch", to: paths.inputs.appendingPathComponent("patch.exe"))
        try writeText("python", to: paths.pythonExecutable)
        try writeText("packages", to: paths.venvPython)
        try writeText("old", to: paths.workspace(version: "0.9").appendingPathComponent("data"))
        try writeText("choices", to: paths.choicesFile)
        try fm.createSymbolicLink(at: paths.workspace(version: "1.0").appendingPathComponent("disc"), withDestinationURL: original)
        let expected = measureBuildFiles(paths: paths, version: "1.0")
        XCTAssertEqual(try removeBuildFiles(paths: paths, version: "1.0", includingTools: false), expected.temporary)
        XCTAssertFalse(fm.fileExists(atPath: paths.workspace(version: "1.0").path))
        XCTAssertFalse(fm.fileExists(atPath: paths.inputs.path))
        XCTAssertEqual(readText(original), "disc")
        XCTAssertEqual(readText(paths.choicesFile), "choices")
        XCTAssertEqual(readText(paths.pythonExecutable), "python")
        XCTAssertEqual(readText(paths.workspace(version: "0.9").appendingPathComponent("data")), "old")
        XCTAssertEqual(try removeBuildFiles(paths: paths, version: "1.0", includingTools: true), expected.tools)
        XCTAssertFalse(fm.fileExists(atPath: paths.python.path))
        XCTAssertFalse(fm.fileExists(atPath: paths.venv.path))
        XCTAssertFalse(try fm.contentsOfDirectory(atPath: paths.support.path).contains { $0.hasPrefix(".trash-") })
    }

    func testNeverFollowsRootOrNestedSymlinksIncludingDanglingLinks() throws {
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        let outside = root.appendingPathComponent("player")
        try writeText("kept", to: outside.appendingPathComponent("save"))
        try fm.createDirectory(at: paths.support, withIntermediateDirectories: true)
        for url in [paths.workspace(version: "1.0"), paths.python, paths.venv] {
            try fm.createSymbolicLink(at: url, withDestinationURL: outside)
        }
        try fm.createSymbolicLink(at: paths.inputs, withDestinationURL: root.appendingPathComponent("missing"))
        XCTAssertEqual(allocatedSize(of: paths.python), 0)
        XCTAssertEqual(measureBuildFiles(paths: paths, version: "1.0").temporary, 0)
        XCTAssertEqual(measureBuildFiles(paths: paths, version: "1.0").tools, 0)
        XCTAssertEqual(try removeBuildFiles(paths: paths, version: "1.0", includingTools: true), 0)
        XCTAssertEqual(readText(outside.appendingPathComponent("save")), "kept")
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: paths.support.path), [])
    }

    func testUnsafeVersionsAndSupportSymlinkAreRefused() throws {
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        try writeText("kept", to: root.appendingPathComponent("player/save"))
        try fm.createSymbolicLink(at: paths.support, withDestinationURL: root.appendingPathComponent("player"))
        XCTAssertThrowsError(try removeBuildFiles(paths: paths, version: "1.0", includingTools: true))
        try fm.removeItem(at: paths.support)
        for version in ["..", "../player", "python", "inputs", "venv", ".", ""] {
            XCTAssertThrowsError(try removeBuildFiles(paths: paths, version: version, includingTools: false), version)
        }
        XCTAssertEqual(readText(root.appendingPathComponent("player/save")), "kept")
    }

    func testRefusesInstalledAppInAnySelectedRootBeforeRenamingAnything() throws {
        for (index, entry) in ["1.0", "1.0/install/Black & White 2.app", "inputs/Black & White 2.app",
                               "python/install/Black & White 2.app", "venv/Black & White 2.app"].enumerated() {
            let paths = BuilderPaths(support: root.appendingPathComponent("support-\(index)"))
            let app = paths.support.appendingPathComponent(entry)
            try writeText("game", to: app.appendingPathComponent("Contents/marker"))
            let work = try writeText("work", to: paths.workspace(version: "1.0").appendingPathComponent("gen/code.c"))
            let patch = try writeText("patch", to: paths.inputs.appendingPathComponent("patch.exe"))
            XCTAssertThrowsError(try removeBuildFiles(paths: paths, version: "1.0", includingTools: true, preservingApp: app)) { error in
                XCTAssertEqual(error.localizedDescription, "Move the game app outside the Builder's work folder before deleting temporary files.")
            }
            XCTAssertEqual(readText(app.appendingPathComponent("Contents/marker")), "game")
            XCTAssertEqual(readText(work), "work")
            XCTAssertEqual(readText(patch), "patch")
            XCTAssertFalse(try fm.contentsOfDirectory(atPath: paths.support.path).contains { $0.hasPrefix(".trash-") })
        }
    }

    func testResolvesLinkedDestinationAndAppBeforeCheckingCleanupScope() throws {
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        let directory = paths.workspace(version: "1.0").appendingPathComponent("install")
        let app = directory.appendingPathComponent(gameAppName)
        try writeText("game", to: app.appendingPathComponent("Contents/marker"))
        let alias = root.appendingPathComponent("Applications")
        try fm.createSymbolicLink(at: alias, withDestinationURL: directory)
        let appLink = root.appendingPathComponent("Game.app")
        try fm.createSymbolicLink(at: appLink, withDestinationURL: app)
        for selected in [alias.appendingPathComponent(gameAppName), appLink] {
            XCTAssertThrowsError(try removeBuildFiles(paths: paths, version: "1.0", includingTools: false, preservingApp: selected))
            XCTAssertEqual(readText(app.appendingPathComponent("Contents/marker")), "game")
        }
        let linkedPaths = BuilderPaths(support: root.appendingPathComponent("linked-support"))
        try fm.createDirectory(at: linkedPaths.support, withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: linkedPaths.inputs, withDestinationURL: directory)
        XCTAssertThrowsError(try removeBuildFiles(paths: linkedPaths, version: "1.0", includingTools: false, preservingApp: app))
        XCTAssertEqual(readText(app.appendingPathComponent("Contents/marker")), "game")
    }

    func testExternalAppIsKeptAndUnselectedToolsDoNotBlockCleanup() throws {
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        let app = paths.python.appendingPathComponent("install/Black & White 2.app")
        try writeText("game", to: app.appendingPathComponent("Contents/marker"))
        try writeText("work", to: paths.workspace(version: "1.0").appendingPathComponent("gen/code.c"))
        try removeBuildFiles(paths: paths, version: "1.0", includingTools: false, preservingApp: app)
        XCTAssertEqual(readText(app.appendingPathComponent("Contents/marker")), "game")
        let external = root.appendingPathComponent("Applications/Black & White 2.app")
        try writeText("external game", to: external.appendingPathComponent("Contents/marker"))
        try fm.createDirectory(at: paths.workspace(version: "1.0"), withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: paths.workspace(version: "1.0").appendingPathComponent("game"), withDestinationURL: external)
        try removeBuildFiles(paths: paths, version: "1.0", includingTools: true, preservingApp: external)
        XCTAssertEqual(readText(external.appendingPathComponent("Contents/marker")), "external game")
        XCTAssertFalse(fm.fileExists(atPath: paths.workspace(version: "1.0").path))
    }
}
