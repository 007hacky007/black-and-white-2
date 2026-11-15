import XCTest
@testable import BuilderCore

final class DiskSpaceTests: XCTestCase {
    func testRequiredBytesIsTheMeasuredNeed() {
        XCTAssertEqual(requiredBuildBytes, 13_100_000_000)
    }

    func testEnoughAndNotEnough() {
        let snapshotNote = ". Time Machine's local snapshots may be holding some of this disk's space."
        XCTAssertEqual(diskSpaceCheck(available: requiredBuildBytes), .ready("13.1 GB free"))
        XCTAssertEqual(diskSpaceCheck(available: 53_000_000_000), .ready("53.0 GB free"))
        XCTAssertEqual(diskSpaceCheck(available: requiredBuildBytes - 1), .blocked("Only 13.1 GB free, 13.1 GB needed" + snapshotNote))
        XCTAssertEqual(diskSpaceCheck(available: 4_000_000_000, required: 5_000_000_000), .blocked("Only 4.0 GB free, 5.0 GB needed" + snapshotNote))
        XCTAssertEqual(diskSpaceCheck(available: 1_000_000_000, required: 2_000_000_000, disk: "Macintosh HD"),
                       .blocked("Only 1.0 GB free on Macintosh HD, 2.0 GB needed" + snapshotNote))
    }

    func testAvailableBytesOfARealFolder() throws {
        let tmp = try makeTempFolder("space")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let free = try XCTUnwrap(availableBytes(at: tmp.appendingPathComponent("not yet/made")))
        XCTAssertGreaterThan(free, 0)
        XCTAssertNotNil(volumeName(of: tmp))
    }

    /// The row counts the space the build's own check counts (bw2/builder_cli.py check_space: shutil.disk_usage,
    /// statfs's f_bavail), not purgeable space, or the row could be green while the build refuses at once.
    func testAvailableBytesIsWhatStatfsSays() throws {
        let tmp = try makeTempFolder("space")
        defer { try? FileManager.default.removeItem(at: tmp) }
        var info = statfs()
        XCTAssertEqual(statfs(tmp.path, &info), 0)
        let statfsFree = Int64(info.f_bavail) * Int64(info.f_bsize)
        let free = try XCTUnwrap(availableBytes(at: tmp))
        XCTAssertLessThan(abs(free - statfsFree), 512 << 20, "\(free) against statfs \(statfsFree)")
    }

    /// What a cancelled or failed build leaves, as bw2/builder_cli.py clean removes it (gamefiles, gen, out but
    /// out/tools), which `build` does before its space check: counted as free, so one Cancel cannot block the row.
    /// Not counted: out/tools, links (never followed), files linked elsewhere too, and the APFS clones (gamefiles/game
    /// of install, an app in out of the game), whose shared blocks would be counted twice.
    func testALeftoverWorkspaceDoesNotBlockTheRow() throws {
        let support = try makeTempFolder("space")
        defer { try? FileManager.default.removeItem(at: support) }
        let paths = BuilderPaths(support: support)
        let ws = paths.workspace(version: "9.9.9")
        let mb = 1 << 20
        @discardableResult func put(_ rel: String, _ size: Int) throws -> URL {
            try writeFile(Data(repeating: 7, count: size), to: ws.appendingPathComponent(rel))
        }
        try put("gamefiles/install/data.bin", 48 * mb)
        try put("gen/white.c", 8 * mb)
        try put("out/bw2", 8 * mb)
        try put("out/tools/unshield/unshield", 4 * mb)
        try put("gamefiles/game/data.bin", 4 * mb)
        try put("out/Black & White 2.app.part/Contents/MacOS/bw2", 4 * mb)
        let outside = try writeFile(Data(repeating: 1, count: 4 * mb), to: support.appendingPathComponent("user.bin"))
        try FileManager.default.createDirectory(at: ws.appendingPathComponent("gamefiles/discs/d1"),
                                                withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: ws.appendingPathComponent("gamefiles/discs/d1/d1.bin"),
                                                   withDestinationURL: outside)
        try FileManager.default.createDirectory(at: ws.appendingPathComponent("gamefiles/patches"),
                                                withIntermediateDirectories: true)
        try FileManager.default.linkItem(at: outside, to: ws.appendingPathComponent("gamefiles/patches/p.exe"))

        XCTAssertEqual(reclaimableBuildBytes(workspace: ws), 0, "not a workspace without SOURCES.txt")
        try put("SOURCES.txt", 10)
        let reclaimable = reclaimableBuildBytes(workspace: ws)
        XCTAssertGreaterThanOrEqual(reclaimable, Int64(64 * mb))
        XCTAssertLessThan(reclaimable, Int64(65 * mb))

        XCTAssertEqual(diskSpaceCheck(available: 5_000_000_000, reclaimable: 9_000_000_000, required: 13_100_000_000),
                       .ready("5.0 GB free, and 9.0 GB of an earlier build's files that the build removes first"))
        XCTAssertEqual(diskSpaceCheck(available: 3_000_000_000, reclaimable: 9_000_000_000, required: 13_100_000_000,
                                      disk: "Macintosh HD"),
                       .blocked("Only 3.0 GB free on Macintosh HD (12.0 GB with an earlier build's files), 13.1 GB needed"
                                + snapshotNote))

        // The row itself: a need between the free space and the free space plus the leftovers.
        let available = try XCTUnwrap(availableBytes(at: support))
        let need = available + reclaimable / 2
        XCTAssertEqual(diskSpaceState(paths: paths, version: "9.9.9", required: need).name, "ready",
                       "\(diskSpaceState(paths: paths, version: "9.9.9", required: need))")
        XCTAssertEqual(diskSpaceState(paths: paths, version: "1.0.0", required: need).name, "blocked")
    }
}
