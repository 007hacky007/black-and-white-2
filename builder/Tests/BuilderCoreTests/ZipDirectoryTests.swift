import XCTest
@testable import BuilderCore

final class ZipDirectoryTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = try makeTempFolder("zip")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    /// A zip made with /usr/bin/zip: a deflated bin, a stored cue, and a folder.
    func testEntriesOfAZipMadeByZip() throws {
        let stage = tmp.appendingPathComponent("stage")
        var bin = Data()
        for i in 0..<20_000 { bin.append(contentsOf: [UInt8(i & 0xFF), UInt8((i >> 3) & 0xFF), 0x20]) }
        let cue = Data(#"FILE "Black & White 2 (USA) (Disc 1).bin" BINARY"#.utf8 + [0x0A])
        try writeFile(bin, to: stage.appendingPathComponent("Black & White 2 (USA) (Disc 1).bin"))
        try writeFile(cue, to: stage.appendingPathComponent("Black & White 2 (USA) (Disc 1).cue"))
        try writeFile(Data("x".utf8), to: stage.appendingPathComponent("sub/inner.txt"))
        let zip = tmp.appendingPathComponent("Disc 1 & more.zip")
        let r = try runTool("/usr/bin/zip", ["-r", "-X", zip.path, "Black & White 2 (USA) (Disc 1).bin",
                                             "Black & White 2 (USA) (Disc 1).cue", "sub"], in: stage)
        XCTAssertEqual(r.status, 0, r.output)

        let entries = try ZipDirectory.entries(zip)
        XCTAssertEqual(entries.map { $0.name }, ["Black & White 2 (USA) (Disc 1).bin", "Black & White 2 (USA) (Disc 1).cue",
                                                 "sub/", "sub/inner.txt"])
        XCTAssertEqual(entries[0], ZipEntry(name: "Black & White 2 (USA) (Disc 1).bin", size: Int64(bin.count),
                                            crc32: referenceCRC32(bin)))
        XCTAssertEqual(entries[1], ZipEntry(name: "Black & White 2 (USA) (Disc 1).cue", size: Int64(cue.count),
                                            crc32: referenceCRC32(cue)))
        XCTAssertTrue(entries[2].isDirectory)
        XCTAssertFalse(entries[3].isDirectory)
        XCTAssertEqual(entries[3].size, 1)
    }

    func testDamagedZipsThrow() throws {
        let stage = tmp.appendingPathComponent("stage")
        try writeFile(Data(repeating: 7, count: 5000), to: stage.appendingPathComponent("a.bin"))
        let zip = tmp.appendingPathComponent("a.zip")
        XCTAssertEqual(try runTool("/usr/bin/zip", ["-q", zip.path, "a.bin"], in: stage).status, 0)
        let data = try Data(contentsOf: zip)
        let truncated = try writeFile(data.prefix(data.count / 2), to: tmp.appendingPathComponent("half.zip"))
        XCTAssertThrowsError(try ZipDirectory.entries(truncated))
        let short = try writeFile(Data([0x50, 0x4B]), to: tmp.appendingPathComponent("short.zip"))
        XCTAssertThrowsError(try ZipDirectory.entries(short))
        let text = try writeText("not a zip at all, just some words", to: tmp.appendingPathComponent("text.zip"))
        XCTAssertThrowsError(try ZipDirectory.entries(text))
        XCTAssertThrowsError(try ZipDirectory.entries(tmp.appendingPathComponent("gone.zip")))
        // A directory whose stated offset runs past the file.
        var bent = data
        let eocd = bent.count - 22
        XCTAssertEqual(Array(bent[eocd..<(eocd + 4)]), [0x50, 0x4B, 0x05, 0x06])
        bent[eocd + 16] = 0xFF; bent[eocd + 17] = 0xFF; bent[eocd + 18] = 0xFF; bent[eocd + 19] = 0x00
        XCTAssertThrowsError(try ZipDirectory.entries(try writeFile(bent, to: tmp.appendingPathComponent("bent.zip"))))
    }

    /// Sizes over 4 GB live in the zip64 extra field; a zip64 end record points at the directory.
    func testZip64Records() throws {
        let name = Array("big.bin".utf8)
        let size: UInt64 = 5_000_000_000
        func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)] }
        func le32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8((v >> (8 * $0)) & 0xFF) } }
        func le64(_ v: UInt64) -> [UInt8] { (0..<8).map { UInt8((v >> (8 * UInt64($0))) & 0xFF) } }
        let extra = le16(1) + le16(16) + le64(size) + le64(size)
        var central = le32(0x0201_4B50) + le16(45) + le16(45) + le16(0) + le16(8) + le16(0) + le16(0)
        central += le32(0xDEAD_BEEF) + le32(0xFFFF_FFFF) + le32(0xFFFF_FFFF)
        central += le16(name.count) + le16(extra.count) + le16(0) + le16(0) + le16(0) + le32(0) + le32(0)
        central += name + extra
        let dirOffset: UInt64 = 0
        var file = central
        let zip64End = UInt64(file.count)
        file += le32(0x0606_4B50) + le64(44) + le16(45) + le16(45) + le32(0) + le32(0)
        file += le64(1) + le64(1) + le64(UInt64(central.count)) + le64(dirOffset)
        file += le32(0x0706_4B50) + le32(0) + le64(zip64End) + le32(1)
        file += le32(0x0605_4B50) + le16(0xFFFF) + le16(0xFFFF) + le16(0xFFFF) + le16(0xFFFF)
        file += le32(0xFFFF_FFFF) + le32(0xFFFF_FFFF) + le16(0)
        let zip = try writeFile(Data(file), to: tmp.appendingPathComponent("z64.zip"))
        XCTAssertEqual(try ZipDirectory.entries(zip), [ZipEntry(name: "big.bin", size: Int64(size), crc32: 0xDEAD_BEEF)])
    }
}
