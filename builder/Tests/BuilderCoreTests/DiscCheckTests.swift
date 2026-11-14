import XCTest
@testable import BuilderCore

/// Fake discs with tables that know them, as tests/test_builder_cli.py's FakeSet: disc 1 a zip, disc 2 a cue
/// and its bin, disc 3 a lone bin, disc 4 an ISO.
final class FakeDiscs {
    let root: URL
    var rows: [String] = []
    var zipRows: [String] = []
    var zip1: URL!, cue2: URL!, bin2: URL!, bin3: URL!, iso4: URL!

    static func discName(_ n: Int) -> String { "Black & White 2 (USA) (Disc \(n))" }

    init(_ root: URL) throws {
        self.root = root
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        zip1 = try zip(1, Data(String(repeating: "disc 1 ", count: 100).utf8), "d1.zip")
        bin2 = try write("d2.bin", Data(String(repeating: "disc 2 ", count: 100).utf8))
        cue2 = try write("d2.cue", Data("FILE \"d2.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n".utf8))
        known("disc", FakeDiscs.discName(2), try Data(contentsOf: bin2), note: "redump bin")
        bin3 = try write("d3.bin", Data(String(repeating: "disc 3 ", count: 100).utf8))
        known("disc", FakeDiscs.discName(3), try Data(contentsOf: bin3), note: "redump bin")
        iso4 = try write("d4.iso", Data(String(repeating: "disc 4 iso ", count: 100).utf8))
        known("disc", FakeDiscs.discName(4), try Data(contentsOf: iso4), note: "iso: 2048-byte user data of the redump bin")
        known("patch-1.1", "BW2Patch1_1.exe", Data(String(repeating: "patch 1.1 ", count: 50).utf8), note: "patch")
    }

    @discardableResult
    func write(_ name: String, _ data: Data) throws -> URL {
        try writeFile(data, to: root.appendingPathComponent(name))
    }

    func known(_ kind: String, _ name: String, _ data: Data, note: String = "") {
        rows.append([kind, name, String(data.count), sha256Text(data), note].joined(separator: "\t"))
    }

    /// A zip of the disc's bin and cue made with /usr/bin/zip, and its row (the CRC-32 given, or the right one).
    func zip(_ n: Int, _ bin: Data, _ name: String, crc: UInt32? = nil) throws -> URL {
        let stage = root.appendingPathComponent(".stage-\(UUID().uuidString)")
        let disc = FakeDiscs.discName(n)
        try writeFile(bin, to: stage.appendingPathComponent(disc + ".bin"))
        try writeText("FILE \"\(disc).bin\" BINARY\n", to: stage.appendingPathComponent(disc + ".cue"))
        let zip = root.appendingPathComponent(name)
        let r = try runTool("/usr/bin/zip", ["-q", "-X", zip.path, disc + ".bin", disc + ".cue"], in: stage)
        XCTAssertEqual(r.status, 0, r.output)
        try FileManager.default.removeItem(at: stage)
        zipRows.append("\(n)\t\(disc).bin\t\(bin.count)\t\(String(format: "%08x", crc ?? referenceCRC32(bin)))")
        return zip
    }

    func tables() throws -> (DiscZipTable, KnownFiles) {
        let zips = try DiscZipTable(text: "# disc\tbin\tsize\tcrc32\n" + zipRows.joined(separator: "\n") + "\n")
        let known = try KnownFiles(text: "# kind\tname\tsize\tsha256\tnote\n" + rows.joined(separator: "\n") + "\n")
        return (zips, known)
    }

    var all: [URL] { [zip1, cue2, bin3, iso4] }
}

final class DiscCheckTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = try makeTempFolder("discs")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    func testFourGoodDiscsAreReady() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let (zips, known) = try fake.tables()
        let result = discsCheck(fake.all, zips: zips, known: known)
        XCTAssertEqual(result.0, .ready("Discs 1-4 found"))
        XCTAssertEqual(result.discs, [1: fake.zip1, 2: fake.cue2, 3: fake.bin3, 4: fake.iso4])
        // The cue's bin given too is part of that disc, not a disc of its own.
        XCTAssertEqual(discsCheck(fake.all + [fake.bin2], zips: zips, known: known).0, .ready("Discs 1-4 found"))
    }

    func testAFolderStandsForTheFilesDirectlyInIt() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        try fake.write(".DS_Store", Data("x".utf8))
        try fake.write("notes.txt", Data("x".utf8))
        try fake.write("BW2Patch1_1.exe", Data(String(repeating: "patch 1.1 ", count: 50).utf8))
        try writeText("deeper", to: fake.root.appendingPathComponent("sub/d9.bin"))
        let (zips, known) = try fake.tables()
        let result = discsCheck([fake.root], zips: zips, known: known)
        XCTAssertEqual(result.0, .ready("Discs 1-4 found"))
        XCTAssertEqual(result.discs[1]?.lastPathComponent, "d1.zip")
        XCTAssertEqual(result.discs[2]?.lastPathComponent, "d2.cue")
        // The folder and a file in it: the same file once.
        XCTAssertEqual(discsCheck([fake.root, fake.bin3], zips: zips, known: known).0, .ready("Discs 1-4 found"))
    }

    func testZipWhoseCRCDiffersIsBlockedByName() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let bad = try fake.zip(1, Data(String(repeating: "disc 1 damaged ", count: 10).utf8), "bad d1.zip", crc: 0x1234_5678)
        fake.zipRows.removeFirst()           // the good disc 1 row stays out: the table knows only the bad CRC
        let (zips, known) = try fake.tables()
        let result = discsCheck([bad, fake.cue2, fake.bin3, fake.iso4], zips: zips, known: known)
        XCTAssertEqual(result.0, .blocked("missing: disc 1; not a known file: bad d1.zip"))
        XCTAssertEqual(result.discs, [2: fake.cue2, 3: fake.bin3, 4: fake.iso4])
    }

    func testDisc3Missing() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let (zips, known) = try fake.tables()
        XCTAssertEqual(discsCheck([fake.zip1, fake.cue2, fake.iso4], zips: zips, known: known).0, .blocked("missing: disc 3"))
    }

    func testCueWithoutItsBin() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let lone = try fake.write("d4.cue", Data("FILE \"gone.bin\" BINARY\n  TRACK 01 MODE1/2352\n".utf8))
        let (zips, known) = try fake.tables()
        XCTAssertEqual(discsCheck([fake.zip1, fake.cue2, fake.bin3, lone], zips: zips, known: known).0,
                       .blocked("missing: disc 4; d4.cue: the cue names gone.bin, which is not there"))
    }

    func testISORowMatches() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let (zips, known) = try fake.tables()
        XCTAssertEqual(known.identify(fake.iso4, kind: "disc")?.name, FakeDiscs.discName(4))
        XCTAssertEqual(discsCheck(fake.all, zips: zips, known: known).discs[4], fake.iso4)
        // An ISO of other bytes but the same size is not disc 4.
        let other = try fake.write("other.iso", Data(String(repeating: "disc 4 osi ", count: 100).utf8))
        XCTAssertEqual(discsCheck([fake.zip1, fake.cue2, fake.bin3, other], zips: zips, known: known).0,
                       .blocked("missing: disc 4; not a known file: other.iso"))
    }

    func testDuplicateUnknownAndNotFound() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let again = try fake.write("again.bin", try Data(contentsOf: fake.bin3))
        let stranger = try fake.write("setup.exe", Data("some other installer".utf8))
        let badCue = try fake.write("audio.cue", Data("FILE \"d3.bin\" BINARY\n  TRACK 01 AUDIO\n".utf8))
        let gone = tmp.appendingPathComponent("nowhere.zip")
        let (zips, known) = try fake.tables()
        XCTAssertEqual(discsCheck(fake.all + [again, stranger, badCue, gone], zips: zips, known: known).0,
                       .blocked("not found: \(gone.path); duplicate: disc 3; not a known file: setup.exe; "
                                + "not a known file: audio.cue"))
    }

    func testNothingChosen() throws {
        let fake = try FakeDiscs(tmp.appendingPathComponent("in"))
        let (zips, known) = try fake.tables()
        let result = discsCheck([], zips: zips, known: known)
        XCTAssertEqual(result.0, .blocked("Choose the four discs: their zips, .bin and .cue, or .iso files, or a folder"))
        XCTAssertEqual(result.discs, [:])
    }

    func testTheRealTablesLoad() throws {
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let zips = try DiscZipTable.load(repo.appendingPathComponent("bw2/manifests/disc-zips.tsv"))
        XCTAssertEqual(zips.rows.map { $0.disc }, [1, 2, 3, 4])
        XCTAssertEqual(zips.rows[0].bin, "Black & White 2 (USA) (Disc 1).bin")
        XCTAssertEqual(zips.rows[0].size, 788_886_672)
        XCTAssertEqual(zips.rows[0].crc32, 0x3FCB_89A3)
        let known = try KnownFiles.load(repo.appendingPathComponent("bw2/known_releases.tsv"))
        XCTAssertEqual(known.files.filter { $0.kind == "disc" }.count, 8)
        XCTAssertEqual(known.files.first { $0.kind == "patch-1.2" }?.size, 33_336_733)
    }

    func testInputMessagesFillTheirFieldsOnce() {
        XCTAssertEqual(inputMessage("missing", ["what": "disc 3"]), "missing: disc 3")
        XCTAssertEqual(inputMessage("cue_bin", ["name": "{bin}.cue", "bin": "a.bin"]),
                       "{bin}.cue: the cue names a.bin, which is not there", "a value is not filled again")
        XCTAssertEqual(inputMessage("space", ["volume": "/", "need": "2", "free": "1"]),
                       "not enough free space on /: the build needs 2 bytes and 1 are free")
        XCTAssertEqual(inputMessage("unknown", [:]), "not a known file: {name}")
    }

    func testDamagedTablesThrow() {
        XCTAssertThrowsError(try DiscZipTable(text: "1\tx.bin\tbig\t00000000\n"))
        XCTAssertThrowsError(try DiscZipTable(text: "1\tx.bin\t5\n"))
        XCTAssertThrowsError(try KnownFiles(text: "disc\tname\tsize\tabc\n"))
        XCTAssertThrowsError(try KnownFiles(text: "disc\tname\t5\n"))
    }
}
