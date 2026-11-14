import XCTest
@testable import BuilderCore

/// The ids and weights themselves are compared with bw2/builder_cli.py by tests/test_builder_swift.py.
final class BuildStepsTests: XCTestCase {
    func testWeightsAddUpTo100() {
        XCTAssertEqual(buildSteps.count, 7)
        XCTAssertEqual(buildSteps.reduce(0) { $0 + $1.weight }, 100)
        XCTAssertTrue(buildSteps.allSatisfy { $0.weight > 0 })
        XCTAssertEqual(Set(buildSteps.map { $0.id }).count, buildSteps.count)
    }

    func testFractions() {
        let first = buildSteps[0].id, last = buildSteps[buildSteps.count - 1].id
        XCTAssertEqual(progressFraction(atStart: first), 0)
        var sum = 0.0
        for step in buildSteps {
            XCTAssertEqual(progressFraction(atStart: step.id), sum / 100, accuracy: 1e-9)
            XCTAssertEqual(progressFraction(in: step.id, part: 0.5), (sum + step.weight / 2) / 100, accuracy: 1e-9)
            sum += step.weight
            XCTAssertEqual(progressFraction(in: step.id, part: 1), sum / 100, accuracy: 1e-9)
        }
        XCTAssertEqual(progressFraction(in: last, part: 1), 1, accuracy: 1e-9)
        XCTAssertEqual(progressFraction(in: last, part: 2), 1, accuracy: 1e-9, "a part above 1 is clamped")
        XCTAssertEqual(progressFraction(in: first, part: -1), 0, accuracy: 1e-9, "a part below 0 is clamped")
    }

    func testNamedSteps() {
        XCTAssertEqual(progressFraction(atStart: "patches"), 0.11, accuracy: 1e-9)
        XCTAssertEqual(progressFraction(in: "compile", part: 0.5), 0.795, accuracy: 1e-9)
    }
}
