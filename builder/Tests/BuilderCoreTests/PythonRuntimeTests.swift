import XCTest
@testable import BuilderCore

final class PythonRuntimeTests: XCTestCase {
    private var tmp: URL!
    private let fm = FileManager.default

    override func setUpWithError() throws {
        tmp = try makeTempFolder("python")
    }

    override func tearDownWithError() throws {
        try? fm.removeItem(at: tmp)
    }

    func testPin() {
        XCTAssertEqual(PythonRuntime.url.absoluteString,
                       "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/"
                       + "cpython-3.14.10%2B20261026-aarch64-apple-darwin-install_only.tar.gz")
        XCTAssertEqual(PythonRuntime.sha256, "30a18bcb9ab75e485b4bf4e5d783dde165571e7e42c56dff47a2d2c5345183a4")
        XCTAssertEqual(PythonRuntime.markerName, ".bw2-python-sha256")
    }

    /// A tar.gz holding python/bin/python3, as python-build-standalone's install_only archives do.
    private func fakeArchive(_ name: String = "py.tar.gz", script: String = "#!/bin/sh\necho fake\n") throws -> URL {
        let stage = tmp.appendingPathComponent("stage-\(UUID().uuidString)")
        let exe = try writeText(script, to: stage.appendingPathComponent("python/bin/python3"))
        XCTAssertEqual(chmod(exe.path, 0o755), 0)
        let archive = tmp.appendingPathComponent(name)
        XCTAssertEqual(try runTool("/usr/bin/tar", ["-czf", archive.path, "-C", stage.path, "python"]).status, 0)
        try fm.removeItem(at: stage)
        return archive
    }

    private func install(_ paths: BuilderPaths, sha: String,
                         download: @escaping (URL, @escaping (Result<URL, Error>) -> Void) -> Void) -> Result<Void, Error> {
        var result: Result<Void, Error>?
        let done = expectation(description: "install")
        PythonRuntime.install(paths, sha256: sha, download: download) {
            XCTAssertTrue(Thread.isMainThread)
            result = $0
            done.fulfill()
        }
        wait(for: [done], timeout: 30)
        return result!
    }

    func testGoodArchiveInstallsAndWritesTheMarker() throws {
        let paths = BuilderPaths(support: tmp.appendingPathComponent("support"))
        let archive = try fakeArchive()
        let sha = try sha256Hex(of: archive)
        var asked: URL?
        let result = install(paths, sha: sha) { url, done in
            asked = url
            DispatchQueue.global().async { done(.success(archive)) }
        }
        guard case .success = result else { return XCTFail("\(result)") }
        XCTAssertEqual(asked, PythonRuntime.url)
        XCTAssertTrue(fm.isExecutableFile(atPath: paths.pythonExecutable.path))
        XCTAssertEqual(readText(paths.python.appendingPathComponent(".bw2-python-sha256")), sha)
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: paths.support.path), ["python"])
        XCTAssertFalse(fm.fileExists(atPath: archive.path), "the installer takes the archive and deletes it")
        XCTAssertFalse(PythonRuntime.isInstalled(paths), "installed with another pin than the real one")
        // isInstalled compares the marker with the real pin.
        try writeText(PythonRuntime.sha256, to: paths.python.appendingPathComponent(PythonRuntime.markerName))
        XCTAssertTrue(PythonRuntime.isInstalled(paths))
    }

    func testWrongChecksumIsRefusedAndNothingIsInstalled() throws {
        let support = tmp.appendingPathComponent("support")
        try fm.createDirectory(at: support, withIntermediateDirectories: true)
        let paths = BuilderPaths(support: support)
        let archive = try fakeArchive()
        let result = install(paths, sha: PythonRuntime.sha256) { _, done in done(.success(archive)) }
        guard case .failure(let error) = result else { return XCTFail("expected a failure") }
        XCTAssertTrue(error.localizedDescription.contains("checksum"), error.localizedDescription)
        XCTAssertFalse(fm.fileExists(atPath: paths.python.path))
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: support.path), [])
        XCTAssertFalse(PythonRuntime.isInstalled(paths))
    }

    func testWrongChecksumKeepsAnInstalledPython() throws {
        let paths = BuilderPaths(support: tmp.appendingPathComponent("support"))
        let exe = try writeText("#!/bin/sh\n", to: paths.pythonExecutable)
        XCTAssertEqual(chmod(exe.path, 0o755), 0)
        try writeText(PythonRuntime.sha256, to: paths.python.appendingPathComponent(PythonRuntime.markerName))
        let archive = try fakeArchive()
        guard case .failure = install(paths, sha: String(repeating: "0", count: 64), download: { _, done in done(.success(archive)) })
        else { return XCTFail("expected a failure") }
        XCTAssertTrue(PythonRuntime.isInstalled(paths))
    }

    func testDownloadFailureIsPassedOn() throws {
        let support = tmp.appendingPathComponent("support")
        try fm.createDirectory(at: support, withIntermediateDirectories: true)
        let paths = BuilderPaths(support: support)
        let result = install(paths, sha: PythonRuntime.sha256) { _, done in done(.failure(URLError(.notConnectedToInternet))) }
        guard case .failure(let error) = result else { return XCTFail("expected a failure") }
        XCTAssertEqual((error as? URLError)?.code, .notConnectedToInternet)
        XCTAssertEqual(try fm.contentsOfDirectory(atPath: support.path), [])
    }

    func testArchiveWithoutPythonIsRefused() throws {
        let paths = BuilderPaths(support: tmp.appendingPathComponent("support"))
        let stage = tmp.appendingPathComponent("stage")
        try writeText("x", to: stage.appendingPathComponent("other/file"))
        let archive = tmp.appendingPathComponent("other.tar.gz")
        XCTAssertEqual(try runTool("/usr/bin/tar", ["-czf", archive.path, "-C", stage.path, "other"]).status, 0)
        let sha = try sha256Hex(of: archive)
        guard case .failure(let error) = install(paths, sha: sha, download: { _, done in done(.success(archive)) })
        else { return XCTFail("expected a failure") }
        XCTAssertTrue(error.localizedDescription.contains("python/bin/python3"), error.localizedDescription)
        XCTAssertFalse(fm.fileExists(atPath: paths.python.path))
    }

    func testIsInstalledNeedsThePinnedMarker() throws {
        let paths = BuilderPaths(support: tmp)
        XCTAssertFalse(PythonRuntime.isInstalled(paths))
        let exe = try writeText("#!/bin/sh\n", to: paths.pythonExecutable)
        XCTAssertEqual(chmod(exe.path, 0o755), 0)
        XCTAssertFalse(PythonRuntime.isInstalled(paths))
        try writeText("oldpin", to: paths.python.appendingPathComponent(PythonRuntime.markerName))
        XCTAssertFalse(PythonRuntime.isInstalled(paths))
        try writeText(PythonRuntime.sha256 + "\n", to: paths.python.appendingPathComponent(PythonRuntime.markerName))
        XCTAssertTrue(PythonRuntime.isInstalled(paths))
    }

    /// A new private Python sets the packages up again: the old venv was made with the old one.
    func testNewPythonDropsTheVenv() throws {
        let paths = BuilderPaths(support: tmp.appendingPathComponent("support"))
        try writeText("", to: paths.venvPython)
        try writeText("old", to: paths.pythonExecutable)
        let archive = try fakeArchive()
        let sha = try sha256Hex(of: archive)
        guard case .success = install(paths, sha: sha, download: { _, done in done(.success(archive)) })
        else { return XCTFail("install failed") }
        XCTAssertEqual(readText(paths.pythonExecutable), "#!/bin/sh\necho fake\n")
        XCTAssertFalse(fm.fileExists(atPath: paths.venv.path))
        XCTAssertFalse(try fm.contentsOfDirectory(atPath: paths.support.path).contains { $0.hasPrefix(".") })
    }
}
