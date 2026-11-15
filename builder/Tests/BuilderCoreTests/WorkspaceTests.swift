import XCTest
@testable import BuilderCore

final class WorkspaceTests: XCTestCase {
    private var tmp: URL!
    private let fm = FileManager.default

    override func setUpWithError() throws {
        tmp = try makeTempFolder("workspace")
    }

    override func tearDownWithError() throws {
        try? fm.removeItem(at: tmp)
    }

    func testBuilderPaths() {
        let paths = BuilderPaths(support: URL(fileURLWithPath: "/s"))
        XCTAssertEqual(paths.python.path, "/s/python")
        XCTAssertEqual(paths.pythonExecutable.path, "/s/python/bin/python3")
        XCTAssertEqual(paths.workspace(version: "0.1.0").path, "/s/0.1.0")
        XCTAssertEqual(paths.venv.path, "/s/venv")
        XCTAssertEqual(paths.venvPython.path, "/s/venv/bin/python")
        XCTAssertEqual(paths.inputs.path, "/s/inputs")
        XCTAssertEqual(BuilderPaths().support.lastPathComponent, "BW2 Builder")
        XCTAssertEqual(BuilderPaths().support.deletingLastPathComponent().lastPathComponent, "Application Support")
    }

    /// A Resources/source as Task 8 makes it: what bw2.workspace.copy_sources copies, with its SOURCES.txt.
    private func makeSources(_ root: URL, text: String = "new") throws {
        for entry in ["bw2/builder_cli.py", "runtime/bw2/main.c", "third_party/unshield/LICENSE", "tools/unshield/lib/x.h",
                      "requirements-builder.txt", "VERSION", "SOURCES.txt", "bw2/__pycache__/x.pyc",
                      "tests/test_x.py", "gamefiles/game/white.exe", "README.md"] {
            try writeText(text, to: root.appendingPathComponent(entry))
        }
    }

    func testWorkspaceCopiesOnlyBundledEntries() throws {
        XCTAssertEqual(bundledSourceEntries, ["bw2", "runtime", "third_party", "tools", "requirements-builder.txt", "VERSION",
                                              "SOURCES.txt"])
        let src = tmp.appendingPathComponent("source"), dst = tmp.appendingPathComponent("0.1.0")
        try makeSources(src)
        let value = Array("0081;00000000;Test;".utf8)
        XCTAssertEqual(setxattr(src.appendingPathComponent("bw2/builder_cli.py").path, "com.apple.quarantine", value, value.count, 0, 0), 0)
        try prepareWorkspace(sources: src, destination: dst, version: "0.1.0")
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: dst.path).sorted(),
                       [".builder-version", "SOURCES.txt", "VERSION", "bw2", "requirements-builder.txt", "runtime",
                        "third_party", "tools"])
        XCTAssertFalse(fm.fileExists(atPath: dst.appendingPathComponent("bw2/__pycache__").path))
        XCTAssertEqual(readText(dst.appendingPathComponent("bw2/builder_cli.py")), "new")
        errno = 0
        XCTAssertEqual(getxattr(dst.appendingPathComponent("bw2/builder_cli.py").path, "com.apple.quarantine", nil, 0, 0, 0), -1)
        XCTAssertEqual(errno, ENOATTR)
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: tmp.path).sorted(), ["0.1.0", "source"])
    }

    func testStaleOrHalfCopiedWorkspaceIsReplaced() throws {
        let src = tmp.appendingPathComponent("source"), dst = tmp.appendingPathComponent("ws")
        try makeSources(src)
        try writeText("old", to: dst.appendingPathComponent("bw2/builder_cli.py"))
        try writeText("0.0.9", to: dst.appendingPathComponent(".builder-version"))
        try prepareWorkspace(sources: src, destination: dst, version: "0.1.0")
        XCTAssertEqual(readText(dst.appendingPathComponent("bw2/builder_cli.py")), "new")
        XCTAssertEqual(readText(dst.appendingPathComponent(".builder-version")), "0.1.0")

        // No marker (a quit mid-copy) is replaced too.
        try fm.removeItem(at: dst.appendingPathComponent(".builder-version"))
        try writeText("junk", to: dst.appendingPathComponent("bw2/builder_cli.py"))
        try prepareWorkspace(sources: src, destination: dst, version: "0.1.0")
        XCTAssertEqual(readText(dst.appendingPathComponent("bw2/builder_cli.py")), "new")

        // The same version with its marker and every entry: left alone, with what a build made in it.
        try writeText("kept", to: dst.appendingPathComponent("bw2/builder_cli.py"))
        try writeText("ffmpeg", to: dst.appendingPathComponent("out/tools/ffmpeg-9.0.2/lib/x"))
        try prepareWorkspace(sources: src, destination: dst, version: "0.1.0")
        XCTAssertEqual(readText(dst.appendingPathComponent("bw2/builder_cli.py")), "kept")
        XCTAssertEqual(readText(dst.appendingPathComponent("out/tools/ffmpeg-9.0.2/lib/x")), "ffmpeg")
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: tmp.path).sorted(), ["source", "ws"])

        // The marker but an entry missing: copied again.
        try fm.removeItem(at: dst.appendingPathComponent("SOURCES.txt"))
        try prepareWorkspace(sources: src, destination: dst, version: "0.1.0")
        XCTAssertEqual(readText(dst.appendingPathComponent("bw2/builder_cli.py")), "new")
    }

    func testSweepRemovesOnlyLeftovers() throws {
        let paths = BuilderPaths(support: tmp)
        for name in [".trash-1/bw2/a.py", ".bw2-install-4/Black & White 2.app/Contents/Info.plist", ".0.1.0.tmp-2/VERSION", ".python.tmp-3/python/bin/python3",
                     "python/bin/python3", "0.1.0/.builder-version", "inputs/BW2Patch1_2.exe"] {
            try writeText("x", to: tmp.appendingPathComponent(name))
        }
        sweepLeftovers(paths)
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: tmp.path).sorted(), ["0.1.0", "inputs", "python"])
        XCTAssertEqual(readText(paths.pythonExecutable), "x")
    }

    func testOldVersionWorkspacesAreRemoved() throws {
        let paths = BuilderPaths(support: tmp)
        try writeText("0.0.9", to: tmp.appendingPathComponent("0.0.9/.builder-version"))
        try writeText("x", to: tmp.appendingPathComponent("0.0.9/bw2/a.py"))
        try writeText("0.1.0", to: tmp.appendingPathComponent("0.1.0/.builder-version"))
        try writeText("x", to: tmp.appendingPathComponent("notes/a.txt"))
        try writeText("x", to: paths.pythonExecutable)
        try writeText("x", to: paths.venvPython)
        try writeText("x", to: paths.inputs.appendingPathComponent("BW2Patch1_1.exe"))
        removeOldWorkspaces(paths, keeping: "0.1.0")
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: tmp.path).sorted(), ["0.1.0", "inputs", "notes", "python", "venv"])
    }

    func testMoveToTrashRenamesToHiddenSibling() throws {
        let dir = tmp.appendingPathComponent("0.1.0")
        try writeText("x", to: dir.appendingPathComponent("bw2/a.py"))
        let trash = try moveToTrash(dir)
        XCTAssertFalse(fm.fileExists(atPath: dir.path))
        XCTAssertTrue(trash.lastPathComponent.hasPrefix(".trash-"))
        XCTAssertEqual(trash.deletingLastPathComponent().path, tmp.path)
        XCTAssertEqual(readText(trash.appendingPathComponent("bw2/a.py")), "x")
    }

    func testWorkspaceLinksTheSharedVenv() throws {
        let paths = BuilderPaths(support: tmp)
        try writeText("", to: paths.venvPython)
        let ws = paths.workspace(version: "0.1.0")
        try writeText("old", to: ws.appendingPathComponent(".venv/bin/python"))
        try linkSharedVenv(paths, workspace: ws)
        let link = ws.appendingPathComponent(".venv")
        XCTAssertEqual(try fm.destinationOfSymbolicLink(atPath: link.path), paths.venv.path)
        XCTAssertEqual(readText(link.appendingPathComponent("bin/python")), "")
        try linkSharedVenv(paths, workspace: ws)
        XCTAssertEqual(try fm.destinationOfSymbolicLink(atPath: link.path), paths.venv.path, "a right link stays")
        try writeText("0.1.0", to: ws.appendingPathComponent(".builder-version"))
        removeOldWorkspaces(paths, keeping: "0.2.0")
        XCTAssertFalse(fm.fileExists(atPath: ws.path))
        XCTAssertTrue(fm.fileExists(atPath: paths.venvPython.path), "another version's workspace goes, the packages stay")
        XCTAssertFalse(try fm.contentsOfDirectory(atPath: tmp.path).contains { $0.hasPrefix(".trash-") })
    }

    func testBuildToolsBlockedUntilPythonAndVenvMatch() throws {
        let paths = BuilderPaths(support: tmp)
        let req = paths.workspace(version: "0.1.0").appendingPathComponent("requirements-builder.txt")
        try writeText("capstone==5.0.9\n", to: req)
        try writeText("", to: paths.venvPython)
        try writeText(try sha256Hex(of: req) + "\n", to: paths.venv.appendingPathComponent(".build-requirements-sha256"))
        XCTAssertEqual(buildToolsCheck(paths: paths, version: "0.1.0"),
                       .blocked("A private Python and the build's packages, about 40 MB download"))
        let exe = try writeText("#!/bin/sh\n", to: paths.pythonExecutable)
        XCTAssertEqual(chmod(exe.path, 0o755), 0)
        try writeText(PythonRuntime.sha256, to: paths.python.appendingPathComponent(PythonRuntime.markerName))
        XCTAssertEqual(buildToolsCheck(paths: paths, version: "0.1.0"), .ready("Installed"))
        let again = CheckState.blocked("The build's packages need to be set up again, about 13 MB download")
        try writeText("capstone==5.0.9\npefile==2024.8.26\n", to: req)
        XCTAssertEqual(buildToolsCheck(paths: paths, version: "0.1.0"), again)
        try writeText("capstone==5.0.9\n", to: req)
        XCTAssertEqual(buildToolsCheck(paths: paths, version: "0.1.0"), .ready("Installed"))
        try fm.removeItem(at: paths.venv)
        XCTAssertEqual(buildToolsCheck(paths: paths, version: "0.1.0"), again)
    }

    func testRemoveQuarantineRecursive() throws {
        let dir = tmp.appendingPathComponent("q")
        let file = try writeText("x", to: dir.appendingPathComponent("sub/f.txt"))
        let kept = try writeText("y", to: dir.appendingPathComponent("kept/g.txt"))
        let value = Array("0081;00000000;Test;".utf8)
        for url in [dir, file, kept] {
            XCTAssertEqual(setxattr(url.path, "com.apple.quarantine", value, value.count, 0, 0), 0)
        }
        try removeQuarantine(at: dir, keeping: dir.appendingPathComponent("kept"))
        for url in [dir, file] {
            errno = 0
            XCTAssertEqual(getxattr(url.path, "com.apple.quarantine", nil, 0, 0, 0), -1)
            XCTAssertEqual(errno, ENOATTR)
        }
        XCTAssertGreaterThan(getxattr(kept.path, "com.apple.quarantine", nil, 0, 0, 0), 0)
        try removeQuarantine(at: dir)
        XCTAssertEqual(getxattr(kept.path, "com.apple.quarantine", nil, 0, 0, 0), -1)
        try removeQuarantine(at: dir)
    }

    func testAllocatedSizeAndSizeText() throws {
        let dir = tmp.appendingPathComponent("ws")
        XCTAssertEqual(allocatedSize(of: dir), 0)
        try writeFile(Data(count: 3_000_000), to: dir.appendingPathComponent("gen/all.c"))
        try writeFile(Data(count: 1_000), to: dir.appendingPathComponent("small"))
        let size = allocatedSize(of: dir)
        XCTAssertGreaterThanOrEqual(size, 3_001_000)
        XCTAssertLessThan(size, 3_200_000)
        XCTAssertEqual(sizeText(612_000_000), "612 MB")
    }
}
