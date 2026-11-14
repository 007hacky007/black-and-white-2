import XCTest
@testable import BuilderCore

final class RecordingRunner: CommandRunning {
    var calls: [String] = []
    let results: [String: (status: Int32, output: String)]
    init(results: [String: (status: Int32, output: String)]) { self.results = results }
    func run(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) {
        calls.append(([executable] + arguments).joined(separator: " "))
        return results[executable] ?? (127, "")
    }
}

final class CommandLineToolsTests: XCTestCase {
    let clt = "/Library/Developer/CommandLineTools"
    let xcode = "/Applications/Xcode.app/Contents/Developer"
    let missing = CheckState.blocked("Needed to compile the game on your Mac, about 2 GB from Apple")

    private func state(_ dir: String?, status: Int32 = 0, files: [String]) -> (CheckState, RecordingRunner) {
        let runner = RecordingRunner(results: ["/usr/bin/xcode-select": (status, dir.map { $0 + "\n" } ?? "")])
        let tools = CommandLineTools(runner: runner, fileExists: { files.contains($0) })
        return (tools.state(), runner)
    }

    func testAllPresent() {
        let (s, runner) = state(clt, files: [clt + "/usr/bin/clang", clt + "/usr/bin/make", "/usr/bin/codesign"])
        XCTAssertEqual(s, .ready("Installed"))
        XCTAssertEqual(runner.calls, ["/usr/bin/xcode-select -p"], "only xcode-select -p runs, never clang, make or xcrun")
    }

    func testXcodeToolchain() {
        let tc = xcode + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/"
        XCTAssertEqual(state(xcode, files: [tc + "clang", xcode + "/usr/bin/make", "/usr/bin/codesign"]).0, .ready("Installed"))
        XCTAssertEqual(state(xcode, files: [tc + "clang", tc + "make", "/usr/bin/codesign"]).0, .ready("Installed"))
    }

    func testMissingXcodeSelect() {
        XCTAssertEqual(state(nil, status: 2, files: []).0, missing)
        XCTAssertEqual(state("", status: 0, files: ["/usr/bin/codesign"]).0, missing, "an empty answer is no tools")
        let (s, _) = state(clt, status: 2, files: [clt + "/usr/bin/clang", clt + "/usr/bin/make", "/usr/bin/codesign"])
        XCTAssertEqual(s, missing, "a failed xcode-select -p is no tools, whatever it printed")
    }

    func testDirectoryWithoutClang() {
        XCTAssertEqual(state(clt, files: [clt + "/usr/bin/make", "/usr/bin/codesign"]).0, missing)
    }

    func testDirectoryWithoutMake() {
        XCTAssertEqual(state(clt, files: [clt + "/usr/bin/clang", "/usr/bin/codesign"]).0, missing)
    }

    func testWithoutCodesign() {
        XCTAssertEqual(state(clt, files: [clt + "/usr/bin/clang", clt + "/usr/bin/make"]).0, missing)
    }

    func testStartInstallRunsXcodeSelect() {
        let runner = RecordingRunner(results: [:])
        CommandLineTools(runner: runner, fileExists: { _ in false }).startInstall()
        XCTAssertEqual(runner.calls, ["/usr/bin/xcode-select --install"])
    }

    func testSystemRunnerReportsStatusAndOutput() {
        let r = SystemCommandRunner().run("/bin/sh", ["-c", "echo out; echo err >&2; exit 3"])
        XCTAssertEqual(r.status, 3)
        XCTAssertEqual(r.output, "out\nerr\n")
        XCTAssertEqual(SystemCommandRunner().run("/no/such/tool", []).status, 127)
    }
}
