import Foundation

/// Removes com.apple.quarantine from `url` and everything below it, except below `keeping`
/// (left as it is, the folder included). Links are not followed.
public func removeQuarantine(at url: URL, keeping: URL? = nil) throws {
    let kept = keeping?.standardizedFileURL.path
    var urls = [url]
    if let walker = FileManager.default.enumerator(at: url, includingPropertiesForKeys: nil) {
        for case let child as URL in walker {
            if let kept = kept, child.standardizedFileURL.path == kept {
                walker.skipDescendants()
                continue
            }
            urls.append(child)
        }
    }
    for item in urls {
        if removexattr(item.path, "com.apple.quarantine", XATTR_NOFOLLOW) != 0, errno != ENOATTR {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }
}
