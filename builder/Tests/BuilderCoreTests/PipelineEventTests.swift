import XCTest
@testable import BuilderCore

final class PipelineEventTests: XCTestCase {
    func testTheFourShapes() {
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"step": "lift", "title": "Translating the game's code"}"#),
                       .step(id: "lift", title: "Translating the game's code"))
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"progress": 0.25}"#), .progress(0.25))
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"progress": 0.0}"#), .progress(0))
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"progress": 1}"#), .progress(1))
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"done": "/w/out/Black & White 2.app"}"#),
                       .done(URL(fileURLWithPath: "/w/out/Black & White 2.app")))
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"error": "Compiling: clang failed"}"#),
                       .error("Compiling: clang failed"))
    }

    func testEscapedTextArrivesDecoded() {
        XCTAssertEqual(parsePipelineLine(#"BW2-EVENT {"error": "missing: disc 3; d4.cue: the cue names \"xé.bin\""}"#),
                       .error(#"missing: disc 3; d4.cue: the cue names "xé.bin""#))
    }

    func testPlainTextAndBrokenEventsAreLog() {
        XCTAssertEqual(parsePipelineLine("plain"), .log("plain"))
        XCTAssertEqual(parsePipelineLine(""), .log(""))
        XCTAssertEqual(parsePipelineLine("[builder] lift: 125.3 s"), .log("[builder] lift: 125.3 s"))
        for line in [#"BW2-EVENT {"done": 7}"#, "BW2-EVENT {broken", #"BW2-EVENT {"step": "lift"}"#,
                     #"BW2-EVENT {"progress": "half"}"#, #"BW2-EVENT ["step"]"#, #"BW2-EVENT {}"#,
                     #"  BW2-EVENT {"progress": 1}"#, #"XX-EVENT {"progress": 1}"#, #"BW2-EVENT{"progress": 1}"#] {
            XCTAssertEqual(parsePipelineLine(line), .log(line), line)
        }
    }

    func testPrefix() {
        XCTAssertEqual(eventPrefix, "BW2-EVENT ")
    }
}
