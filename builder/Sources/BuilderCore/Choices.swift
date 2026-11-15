import Foundation

/// What was chosen in the window: the disc files or folders, the three patch files and the destination. Kept in
/// `<support>/choices.json`, so `--support DIR` redirects it with everything else (plan 13 Task 8).
public struct BuilderChoices: Codable, Equatable {
    public var discs: [String]
    public var patch11: String?
    public var patch12: String?
    public var fanPatch: String?
    public var destination: String?

    public init(discs: [String] = [], patch11: String? = nil, patch12: String? = nil, fanPatch: String? = nil,
                destination: String? = nil) {
        (self.discs, self.patch11, self.patch12, self.fanPatch, self.destination) =
            (discs, patch11, patch12, fanPatch, destination)
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        discs = (try? c.decodeIfPresent([String].self, forKey: .discs)) ?? []
        patch11 = try? c.decodeIfPresent(String.self, forKey: .patch11)
        patch12 = try? c.decodeIfPresent(String.self, forKey: .patch12)
        fanPatch = try? c.decodeIfPresent(String.self, forKey: .fanPatch)
        destination = try? c.decodeIfPresent(String.self, forKey: .destination)
    }

    /// The saved choices; nothing chosen when the file is missing or cannot be read.
    public static func load(_ paths: BuilderPaths) -> BuilderChoices {
        guard let data = try? Data(contentsOf: paths.choicesFile),
              let choices = try? JSONDecoder().decode(BuilderChoices.self, from: data) else { return BuilderChoices() }
        return choices
    }

    /// Writes the choices atomically, making the support folder when needed.
    public func save(_ paths: BuilderPaths) throws {
        try FileManager.default.createDirectory(at: paths.support, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: paths.choicesFile, options: .atomic)
    }

    public var discURLs: [URL] { discs.map { URL(fileURLWithPath: $0) } }

    public var destinationURL: URL { destination.map { URL(fileURLWithPath: $0) } ?? defaultDestination }

    /// The chosen file of a patch row (patch11, patch12, fanPatch), or nil.
    public func patch(_ id: CheckID) -> URL? {
        let path: String?
        switch id {
        case .patch11: path = patch11
        case .patch12: path = patch12
        case .fanPatch: path = fanPatch
        default: path = nil
        }
        return path.map { URL(fileURLWithPath: $0) }
    }

    public mutating func setPatch(_ id: CheckID, _ url: URL?) {
        switch id {
        case .patch11: patch11 = url?.path
        case .patch12: patch12 = url?.path
        case .fanPatch: fanPatch = url?.path
        default: break
        }
    }
}

extension BuilderPaths {
    public var choicesFile: URL { support.appendingPathComponent("choices.json") }
}

extension CheckID {
    /// The patch rows, in the window's order.
    public static let patchRows: [CheckID] = [.patch11, .patch12, .fanPatch]

    /// The bw2/known_releases.tsv kind of a patch row (also `builder_cli download --kind`), or nil.
    public var patchKind: String? {
        switch self {
        case .patch11: return "patch-1.1"
        case .patch12: return "patch-1.2"
        case .fanPatch: return "fanpatch-installer"
        default: return nil
        }
    }
}

extension CheckState {
    /// `checking`, `ready`, `automatic` or `blocked`, as `--print-checks` prints it.
    public var name: String {
        switch self {
        case .checking: return "checking"
        case .ready: return "ready"
        case .automatic: return "automatic"
        case .blocked: return "blocked"
        }
    }
}

/// The two tables the input rows check against, read from the Builder's bundled sources (the same files the
/// build's `classify` reads): bw2/known_releases.tsv and bw2/manifests/disc-zips.tsv.
public struct InputTables {
    public let known: KnownFiles
    public let zips: DiscZipTable

    public static func load(sources: URL) throws -> InputTables {
        InputTables(known: try KnownFiles.load(sources.appendingPathComponent("bw2/known_releases.tsv")),
                    zips: try DiscZipTable.load(sources.appendingPathComponent("bw2/manifests/disc-zips.tsv")))
    }
}

/// What a row says when the Builder's bundled sources are missing or damaged.
public let incompleteBuilderText = "This copy of BW2 Builder is incomplete. Make it again."

/// The free-space row: the workspace's volume against `requiredBuildBytes`, counting what an earlier build left in
/// this version's workspace as free (`build` removes it first).
public func diskSpaceState(paths: BuilderPaths, version: String, required: Int64 = requiredBuildBytes) -> CheckState {
    guard let available = availableBytes(at: paths.support) else {
        return .blocked("Could not read the free space on this disk")
    }
    return diskSpaceCheck(available: available, reclaimable: reclaimableBuildBytes(workspace: paths.workspace(version: version)),
                          required: required, disk: volumeName(of: paths.support))
}

/// Every row once, in order, synchronously: what `--print-checks` prints. The window runs the same checks, the
/// slow ones (hashing the discs and patches) in the background. `sources` is the Builder's Resources/source, nil
/// when it is missing.
public func evaluateChecks(paths: BuilderPaths, version: String, sources: URL?, choices: BuilderChoices,
                           commandLineTools: CommandLineTools = CommandLineTools()) -> [CheckID: CheckState] {
    var states: [CheckID: CheckState] = [:]
    states[.mac] = currentMacCheck()
    states[.commandLineTools] = commandLineTools.state()
    let tables = sources.flatMap { try? InputTables.load(sources: $0) }
    if let tables = tables {
        states[.buildTools] = buildToolsCheck(paths: paths, version: version)
        states[.discs] = discsCheck(choices.discURLs, zips: tables.zips, known: tables.known).0
        for id in CheckID.patchRows {
            states[id] = patchCheck(choices.patch(id), kind: id.patchKind!, known: tables.known)
        }
    } else {
        for id in [CheckID.buildTools, .discs] + CheckID.patchRows { states[id] = .blocked(incompleteBuilderText) }
    }
    states[.destination] = destinationCheck(choices.destinationURL)
    states[.diskSpace] = diskSpaceState(paths: paths, version: version)
    return states
}

/// One JSON object per row, in the window's order: `{"id": ..., "state": ..., "text": ...}`.
public func checkLines(_ states: [CheckID: CheckState]) -> [String] {
    CheckID.allCases.map { id in
        let state = states[id] ?? .checking("Not checked")
        let object: [String: String] = ["id": id.rawValue, "state": state.name, "text": state.text]
        let data = try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
        return String(decoding: data, as: UTF8.self)
    }
}

/// The files `builder_cli build --input` gets: each disc once (the file the disc row found it by), then the three
/// patches.
public func buildInputs(discs: [Int: URL], choices: BuilderChoices) -> [URL] {
    discs.keys.sorted().compactMap { discs[$0] } + CheckID.patchRows.compactMap { choices.patch($0) }
}
