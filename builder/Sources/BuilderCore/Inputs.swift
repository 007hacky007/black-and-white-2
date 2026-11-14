import Foundation

/// bw2/manifests/disc-zips.tsv: per disc, the bin's name, size and CRC-32 as each zip's central directory gives
/// them (plan 13 "Measured" item 8).
public struct DiscZipTable {
    public struct Row: Equatable {
        public let disc: Int
        public let bin: String
        public let size: Int64
        public let crc32: UInt32
    }

    public let rows: [Row]

    public init(text: String) throws {
        var rows: [Row] = []
        for (number, line) in text.components(separatedBy: "\n").enumerated()
        where !line.trimmingCharacters(in: .whitespaces).isEmpty && !line.hasPrefix("#") {
            let f = line.components(separatedBy: "\t")
            guard f.count == 4, let disc = Int(f[0]), let size = Int64(f[2]), let crc = UInt32(f[3], radix: 16) else {
                throw TableError(message: "disc-zips.tsv line \(number + 1) is damaged")
            }
            rows.append(Row(disc: disc, bin: f[1], size: size, crc32: crc))
        }
        self.rows = rows
    }

    public static func load(_ tsv: URL) throws -> DiscZipTable {
        try DiscZipTable(text: String(contentsOf: tsv, encoding: .utf8))
    }

    /// The disc a zip holds, from its central directory alone: exactly one .bin and one .cue, the bin known by
    /// name, size and CRC-32 (bw2/builder_cli.py `_zip_disc`). The bin's SHA-256 is checked after unpacking, in
    /// the build's discs step. nil for an unknown, damaged or truncated zip.
    public func disc(ofZip url: URL) -> Int? {
        guard let entries = try? ZipDirectory.entries(url) else { return nil }
        let files = entries.filter { !$0.isDirectory }
        let bins = files.filter { $0.name.lowercased().hasSuffix(".bin") }
        let cues = files.filter { $0.name.lowercased().hasSuffix(".cue") }
        guard bins.count == 1, cues.count == 1, let bin = bins.first else { return nil }
        return rows.first { $0.bin == bin.fileName && $0.size == bin.size && $0.crc32 == bin.crc32 }?.disc
    }
}

/// The input kinds the build takes, by bw2/known_releases.tsv kind: what each is called in messages.
public let patchKinds: [(kind: String, what: String)] = [
    ("patch-1.1", "patch 1.1"), ("patch-1.2", "patch 1.2"), ("fanpatch-installer", "fan patch 1.42"),
]

let discExtensions: Set<String> = ["zip", "cue", "bin", "iso"]

/// The bin a cue names, beside the cue (bw2/disc.py `_parse_cue`): exactly one FILE and one TRACK, MODE1/2352.
/// nil when the cue cannot be read or is not of that form.
func cueBin(_ cue: URL) -> URL? {
    guard let data = try? Data(contentsOf: cue) else { return nil }
    let text = String(decoding: data, as: UTF8.self)
    func groups(_ pattern: String) -> [String] {
        let regex = try! NSRegularExpression(pattern: pattern, options: [.anchorsMatchLines])
        let range = NSRange(text.startIndex..., in: text)
        return regex.matches(in: text, range: range).compactMap { Range($0.range(at: 1), in: text).map { String(text[$0]) } }
    }
    let files = groups(#"^\s*FILE\s+"([^"]+)""#)
    let tracks = groups(#"^\s*TRACK\s+\d+\s+(\S+)"#)
    guard files.count == 1, tracks.count == 1, tracks[0].uppercased() == "MODE1/2352" else { return nil }
    return cue.deletingLastPathComponent().appendingPathComponent(files[0])
}

/// The disc number of a known disc row: "... (Disc 3)" at the end of its name.
func discNumber(_ file: KnownFile?) -> Int? {
    guard let file = file, file.kind == "disc", file.name.hasSuffix(")"),
          let open = file.name.range(of: "(Disc ", options: .backwards) else { return nil }
    return Int(file.name[open.upperBound..<file.name.index(before: file.name.endIndex)])
}

/// The real path of a file (links resolved), for counting the same file once.
func realPath(_ url: URL) -> String {
    var buffer = [CChar](repeating: 0, count: Int(PATH_MAX))
    return realpath(url.path, &buffer) != nil ? String(cString: buffer) : url.standardizedFileURL.path
}

func isDirectory(_ url: URL) -> Bool {
    var isDir: ObjCBool = false
    return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue
}

/// The disc row: which of discs 1 to 4 the chosen files are (bw2/builder_cli.py `classify`, the discs part).
/// A folder stands for the zips, cues, bins and ISOs directly in it (not hidden, not deeper); a file given
/// directly is looked at whatever its name. Zips are known by `zips` (their central directory), bins and ISOs
/// by `known` (size, then SHA-256), a cue through its bin; a bin named by a chosen cue is part of that disc.
/// Every problem is named at once, in `classify`'s order and words. `discs` holds each disc found once, by the
/// file the build is given (the zip, the cue, the bin or the ISO).
public func discsCheck(_ urls: [URL], zips: DiscZipTable, known: KnownFiles) -> (CheckState, discs: [Int: URL]) {
    if urls.isEmpty {
        return (.blocked("Choose the four discs: their zips, .bin and .cue, or .iso files, or a folder"), [:])
    }
    let fm = FileManager.default
    var problems: [String] = []
    var files: [URL] = []
    var seen = Set<String>()
    for url in urls {
        var found: [URL]
        if isDirectory(url) {
            let names = ((try? fm.contentsOfDirectory(atPath: url.path)) ?? []).sorted()
            found = names.filter { !$0.hasPrefix(".") }.map { url.appendingPathComponent($0) }
                .filter { discExtensions.contains($0.pathExtension.lowercased()) && fileSize($0) != nil }
        } else if fileSize(url) != nil {
            found = [url]
        } else {
            problems.append(inputMessage("not_found", ["name": url.path]))
            continue
        }
        for file in found where seen.insert(realPath(file)).inserted {
            files.append(file)
        }
    }

    var inCue = Set<String>()
    for file in files where file.pathExtension.lowercased() == "cue" {
        if let bin = cueBin(file) { inCue.insert(realPath(bin)) }
    }
    let discRows = KnownFiles(rows: known.files.filter { $0.kind == "disc" })
    var discs: [Int: [URL]] = [:]
    var fileProblems: [String] = []
    for file in files {
        var number: Int?
        switch file.pathExtension.lowercased() {
        case "zip":
            number = zips.disc(ofZip: file)
        case "cue":
            let bin = cueBin(file)
            if let bin = bin, fileSize(bin) == nil {
                fileProblems.append(inputMessage("cue_bin", ["name": file.lastPathComponent, "bin": bin.lastPathComponent]))
                continue
            }
            number = bin.flatMap { discNumber(discRows.identify($0, kind: "disc")) }
        case "bin" where inCue.contains(realPath(file)):
            continue
        case "bin", "iso":
            number = discNumber(discRows.identify(file, kind: "disc"))
        default:
            number = nil
        }
        if let number = number {
            discs[number, default: []].append(file)
        } else {
            fileProblems.append(inputMessage("unknown", ["name": file.lastPathComponent]))
        }
    }
    for n in 1...4 {
        if discs[n] == nil {
            problems.append(inputMessage("missing", ["what": "disc \(n)"]))
        } else if discs[n]!.count > 1 {
            problems.append(inputMessage("duplicate", ["what": "disc \(n)"]))
        }
    }
    problems += fileProblems
    let found = discs.compactMapValues { $0.first }
    return (problems.isEmpty ? .ready("Discs 1-4 found") : .blocked(problems.joined(separator: "; ")), found)
}

/// A patch row: ready with the release's name when `url` is that patch (size, then SHA-256). Otherwise red
/// with what it is instead, or that nothing is chosen yet.
public func patchCheck(_ url: URL?, kind: String, known: KnownFiles) -> CheckState {
    guard let url = url else { return .blocked("Download it, or choose the file you have") }
    let what = patchKinds.first { $0.kind == kind }?.what ?? kind
    guard fileSize(url) != nil else { return .blocked(inputMessage("not_found", ["name": url.path])) }
    let candidates = KnownFiles(rows: known.files.filter { row in patchKinds.contains { $0.kind == row.kind } })
    guard let file = candidates.identify(url, kind: nil) else {
        return .blocked("Not \(what): \(url.lastPathComponent)")
    }
    if file.kind == kind { return .ready(file.name) }
    let other = patchKinds.first { $0.kind == file.kind }?.what ?? file.kind
    return .blocked("Not \(what): \(url.lastPathComponent) is \(other)")
}

extension KnownFiles {
    init(rows: [KnownFile]) {
        self.files = rows
    }
}
