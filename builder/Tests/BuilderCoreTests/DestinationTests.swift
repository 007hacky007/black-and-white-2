import XCTest
@testable import BuilderCore

final class DestinationTests: XCTestCase {
    private var root: URL!
    private let fm = FileManager.default

    override func setUpWithError() throws {
        root = try makeTempFolder("dest")
    }

    override func tearDown() {
        try? fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: root.path)
        try? fm.removeItem(at: root)
    }

    private func makeApp(at url: URL, marker: String, builderVersion: String? = nil) throws {
        var info: [String: Any] = ["CFBundleIdentifier": gameBundleID, "CFBundleShortVersionString": "0.1.0"]
        if let v = builderVersion { info[builderVersionKey] = v }
        try fm.createDirectory(at: url.appendingPathComponent("Contents/Resources/game"), withIntermediateDirectories: true)
        XCTAssertTrue((info as NSDictionary).write(to: url.appendingPathComponent("Contents/Info.plist"), atomically: true))
        try writeText(marker, to: url.appendingPathComponent("Contents/marker"))
        try writeText("data \(marker)", to: url.appendingPathComponent("Contents/Resources/game/data.txt"))
    }

    private func marker(_ app: URL) -> String? { readText(app.appendingPathComponent("Contents/marker")) }

    private func listing(_ dir: URL) -> [String] { ((try? fm.contentsOfDirectory(atPath: dir.path)) ?? []).sorted() }

    func testInstallIntoAnEmptyFolder() throws {
        let dest = root.appendingPathComponent("Applications")
        try fm.createDirectory(at: dest, withIntermediateDirectories: true)
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new", builderVersion: "0.1.0")
        let value = Array("0081;00000000;Test;".utf8)
        XCTAssertEqual(setxattr(built.appendingPathComponent("Contents/marker").path, "com.apple.quarantine", value, value.count, 0, 0), 0)
        var asked: [String] = []
        let result = try installApp(built: built, into: dest, isRunning: { asked.append($0); return false })
        XCTAssertEqual(asked, ["local.bw2-port.game"])
        XCTAssertEqual(result.path, dest.appendingPathComponent("Black & White 2.app").path)
        XCTAssertEqual(marker(result), "new")
        XCTAssertEqual(InstalledApp(result)?.builderVersion, "0.1.0")
        XCTAssertEqual(listing(dest), ["Black & White 2.app"])
        XCTAssertFalse(fm.fileExists(atPath: built.path), "a build on the same volume is moved, not copied")
        errno = 0
        XCTAssertEqual(getxattr(result.appendingPathComponent("Contents/marker").path, "com.apple.quarantine", nil, 0, 0, 0), -1)
        XCTAssertEqual(errno, ENOATTR)
    }

    func testMissingFolderIsMade() throws {
        let dest = root.appendingPathComponent("Applications")
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new")
        let result = try installApp(built: built, into: dest, isRunning: { _ in false })
        XCTAssertEqual(marker(result), "new")
    }

    /// The saves live outside the app; a folder beside it (or anything else there) is never touched.
    func testReplaceAnOlderAppKeepingASiblingSavesFolder() throws {
        let dest = root.appendingPathComponent("Applications")
        try makeApp(at: dest.appendingPathComponent("Black & White 2.app"), marker: "old", builderVersion: "0.0.9")
        let saves = try writeText("my save", to: dest.appendingPathComponent("Saves/slot1.sav"))
        let savesDate = try fm.attributesOfItem(atPath: saves.path)[.modificationDate] as? Date
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new", builderVersion: "0.1.0")
        XCTAssertEqual(InstalledApp(dest.appendingPathComponent("Black & White 2.app"))?.builderVersion, "0.0.9")
        let result = try installApp(built: built, into: dest, isRunning: { _ in false })
        XCTAssertEqual(marker(result), "new")
        XCTAssertEqual(readText(result.appendingPathComponent("Contents/Resources/game/data.txt")), "data new")
        XCTAssertEqual(InstalledApp(result)?.builderVersion, "0.1.0")
        XCTAssertEqual(listing(dest), ["Black & White 2.app", "Saves"], "no .trash or .part left behind")
        XCTAssertEqual(readText(saves), "my save")
        XCTAssertEqual(try fm.attributesOfItem(atPath: saves.path)[.modificationDate] as? Date, savesDate)
        XCTAssertEqual(listing(dest.appendingPathComponent("Saves")), ["slot1.sav"])
    }

    func testRefuseWhileRunning() throws {
        let dest = root.appendingPathComponent("Applications")
        try makeApp(at: dest.appendingPathComponent("Black & White 2.app"), marker: "old")
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new")
        XCTAssertThrowsError(try installApp(built: built, into: dest, isRunning: { $0 == "local.bw2-port.game" })) { error in
            XCTAssertEqual(error.localizedDescription, "Quit Black & White 2 first")
        }
        XCTAssertEqual(marker(dest.appendingPathComponent("Black & White 2.app")), "old")
        XCTAssertEqual(marker(built), "new", "the build stays for another try")
        XCTAssertEqual(listing(dest), ["Black & White 2.app"])
    }

    /// The old app is renamed aside, then the new one is moved in; when that move fails, the old one goes back.
    func testAFailedMovePutsTheOldAppBack() throws {
        let dest = root.appendingPathComponent("Applications")
        let target = dest.appendingPathComponent("Black & White 2.app")
        try makeApp(at: target, marker: "old")
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new")
        var moves: [(String, String)] = []
        let failing: (URL, URL) throws -> Void = { from, to in
            moves.append((from.lastPathComponent, to.lastPathComponent))
            if to.path == target.path && !from.lastPathComponent.hasPrefix(".trash-") {
                throw CocoaError(.fileWriteUnknown)
            }
            try FileManager.default.moveItem(at: from, to: to)
        }
        XCTAssertThrowsError(try installApp(built: built, into: dest, isRunning: { _ in false }, move: failing)) { error in
            XCTAssertTrue(error.localizedDescription.hasPrefix("Could not install Black & White 2 into Applications"),
                          error.localizedDescription)
        }
        XCTAssertEqual(marker(target), "old")
        XCTAssertEqual(readText(target.appendingPathComponent("Contents/Resources/game/data.txt")), "data old")
        XCTAssertEqual(marker(built), "new", "the build is back where it was")
        XCTAssertEqual(listing(dest), ["Black & White 2.app"])
        XCTAssertTrue(moves.contains { $0.1.hasPrefix(".trash-") }, "the old app was moved aside first: \(moves)")
    }

    func testIncompleteBuildIsRefusedBeforeTheOldAppIsTouched() throws {
        let dest = root.appendingPathComponent("Applications")
        try makeApp(at: dest.appendingPathComponent("Black & White 2.app"), marker: "old")
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new")
        try fm.removeItem(at: built.appendingPathComponent("Contents/Info.plist"))
        XCTAssertThrowsError(try installApp(built: built, into: dest, isRunning: { _ in false }))
        XCTAssertEqual(marker(dest.appendingPathComponent("Black & White 2.app")), "old")
        XCTAssertEqual(marker(built), "new")
        XCTAssertEqual(listing(dest), ["Black & White 2.app"])
        XCTAssertThrowsError(try installApp(built: root.appendingPathComponent("nope.app"), into: dest, isRunning: { _ in false }))
        XCTAssertEqual(listing(dest), ["Black & White 2.app"])
    }

    func testUnwritableFolderKeepsTheOldApp() throws {
        let dest = root.appendingPathComponent("Applications")
        try makeApp(at: dest.appendingPathComponent("Black & White 2.app"), marker: "old")
        let built = root.appendingPathComponent("ws/out/Black & White 2.app")
        try makeApp(at: built, marker: "new")
        try fm.setAttributes([.posixPermissions: 0o555], ofItemAtPath: dest.path)
        defer { try? fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: dest.path) }
        XCTAssertThrowsError(try installApp(built: built, into: dest, isRunning: { _ in false }))
        XCTAssertEqual(marker(dest.appendingPathComponent("Black & White 2.app")), "old")
        XCTAssertEqual(marker(built), "new")
    }

    func testDestinationCheck() throws {
        XCTAssertEqual(destinationCheck(root), .ready("Black & White 2.app goes to \(abbreviated(root))"))
        let missing = root.appendingPathComponent("Applications")
        XCTAssertEqual(destinationCheck(missing), .ready("Black & White 2.app goes to \(abbreviated(missing))"),
                       "a missing folder in a writable one is made when installing")
        XCTAssertEqual(destinationCheck(root.appendingPathComponent("a/b")), .blocked("Choose a folder you can write to"))
        let file = try writeText("x", to: root.appendingPathComponent("file"))
        XCTAssertEqual(destinationCheck(file), .blocked("Choose a folder you can write to"))
        try fm.setAttributes([.posixPermissions: 0o555], ofItemAtPath: root.path)
        XCTAssertEqual(destinationCheck(root), .blocked("Choose a folder you can write to"))
    }

    func testDefaultDestination() {
        let home = URL(fileURLWithPath: "/Users/someone")
        XCTAssertEqual(chooseDestination(home: home, isWritable: { _ in true }).path, "/Applications")
        XCTAssertEqual(chooseDestination(home: home, isWritable: { $0 != "/Applications" }).path, "/Users/someone/Applications")
        XCTAssertTrue(["/Applications", fm.homeDirectoryForCurrentUser.appendingPathComponent("Applications").path]
            .contains(defaultDestination.path))
    }

    func testInstalledApp() throws {
        XCTAssertNil(InstalledApp(root.appendingPathComponent("none.app")))
        let app = root.appendingPathComponent("Black & White 2.app")
        try makeApp(at: app, marker: "x")
        XCTAssertNil(InstalledApp(app)?.builderVersion)
        XCTAssertEqual(InstalledApp(app)?.appVersion, "0.1.0")
        try fm.removeItem(at: app)
        try makeApp(at: app, marker: "x", builderVersion: "0.3.0")
        XCTAssertEqual(InstalledApp(app)?.builderVersion, "0.3.0")
        XCTAssertEqual(InstalledApp.find(in: root)?.url.path, app.path)
        XCTAssertEqual(builderVersionKey, "BW2BuilderVersion")
        XCTAssertEqual(gameAppName, "Black & White 2.app")
    }
}
