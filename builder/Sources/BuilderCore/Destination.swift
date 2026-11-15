import AppKit
import CoreServices
import Foundation

/// bw2/app.py BUNDLE_ID and APP_NAME: the built game's bundle identifier and folder name.
public let gameBundleID = "local.bw2-port.game"
public let gameAppName = "Black & White 2.app"
/// The Info.plist key bw2/app.py writes the Builder's version under.
public let builderVersionKey = "BW2BuilderVersion"

public struct InstallError: LocalizedError {
    public let message: String
    /// Underlying error text, for the log.
    public var details: String? = nil
    public var errorDescription: String? { message }
}

/// `/Applications`, or `~/Applications` when `/Applications` is not writable (plan 13 Ruling 11).
public var defaultDestination: URL {
    chooseDestination(home: FileManager.default.homeDirectoryForCurrentUser,
                       isWritable: { FileManager.default.isWritableFile(atPath: $0) })
}

func chooseDestination(home: URL, isWritable: (String) -> Bool) -> URL {
    isWritable("/Applications") ? URL(fileURLWithPath: "/Applications") : home.appendingPathComponent("Applications")
}

/// Green for a folder the app can be put in: a writable folder, or a missing one inside a writable folder
/// (installApp makes it, as ~/Applications may not exist yet).
public func destinationCheck(_ directory: URL) -> CheckState {
    let fm = FileManager.default
    let ok = isDirectory(directory) ? fm.isWritableFile(atPath: directory.path)
        : !fm.fileExists(atPath: directory.path) && isDirectory(directory.deletingLastPathComponent())
            && fm.isWritableFile(atPath: directory.deletingLastPathComponent().path)
    return ok ? .ready("\(gameAppName) goes to \(abbreviated(directory))") : .blocked("Choose a folder you can write to")
}

/// A Black & White 2.app that is already there: the one an install replaces.
public struct InstalledApp: Equatable {
    public let url: URL
    /// The Builder version that made it (BW2BuilderVersion).
    public let builderVersion: String?
    /// Its CFBundleShortVersionString.
    public let appVersion: String?

    public init?(_ url: URL) {
        let plist = url.appendingPathComponent("Contents/Info.plist")
        guard FileManager.default.fileExists(atPath: plist.path) else { return nil }
        let info = NSDictionary(contentsOf: plist)
        self.url = url
        builderVersion = (info?[builderVersionKey] as? String).flatMap { $0.isEmpty ? nil : $0 }
        appVersion = (info?["CFBundleShortVersionString"] as? String).flatMap { $0.isEmpty ? nil : $0 }
    }

    /// The Black & White 2.app in `directory`, if there is one.
    public static func find(in directory: URL) -> InstalledApp? {
        InstalledApp(directory.appendingPathComponent(gameAppName))
    }
}

/// True while an app with this bundle identifier runs.
public func isAppRunning(_ bundleID: String) -> Bool {
    !NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).isEmpty
}

/// The volume a path is on (that of its nearest existing folder), for comparing two paths.
public func volumeID(_ url: URL) -> UInt64? {
    var path = url.standardizedFileURL
    while !FileManager.default.fileExists(atPath: path.path) && path.path != "/" { path.deleteLastPathComponent() }
    var info = stat()
    guard stat(path.path, &info) == 0 else { return nil }
    return UInt64(UInt32(bitPattern: info.st_dev))
}

/// Recover an interrupted replacement before discarding old game apps. Unrelated hidden files stay put.
public func sweepInstallLeftovers(in directory: URL) {
    let fm = FileManager.default
    func kind(_ url: URL) -> mode_t? {
        var info = stat()
        return lstat(url.path, &info) == 0 ? info.st_mode & S_IFMT : nil
    }
    func knownApp(_ url: URL) -> Bool {
        let contents = url.appendingPathComponent("Contents")
        let plist = contents.appendingPathComponent("Info.plist")
        guard kind(url) == S_IFDIR, kind(contents) == S_IFDIR, kind(plist) == S_IFREG else { return false }
        return NSDictionary(contentsOf: plist)?["CFBundleIdentifier"] as? String == gameBundleID
    }
    func owned(_ name: String, prefix: String) -> Bool {
        name.hasPrefix(prefix) && UUID(uuidString: String(name.dropFirst(prefix.count))) != nil
    }
    guard kind(directory) == S_IFDIR else { return }
    let names = ((try? fm.contentsOfDirectory(atPath: directory.path)) ?? []).sorted()
    let target = directory.appendingPathComponent(gameAppName)
    for name in names where owned(name, prefix: ".trash-") {
        let old = directory.appendingPathComponent(name)
        guard knownApp(old) else { continue }
        if kind(target) == nil {
            try? fm.moveItem(at: old, to: target)
        } else if knownApp(target) {
            try? fm.removeItem(at: old)
        }
    }
    for name in names where owned(name, prefix: ".bw2-install-") {
        try? fm.removeItem(at: directory.appendingPathComponent(name))
    }
}

/// Puts `built` (the app the build made in the workspace's out/) into `into` as Black & White 2.app and returns
/// where it is. Refused while the game runs. The new app is made complete next to the final place first, as
/// `.bw2-install-<uuid>/Black & White 2.app` (moved on the same volume; copied otherwise), and loses quarantine; then the
/// old app is renamed to a hidden `.trash-<uuid>`, the new one is moved in and the old one deleted. When moving
/// the new one in fails, the old one is put back; when anything fails, a moved build goes back to `built` for
/// another try. Nothing beside the app and our interrupted install leftovers (the saves live in ~/Library/Application Support/Black & White 2) is
/// touched.
public func installApp(built: URL, into directory: URL, isRunning: (String) -> Bool) throws -> URL {
    try installApp(built: built, into: directory, isRunning: isRunning,
                   move: { try FileManager.default.moveItem(at: $0, to: $1) })
}

func installApp(built: URL, into directory: URL, isRunning: (String) -> Bool,
                move: (URL, URL) throws -> Void) throws -> URL {
    if isRunning(gameBundleID) { throw InstallError(message: "Quit Black & White 2 first") }
    let fm = FileManager.default
    guard fm.fileExists(atPath: built.appendingPathComponent("Contents/Info.plist").path) else {
        throw InstallError(message: "The built app is incomplete: \(built.path) has no Contents/Info.plist")
    }
    let target = directory.appendingPathComponent(gameAppName)
    let stagingDirectory = directory.appendingPathComponent(".bw2-install-\(UUID().uuidString)")
    let staging = stagingDirectory.appendingPathComponent(gameAppName)
    var moved = false
    do {
        if !fm.fileExists(atPath: directory.path) {
            try fm.createDirectory(at: directory, withIntermediateDirectories: false)
        }
        sweepInstallLeftovers(in: directory)
        try fm.createDirectory(at: stagingDirectory, withIntermediateDirectories: false)
        if let a = volumeID(built), a == volumeID(directory) {
            try move(built, staging)
            moved = true
        } else {
            try fm.copyItem(at: built, to: staging)
        }
        try removeQuarantine(at: staging)
        var trash: URL?
        if fm.fileExists(atPath: target.path) || (try? fm.destinationOfSymbolicLink(atPath: target.path)) != nil {
            let aside = directory.appendingPathComponent(".trash-\(UUID().uuidString)")
            try move(target, aside)
            trash = aside
        }
        do {
            try move(staging, target)
        } catch {
            if let trash = trash { try? move(trash, target) }
            throw error
        }
        try? fm.removeItem(at: stagingDirectory)
        _ = LSRegisterURL(target as CFURL, true)
        try? fm.setAttributes([.modificationDate: Date()], ofItemAtPath: target.path)
        if let trash = trash { try? fm.removeItem(at: trash) }
        return target
    } catch {
        if moved, fm.fileExists(atPath: staging.path), !fm.fileExists(atPath: built.path) {
            try? move(staging, built)
        }
        // Keep the staged app for recovery if restoring a moved build also failed.
        if !moved || !fm.fileExists(atPath: staging.path) { try? fm.removeItem(at: stagingDirectory) }
        throw InstallError(
            message: "Could not install Black & White 2 into \(directory.lastPathComponent). Choose another folder and try again.",
            details: String(describing: error))
    }
}

/// A path with the home folder written as ~.
public func abbreviated(_ url: URL) -> String {
    (url.path as NSString).abbreviatingWithTildeInPath
}
