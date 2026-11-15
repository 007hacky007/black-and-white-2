import XCTest
@testable import BuilderCore

final class ChoicesTests: XCTestCase {
    func testMissingOrDamagedFileIsNothingChosen() throws {
        let paths = BuilderPaths(support: try makeTempFolder("choices").appendingPathComponent("support"))
        XCTAssertEqual(BuilderChoices.load(paths), BuilderChoices())
        try writeText("{not json", to: paths.choicesFile)
        XCTAssertEqual(BuilderChoices.load(paths), BuilderChoices())
        try writeText(#"{"patch12": "/x/BW2Patch1_2.exe", "unknown": 3}"#, to: paths.choicesFile)
        XCTAssertEqual(BuilderChoices.load(paths), BuilderChoices(patch12: "/x/BW2Patch1_2.exe"))
    }

    func testSaveAndLoad() throws {
        let paths = BuilderPaths(support: try makeTempFolder("choices").appendingPathComponent("new support"))
        var choices = BuilderChoices(discs: ["/a b/disc 1.zip", "/c & d"], destination: "/Applications")
        choices.setPatch(.patch11, URL(fileURLWithPath: "/p/BW2Patch1_1.exe"))
        choices.setPatch(.fanPatch, URL(fileURLWithPath: "/p/B&W2 Fan Patch v1.42 Installer.exe"))
        try choices.save(paths)
        let loaded = BuilderChoices.load(paths)
        XCTAssertEqual(loaded, choices)
        XCTAssertEqual(loaded.patch(.patch11)?.path, "/p/BW2Patch1_1.exe")
        XCTAssertNil(loaded.patch(.patch12))
        XCTAssertEqual(loaded.patch(.fanPatch)?.lastPathComponent, "B&W2 Fan Patch v1.42 Installer.exe")
        XCTAssertNil(loaded.patch(.discs))
        XCTAssertEqual(loaded.destinationURL.path, "/Applications")
    }

    func testPatchKinds() {
        XCTAssertEqual(CheckID.patchRows.map { $0.patchKind }, ["patch-1.1", "patch-1.2", "fanpatch-installer"])
        XCTAssertNil(CheckID.discs.patchKind)
        XCTAssertEqual(CheckID.patchRows.compactMap { $0.patchKind }, patchKinds.map { $0.kind })
    }

    func testCheckLines() {
        var states: [CheckID: CheckState] = [:]
        for id in CheckID.allCases { states[id] = .blocked("no / \"quoted\"") }
        states[.mac] = .ready("Apple Silicon, macOS 27.0")
        states[.diskSpace] = .automatic("x")
        let lines = checkLines(states)
        XCTAssertEqual(lines.count, 9)
        XCTAssertEqual(lines[0], #"{"id":"mac","state":"ready","text":"Apple Silicon, macOS 27.0"}"#)
        XCTAssertEqual(lines[3], #"{"id":"discs","state":"blocked","text":"no / \"quoted\""}"#)
        XCTAssertEqual(lines[8], #"{"id":"diskSpace","state":"automatic","text":"x"}"#)
    }

    func testEvaluateWithoutSourcesNamesTheBuilder() throws {
        let paths = BuilderPaths(support: try makeTempFolder("eval").appendingPathComponent("support"))
        let states = evaluateChecks(paths: paths, version: "0.1.0", sources: nil, choices: BuilderChoices())
        XCTAssertEqual(Set(states.keys), Set(CheckID.allCases))
        for id in [CheckID.buildTools, .discs, .patch11, .patch12, .fanPatch] {
            XCTAssertEqual(states[id], .blocked(incompleteBuilderText), "\(id)")
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: paths.support.path), "the checks write nothing")
    }

    func testEvaluateWithTables() throws {
        let root = try makeTempFolder("eval")
        let sources = root.appendingPathComponent("source")
        let patch = Data("patch one point one".utf8)
        try writeText("# kind\tname\tsize\tsha256\tnote\npatch-1.1\tBW2Patch1_1.exe\t\(patch.count)\t\(sha256Text(patch))\t\n",
                      to: sources.appendingPathComponent("bw2/known_releases.tsv"))
        try writeText("# disc\tbin\tsize\tcrc32\n", to: sources.appendingPathComponent("bw2/manifests/disc-zips.tsv"))
        let file = try writeFile(patch, to: root.appendingPathComponent("mine/whatever.exe"))
        let paths = BuilderPaths(support: root.appendingPathComponent("support"))
        let dest = root.appendingPathComponent("Apps")
        try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
        let choices = BuilderChoices(patch11: file.path, patch12: file.path, destination: dest.path)
        let states = evaluateChecks(paths: paths, version: "0.1.0", sources: sources, choices: choices)
        XCTAssertEqual(states[.patch11], .ready("BW2Patch1_1.exe"))
        XCTAssertEqual(states[.patch12], .blocked("Not patch 1.2: whatever.exe is patch 1.1"))
        XCTAssertEqual(states[.fanPatch], .blocked("Download it, or choose the file you have"))
        XCTAssertEqual(states[.discs]?.isBlocked, true)
        XCTAssertEqual(states[.buildTools]?.isBlocked, true)
        XCTAssertEqual(states[.destination]?.isReady, true)
        XCTAssertNotNil(states[.diskSpace])
        XCTAssertFalse(FileManager.default.fileExists(atPath: paths.support.path), "the checks write nothing")
    }

    func testBuildInputsOrder() {
        let discs = [3: URL(fileURLWithPath: "/d/3.zip"), 1: URL(fileURLWithPath: "/d/1.cue"),
                     4: URL(fileURLWithPath: "/d/4.iso"), 2: URL(fileURLWithPath: "/d/2.bin")]
        let choices = BuilderChoices(patch11: "/p/11.exe", patch12: "/p/12.exe", fanPatch: "/p/fan.exe")
        XCTAssertEqual(buildInputs(discs: discs, choices: choices).map { $0.path },
                       ["/d/1.cue", "/d/2.bin", "/d/3.zip", "/d/4.iso", "/p/11.exe", "/p/12.exe", "/p/fan.exe"])
    }
}
