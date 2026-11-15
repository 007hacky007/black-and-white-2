import Foundation

/// The Builder's rows, in the window's order.
public enum CheckID: String, CaseIterable {
    case mac, commandLineTools, buildTools, discs, patch11, patch12, fanPatch, destination, diskSpace
}

/// Grey (checking), green (ready), amber (handled or informational, not blocking), red (blocked).
public enum CheckState: Equatable {
    case checking(String)
    case ready(String)
    case automatic(String)
    case blocked(String)

    public var isReady: Bool {
        if case .ready = self { return true }
        return false
    }

    public var isBlocked: Bool {
        if case .blocked = self { return true }
        return false
    }

    public var text: String {
        switch self {
        case .checking(let t), .ready(let t), .automatic(let t), .blocked(let t): return t
        }
    }
}

/// Build is possible when every row is green or amber.
public func canBuild(_ states: [CheckID: CheckState]) -> Bool {
    CheckID.allCases.allSatisfy { id in
        switch states[id] {
        case .ready?, .automatic?: return true
        default: return false
        }
    }
}

public func macCheck(isAppleSilicon: Bool, version: OperatingSystemVersion) -> CheckState {
    guard isAppleSilicon else {
        return .blocked("Black & White 2 needs a Mac with Apple Silicon (M1 or later)")
    }
    guard version.majorVersion >= 13 else {
        return .blocked("Black & White 2 needs macOS 13 or newer")
    }
    return .ready("Apple Silicon, macOS \(version.majorVersion).\(version.minorVersion)")
}

/// True on an Apple Silicon Mac, also when the Builder itself runs under Rosetta.
public func isAppleSilicon() -> Bool {
    var value: Int32 = 0
    var size = MemoryLayout<Int32>.size
    return sysctlbyname("hw.optional.arm64", &value, &size, nil, 0) == 0 && value == 1
}

public func currentMacCheck() -> CheckState {
    macCheck(isAppleSilicon: isAppleSilicon(), version: ProcessInfo.processInfo.operatingSystemVersion)
}

/// The peak extra bytes a build from the four zips needs in the workspace: bw2/builder_cli.py REQUIRED_BYTES
/// (the largest fall of the volume's free space in plan 13 Task 6's end-to-end build, 10,604,712 KB, plus 20%;
/// not the workspace's `du` peak, which counts the APFS clones in full). tests/test_builder_swift.py compares the two.
public let requiredBuildBytes: Int64 = 13_100_000_000

/// Said under a red free-space row: statfs does not count purgeable space, and on a Mac with Time Machine most of
/// it is often local snapshots (plan 13 Task 8: 19.4 GB free by statfs, 220 GB "for important usage").
public let snapshotNote = ". Time Machine's local snapshots may be holding some of this disk's space."

public func diskSpaceCheck(available: Int64, required: Int64 = requiredBuildBytes, disk: String? = nil) -> CheckState {
    let gb = { (b: Int64) in String(format: "%.1f GB", Double(b) / 1_000_000_000) }
    if available >= required {
        return .ready("\(gb(available)) free")
    }
    let on = disk.map { " on \($0)" } ?? ""
    return .blocked("Only \(gb(available)) free\(on), \(gb(required)) needed" + snapshotNote)
}

/// The space free on the volume `url` is on (its nearest existing folder), or nil: statfs's blocks available to
/// the user, the figure the build's own check uses (bw2/builder_cli.py check_space, shutil.disk_usage) and `df`
/// shows. Not "available for important usage", which also counts purgeable space the build cannot count on: with
/// it the row was green at 220 GB while the build refused 19.4 GB (plan 13 Task 8).
public func availableBytes(at url: URL) -> Int64? {
    var path = url.standardizedFileURL
    while !FileManager.default.fileExists(atPath: path.path) && path.path != "/" { path.deleteLastPathComponent() }
    var info = statfs()
    guard statfs(path.path, &info) == 0 else { return nil }
    return Int64(info.f_bavail) * Int64(info.f_bsize)
}

/// The name Finder shows for the volume `url` is on.
public func volumeName(of url: URL) -> String? {
    var path = url.standardizedFileURL
    while !FileManager.default.fileExists(atPath: path.path) && path.path != "/" { path.deleteLastPathComponent() }
    return (try? path.resourceValues(forKeys: [.volumeLocalizedNameKey]))?.volumeLocalizedName
}

// MARK: - Inputs

/// bw2/builder_cli.py MESSAGES, copied: the texts a wrong or missing input gets, the same in the window and the
/// command line. tests/test_builder_swift.py compares the two tables.
public let inputMessageTemplates: [String: String] = [
    "missing": "missing: {what}",
    "duplicate": "duplicate: {what}",
    "unknown": "not a known file: {name}",
    "cue_bin": "{name}: the cue names {bin}, which is not there",
    "not_found": "not found: {name}",
    "space": "not enough free space on {volume}: the build needs {need} bytes and {free} are free",
]

/// A template of `inputMessageTemplates` with its `{field}`s filled in, as Python's str.format does.
public func inputMessage(_ key: String, _ fields: [String: String]) -> String {
    let template = inputMessageTemplates[key] ?? key
    var text = ""
    var rest = template[...]
    while let open = rest.firstIndex(of: "{"), let close = rest[open...].firstIndex(of: "}") {
        text += rest[..<open]
        let name = String(rest[rest.index(after: open)..<close])
        text += fields[name] ?? String(rest[open...close])
        rest = rest[rest.index(after: close)...]
    }
    return text + rest
}
