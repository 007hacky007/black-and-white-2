import XCTest
@testable import BuilderCore

final class PatchCheckTests: XCTestCase {
    private var tmp: URL!
    private var known: KnownFiles!
    private var p11: URL!, p12: URL!, fan: URL!

    override func setUpWithError() throws {
        tmp = try makeTempFolder("patches")
        var rows: [String] = []
        func make(_ kind: String, _ name: String, _ file: String, _ text: String) throws -> URL {
            let data = Data(String(repeating: text, count: 50).utf8)
            rows.append([kind, name, String(data.count), sha256Text(data), "note"].joined(separator: "\t"))
            return try writeFile(data, to: tmp.appendingPathComponent(file))
        }
        p11 = try make("patch-1.1", "BW2Patch1_1.exe", "BW2Patch1_1.exe", "patch 1.1 ")
        p12 = try make("patch-1.2", "BW2Patch1_2.exe", "my 1.2 copy.exe", "patch 1.2 ")
        fan = try make("fanpatch-installer", "B&W2 Fan Patch v1.42 Installer.exe", "fan.exe", "fan 1.42 ")
        known = try KnownFiles(text: rows.joined(separator: "\n"))
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    func testGood() {
        XCTAssertEqual(patchCheck(p11, kind: "patch-1.1", known: known), .ready("BW2Patch1_1.exe"))
        XCTAssertEqual(patchCheck(p12, kind: "patch-1.2", known: known), .ready("BW2Patch1_2.exe"))
        XCTAssertEqual(patchCheck(fan, kind: "fanpatch-installer", known: known), .ready("B&W2 Fan Patch v1.42 Installer.exe"))
    }

    func testWrongKind() {
        XCTAssertEqual(patchCheck(p12, kind: "patch-1.1", known: known), .blocked("Not patch 1.1: my 1.2 copy.exe is patch 1.2"))
        XCTAssertEqual(patchCheck(p11, kind: "fanpatch-installer", known: known),
                       .blocked("Not fan patch 1.42: BW2Patch1_1.exe is patch 1.1"))
        XCTAssertEqual(patchCheck(fan, kind: "patch-1.2", known: known),
                       .blocked("Not patch 1.2: fan.exe is fan patch 1.42"))
    }

    func testUnknown() throws {
        let other = try writeText("some other installer", to: tmp.appendingPathComponent("setup.exe"))
        XCTAssertEqual(patchCheck(other, kind: "patch-1.2", known: known), .blocked("Not patch 1.2: setup.exe"))
        // The right size but other bytes.
        var bytes = try Data(contentsOf: p11)
        bytes[0] ^= 1
        let damaged = try writeFile(bytes, to: tmp.appendingPathComponent("damaged 1.1.exe"))
        XCTAssertEqual(patchCheck(damaged, kind: "patch-1.1", known: known), .blocked("Not patch 1.1: damaged 1.1.exe"))
        let gone = tmp.appendingPathComponent("gone.exe")
        XCTAssertEqual(patchCheck(gone, kind: "patch-1.1", known: known), .blocked("not found: \(gone.path)"))
    }

    func testNone() {
        for kind in ["patch-1.1", "patch-1.2", "fanpatch-installer"] {
            XCTAssertEqual(patchCheck(nil, kind: kind, known: known), .blocked("Download it, or choose the file you have"))
        }
    }

    func testIdentifyHashesOnlyOnASizeMatch() throws {
        let big = try writeFile(Data(count: 3_000_000), to: tmp.appendingPathComponent("big.exe"))
        XCTAssertNil(known.identify(big, kind: nil))
        XCTAssertEqual(known.identify(p12, kind: nil)?.kind, "patch-1.2")
        XCTAssertNil(known.identify(p12, kind: "patch-1.1"))
        XCTAssertEqual(try sha256Hex(of: p11), known.files[0].sha256)
    }
}
