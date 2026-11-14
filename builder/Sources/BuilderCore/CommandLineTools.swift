import Foundation

public protocol CommandRunning {
    func run(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String)
}

public struct SystemCommandRunner: CommandRunning {
    public init() {}

    /// Runs `executable` and waits; the output is stdout and stderr together, the status 127 when it cannot start.
    public func run(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
        } catch {
            return (127, "\(error)")
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: data, as: UTF8.self))
    }
}

/// Detects the Command Line Tools (clang and make for the build, codesign for the app) with
/// `xcode-select -p` and file checks only. It never runs clang, make or xcrun: on a Mac without the
/// tools those are shims that pop up Apple's installer (spec 7.5).
public struct CommandLineTools {
    let runner: CommandRunning
    let fileExists: (String) -> Bool

    public init(runner: CommandRunning = SystemCommandRunner(),
                fileExists: @escaping (String) -> Bool = { FileManager.default.fileExists(atPath: $0) }) {
        self.runner = runner
        self.fileExists = fileExists
    }

    public func state() -> CheckState {
        let missing = CheckState.blocked("Needed to compile the game on your Mac, about 2 GB from Apple")
        let result = runner.run("/usr/bin/xcode-select", ["-p"])
        let dir = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
        guard result.status == 0, !dir.isEmpty else { return missing }
        func tool(_ name: String) -> Bool {
            [dir + "/usr/bin/" + name, dir + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/" + name].contains(where: fileExists)
        }
        guard tool("clang"), tool("make"), fileExists("/usr/bin/codesign") else { return missing }
        return .ready("Installed")
    }

    /// Apple's installer for the Command Line Tools.
    public func startInstall() {
        _ = runner.run("/usr/bin/xcode-select", ["--install"])
    }
}
