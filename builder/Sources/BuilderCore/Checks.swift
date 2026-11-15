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

/// `reclaimable` is what an earlier build left in the workspace (reclaimableBuildBytes), which `build` removes
/// before its own space check, so it counts as free.
public func diskSpaceCheck(available: Int64, reclaimable: Int64 = 0, required: Int64 = requiredBuildBytes,
                           disk: String? = nil) -> CheckState {
    let gb = { (b: Int64) in String(format: "%.1f GB", Double(b) / 1_000_000_000) }
    let earlier = "an earlier build's files"
    if available + reclaimable >= required {
        let plus = reclaimable > 0 ? ", and \(gb(reclaimable)) of \(earlier) that the build removes first" : ""
        return .ready("\(gb(available)) free\(plus)")
    }
    let on = disk.map { " on \($0)" } ?? ""
    let with = reclaimable > 0 ? " (\(gb(available + reclaimable)) with \(earlier))" : ""
    return .blocked("Only \(gb(available)) free\(on)\(with), \(gb(required)) needed" + snapshotNote)
}

/// The bytes bw2/builder_cli.py clean would give back in a workspace (a folder with SOURCES.txt): gamefiles/, gen/
/// and out/ but out/tools/, which `build` removes before its space check. Only a regular file whose only link is
/// this one counts (links are never followed; a hard link's blocks stay with its other name), by allocated size;
/// gamefiles/game and an app in out/ are left out, since they are APFS clones of gamefiles/install and of the game
/// and their shared blocks would be counted twice (what they hold alone, the patched files and the engine, is
/// left uncounted: the row may say too little, never too much).
public func reclaimableBuildBytes(workspace ws: URL) -> Int64 {
    let fm = FileManager.default
    guard fm.fileExists(atPath: ws.appendingPathComponent("SOURCES.txt").path) else { return 0 }
    let keys: [URLResourceKey] = [.isRegularFileKey, .isSymbolicLinkKey, .linkCountKey, .totalFileAllocatedSizeKey]
    func size(_ url: URL) -> Int64 {
        guard let v = try? url.resourceValues(forKeys: Set(keys)), v.isSymbolicLink != true else { return 0 }
        if v.isRegularFile == true { return v.linkCount == 1 ? Int64(v.totalFileAllocatedSize ?? 0) : 0 }
        guard let walk = fm.enumerator(at: url, includingPropertiesForKeys: keys, options: [],
                                       errorHandler: { _, _ in true }) else { return 0 }
        var total: Int64 = 0
        for case let f as URL in walk {
            guard let v = try? f.resourceValues(forKeys: Set(keys)), v.isRegularFile == true, v.isSymbolicLink != true,
                  v.linkCount == 1 else { continue }
            total += Int64(v.totalFileAllocatedSize ?? 0)
        }
        return total
    }
    func children(_ dir: String, except skip: (String) -> Bool) -> [URL] {
        let url = ws.appendingPathComponent(dir)
        guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]))?.isSymbolicLink != true else { return [] }
        return ((try? fm.contentsOfDirectory(at: url, includingPropertiesForKeys: keys)) ?? [])
            .filter { !skip($0.lastPathComponent) }
    }
    let clones = { (name: String) in name == "game" || name == "game.part" }
    let apps = { (name: String) in name == "tools" || name.hasSuffix(".app") || name.hasSuffix(".app.part") }
    let items = children("gamefiles", except: clones) + children("gen", except: { _ in false })
        + children("out", except: apps)
    return items.reduce(0) { $0 + size($1) }
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
