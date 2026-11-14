import Foundation

/// One member of a zip as its central directory names it.
public struct ZipEntry: Equatable {
    /// The name as stored ("/" between folders).
    public let name: String
    /// Uncompressed size.
    public let size: Int64
    public let crc32: UInt32

    public init(name: String, size: Int64, crc32: UInt32) {
        (self.name, self.size, self.crc32) = (name, size, crc32)
    }

    public var isDirectory: Bool { name.hasSuffix("/") }
    /// The last part of the name, as Python's Path(name).name gives it.
    public var fileName: String { name.split(separator: "/").last.map(String.init) ?? "" }
}

public struct ZipError: LocalizedError, Equatable {
    public let message: String
    public var errorDescription: String? { message }
}

/// The member list of a zip (names, sizes, CRC-32s), read from its central directory without unpacking
/// anything, so a 780 MB disc zip is known in milliseconds (plan 13 Ruling 9). Zip64 records are read.
public enum ZipDirectory {
    public static func entries(_ url: URL) throws -> [ZipEntry] {
        let handle: FileHandle
        do { handle = try FileHandle(forReadingFrom: url) } catch {
            throw ZipError(message: "\(url.lastPathComponent) cannot be read")
        }
        defer { try? handle.close() }
        guard let size = fileSize(url) else { throw ZipError(message: "\(url.lastPathComponent) is not a file") }
        return try entries(size: size) { offset, length in
            try handle.seek(toOffset: UInt64(offset))
            return try handle.read(upToCount: length) ?? Data()
        } name: { url.lastPathComponent }
    }

    /// `read(offset, length)` returns bytes of the zip file of `size` bytes.
    static func entries(size: Int64, read: (Int64, Int) throws -> Data, name: () -> String) throws -> [ZipEntry] {
        let damaged = ZipError(message: "\(name()) is not a complete zip")
        func bytes(_ offset: Int64, _ length: Int) throws -> [UInt8] {
            guard offset >= 0, length >= 0, offset + Int64(length) <= size else { throw damaged }
            let b = [UInt8](try read(offset, length))
            guard b.count == length else { throw damaged }
            return b
        }
        func u16(_ b: [UInt8], _ p: Int) -> UInt64 { UInt64(b[p]) | UInt64(b[p + 1]) << 8 }
        func u32(_ b: [UInt8], _ p: Int) -> UInt64 { u16(b, p) | u16(b, p + 2) << 16 }
        func u64(_ b: [UInt8], _ p: Int) -> UInt64 { u32(b, p) | u32(b, p + 4) << 32 }

        // The end of central directory record: 22 bytes plus a comment of up to 65,535.
        guard size >= 22 else { throw damaged }
        let tailLength = Int(min(size, 22 + 65_535))
        let tailStart = size - Int64(tailLength)
        let tail = try bytes(tailStart, tailLength)
        var eocd = -1
        var i = tail.count - 22
        while i >= 0 {
            if tail[i] == 0x50, tail[i + 1] == 0x4B, tail[i + 2] == 0x05, tail[i + 3] == 0x06,
               i + 22 + Int(u16(tail, i + 20)) <= tail.count {
                eocd = i
                break
            }
            i -= 1
        }
        guard eocd >= 0 else { throw damaged }
        var count = u16(tail, eocd + 10)
        var dirSize = u32(tail, eocd + 12)
        var dirOffset = u32(tail, eocd + 16)
        if count == 0xFFFF || dirSize == 0xFFFF_FFFF || dirOffset == 0xFFFF_FFFF {
            // The zip64 end record, found through its locator just before the end record.
            let locatorAt = tailStart + Int64(eocd) - 20
            let locator = try bytes(locatorAt, 20)
            guard u32(locator, 0) == 0x0706_4B50 else { throw damaged }
            let end64 = try bytes(Int64(u64(locator, 8)), 56)
            guard u32(end64, 0) == 0x0606_4B50 else { throw damaged }
            count = u64(end64, 32)
            dirSize = u64(end64, 40)
            dirOffset = u64(end64, 48)
        }
        guard dirSize <= UInt64(Int.max), dirOffset <= UInt64(Int64.max) else { throw damaged }
        let dir = try bytes(Int64(dirOffset), Int(dirSize))
        var result: [ZipEntry] = []
        var p = 0
        var n: UInt64 = 0
        while n < count {
            guard p + 46 <= dir.count, u32(dir, p) == 0x0201_4B50 else { throw damaged }
            let crc = UInt32(u32(dir, p + 16))
            let compressed = u32(dir, p + 20)
            var length = u32(dir, p + 24)
            let nameLength = Int(u16(dir, p + 28)), extraLength = Int(u16(dir, p + 30)), comment = Int(u16(dir, p + 32))
            guard p + 46 + nameLength + extraLength + comment <= dir.count else { throw damaged }
            let raw = Data(dir[(p + 46)..<(p + 46 + nameLength)])
            let name = String(data: raw, encoding: .utf8) ?? String(data: raw, encoding: .isoLatin1) ?? ""
            // Zip64 extra field (id 1): the 64-bit sizes, in this order, of the fields that read 0xFFFFFFFF.
            var e = p + 46 + nameLength
            let extraEnd = e + extraLength
            while e + 4 <= extraEnd {
                let id = u16(dir, e), fieldLength = Int(u16(dir, e + 2))
                guard e + 4 + fieldLength <= extraEnd else { throw damaged }
                if id == 1 {
                    var q = e + 4
                    if length == 0xFFFF_FFFF {
                        guard q + 8 <= e + 4 + fieldLength else { throw damaged }
                        length = u64(dir, q); q += 8
                    }
                    if compressed == 0xFFFF_FFFF { q += 8 }
                    guard q <= e + 4 + fieldLength else { throw damaged }
                }
                e += 4 + fieldLength
            }
            guard length <= UInt64(Int64.max) else { throw damaged }
            result.append(ZipEntry(name: name, size: Int64(length), crc32: crc))
            p += 46 + nameLength + extraLength + comment
            n += 1
        }
        return result
    }
}
