import Foundation

/// Only this version's workspace and downloaded inputs; tools are shared across versions.
private func buildFileRoots(paths: BuilderPaths, version: String, includingTools: Bool) throws -> [URL] {
    guard !version.isEmpty, !version.hasPrefix("."), !version.contains("/"),
          !["python", "venv", "inputs", "choices.json"].contains(version),
          !isSymbolicLink(paths.support) else { throw CocoaError(.fileWriteInvalidFileName) }
    return [paths.workspace(version: version), paths.inputs] + (includingTools ? [paths.python, paths.venv] : [])
}

private func isSymbolicLink(_ url: URL) -> Bool {
    var info = stat()
    return lstat(url.path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFLNK
}

/// Allocated file blocks, with each device/inode counted once and no symbolic links followed.
func allocatedSize(of roots: [URL]) -> Int64 {
    let fm = FileManager.default
    var seen = Set<String>()
    func size(_ url: URL) -> Int64 {
        var info = stat()
        guard lstat(url.path, &info) == 0 else { return 0 }
        switch info.st_mode & S_IFMT {
        case S_IFREG:
            guard seen.insert("\(info.st_dev):\(info.st_ino)").inserted else { return 0 }
            return Int64(info.st_blocks) * 512
        case S_IFDIR:
            return ((try? fm.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? [])
                .reduce(0) { $0 + size($1) }
        default: return 0
        }
    }
    return roots.reduce(0) { $0 + size($1) }
}

public func measureBuildFiles(paths: BuilderPaths, version: String) -> (temporary: Int64, tools: Int64) {
    guard let roots = try? buildFileRoots(paths: paths, version: version, includingTools: true) else { return (0, 0) }
    let temporary = allocatedSize(of: Array(roots.prefix(2)))
    // Deduplicate across both groups too, so selecting tools cannot double-count a hard link.
    return (temporary, allocatedSize(of: roots) - temporary)
}

private struct AppInsideBuildFiles: LocalizedError {
    var errorDescription: String? {
        "Move the game app outside the Builder's work folder before deleting temporary files."
    }
}

/// Run off the main thread. Refuse to delete an installed app's ancestor, including through linked destinations.
/// Rename everything aside first, then delete; interrupted deletions are swept on start.
@discardableResult
public func removeBuildFiles(paths: BuilderPaths, version: String, includingTools: Bool,
                             preservingApp app: URL? = nil) throws -> Int64 {
    let roots = try buildFileRoots(paths: paths, version: version, includingTools: includingTools)
    if let app = app?.resolvingSymlinksInPath().standardizedFileURL {
        guard !roots.contains(where: { app.pathComponents.starts(with: $0.resolvingSymlinksInPath().standardizedFileURL.pathComponents) })
        else { throw AppInsideBuildFiles() }
    }
    let bytes = allocatedSize(of: roots)
    var trash: [URL] = []
    do {
        for root in roots {
            var info = stat()
            if lstat(root.path, &info) == 0 {
                trash.append(try moveToTrash(root))
            } else if errno != ENOENT {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }
    } catch {
        for item in trash { try? FileManager.default.removeItem(at: item) }
        throw error
    }
    for item in trash { try FileManager.default.removeItem(at: item) }
    return bytes
}
