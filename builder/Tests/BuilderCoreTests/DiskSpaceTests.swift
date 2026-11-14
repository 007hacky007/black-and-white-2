import XCTest
@testable import BuilderCore

final class DiskSpaceTests: XCTestCase {
    func testRequiredBytesIsTheMeasuredNeed() {
        XCTAssertEqual(requiredBuildBytes, 20_100_000_000)
    }

    func testEnoughAndNotEnough() {
        XCTAssertEqual(diskSpaceCheck(available: requiredBuildBytes), .ready("20.1 GB free"))
        XCTAssertEqual(diskSpaceCheck(available: 53_000_000_000), .ready("53.0 GB free"))
        XCTAssertEqual(diskSpaceCheck(available: requiredBuildBytes - 1), .blocked("Only 20.1 GB free, 20.1 GB needed"))
        XCTAssertEqual(diskSpaceCheck(available: 4_000_000_000, required: 5_000_000_000), .blocked("Only 4.0 GB free, 5.0 GB needed"))
        XCTAssertEqual(diskSpaceCheck(available: 1_000_000_000, required: 2_000_000_000, disk: "Macintosh HD"),
                       .blocked("Only 1.0 GB free on Macintosh HD, 2.0 GB needed"))
    }

    func testAvailableBytesOfARealFolder() throws {
        let tmp = try makeTempFolder("space")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let free = try XCTUnwrap(availableBytes(at: tmp.appendingPathComponent("not yet/made")))
        XCTAssertGreaterThan(free, 0)
        XCTAssertNotNil(volumeName(of: tmp))
    }
}
