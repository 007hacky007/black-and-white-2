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
}
