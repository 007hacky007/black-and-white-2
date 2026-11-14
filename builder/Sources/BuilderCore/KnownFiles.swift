import CryptoKit
import Foundation

public struct TableError: LocalizedError, Equatable {
    public let message: String
    public var errorDescription: String? { message }
}

/// One row of bw2/known_releases.tsv: a file the build knows by size and SHA-256.
public struct KnownFile: Equatable {
    public let kind: String
    public let name: String
    public let size: Int64
    public let sha256: String
    public let note: String
}

/// bw2/known_releases.tsv (bw2/releases.py `load_releases`): kind, name, size, SHA-256, note; `#` lines are comments.
public struct KnownFiles {
    public let files: [KnownFile]

    public init(text: String) throws {
        var files: [KnownFile] = []
        for (number, line) in text.components(separatedBy: "\n").enumerated()
        where !line.trimmingCharacters(in: .whitespaces).isEmpty && !line.hasPrefix("#") {
            let f = line.components(separatedBy: "\t")
            guard f.count >= 4, let size = Int64(f[2]), size >= 0, isSHA256(f[3]) else {
                throw TableError(message: "known_releases.tsv line \(number + 1) is damaged")
            }
            files.append(KnownFile(kind: f[0], name: f[1], size: size, sha256: f[3].lowercased(),
                                   note: f.count > 4 ? f[4] : ""))
        }
        self.files = files
    }

    public static func load(_ tsv: URL) throws -> KnownFiles {
        try KnownFiles(text: String(contentsOf: tsv, encoding: .utf8))
    }

    /// The known file `url` is (of `kind`, or of any kind), or nil: the size first, then a streamed SHA-256,
    /// computed only when a row has that size.
    public func identify(_ url: URL, kind: String?) -> KnownFile? {
        guard let size = fileSize(url) else { return nil }
        let candidates = files.filter { $0.size == size && (kind == nil || $0.kind == kind) }
        guard !candidates.isEmpty, let digest = try? sha256Hex(of: url) else { return nil }
        return candidates.first { $0.sha256 == digest }
    }
}

func isSHA256(_ text: String) -> Bool {
    text.count == 64 && text.allSatisfy { $0.isHexDigit }
}

/// The size of the regular file at `url` (links followed), or nil.
func fileSize(_ url: URL) -> Int64? {
    var info = stat()
    guard stat(url.path, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG else { return nil }
    return Int64(info.st_size)
}

/// The SHA-256 of a file, read in 4 MB pieces, as lower-case hex.
public func sha256Hex(of url: URL) throws -> String {
    let handle = try FileHandle(forReadingFrom: url)
    defer { try? handle.close() }
    var hasher = SHA256()
    while true {
        let chunk = try autoreleasepool { try handle.read(upToCount: 4 << 20) ?? Data() }
        if chunk.isEmpty { break }
        hasher.update(data: chunk)
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}

public func sha256Hex(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}
