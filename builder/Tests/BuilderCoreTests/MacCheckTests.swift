import XCTest
@testable import BuilderCore

final class MacCheckTests: XCTestCase {
    private func v(_ major: Int, _ minor: Int) -> OperatingSystemVersion {
        OperatingSystemVersion(majorVersion: major, minorVersion: minor, patchVersion: 0)
    }

    func testReady() {
        XCTAssertEqual(macCheck(isAppleSilicon: true, version: v(13, 0)), .ready("Apple Silicon, macOS 13.0"))
        XCTAssertEqual(macCheck(isAppleSilicon: true, version: v(27, 1)), .ready("Apple Silicon, macOS 27.1"))
    }

    func testTooOld() {
        XCTAssertEqual(macCheck(isAppleSilicon: true, version: v(12, 7)), .blocked("Black & White 2 needs macOS 13 or newer"))
        XCTAssertEqual(macCheck(isAppleSilicon: true, version: v(11, 0)), .blocked("Black & White 2 needs macOS 13 or newer"))
    }

    func testIntel() {
        XCTAssertEqual(macCheck(isAppleSilicon: false, version: v(15, 0)),
                       .blocked("Black & White 2 needs a Mac with Apple Silicon (M1 or later)"))
        XCTAssertEqual(macCheck(isAppleSilicon: false, version: v(12, 0)),
                       .blocked("Black & White 2 needs a Mac with Apple Silicon (M1 or later)"), "Intel is named first")
    }

    func testThisMac() {
        XCTAssertTrue(isAppleSilicon(), "the port is built and tested on Apple Silicon")
        XCTAssertTrue(currentMacCheck().isReady)
    }

    func testCanBuildNeedsEveryRow() {
        XCTAssertEqual(CheckID.allCases, [.mac, .commandLineTools, .buildTools, .discs, .patch11, .patch12, .fanPatch,
                                          .destination, .diskSpace])
        var s = Dictionary(uniqueKeysWithValues: CheckID.allCases.map { ($0, CheckState.ready("ok")) })
        XCTAssertTrue(canBuild(s))
        s[.patch11] = .automatic("downloads when you build"); XCTAssertTrue(canBuild(s))
        s[.discs] = .checking("Checking the discs"); XCTAssertFalse(canBuild(s))
        s[.discs] = .blocked("missing: disc 3"); XCTAssertFalse(canBuild(s))
        s[.discs] = .ready("Discs 1-4 found"); s.removeValue(forKey: .diskSpace); XCTAssertFalse(canBuild(s))
    }

    func testStateFlags() {
        XCTAssertTrue(CheckState.ready("x").isReady)
        XCTAssertFalse(CheckState.ready("x").isBlocked)
        XCTAssertTrue(CheckState.blocked("x").isBlocked)
        XCTAssertFalse(CheckState.checking("x").isReady)
        XCTAssertEqual(CheckState.automatic("a").text, "a")
        XCTAssertEqual(CheckState.blocked("b").text, "b")
    }
}
