import Foundation
import XCTest
@testable import BuilderCore

/// A fresh folder under the temporary directory, with a space, quotes and an ampersand in its name,
/// as a support folder or a game path from Finder can have.
func makeTempFolder(_ label: String) throws -> URL {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("bw2 builder \(label) \"q\" & ö-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

@discardableResult
func writeFile(_ data: Data, to url: URL) throws -> URL {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try data.write(to: url)
    return url
}

@discardableResult
func writeText(_ text: String, to url: URL) throws -> URL {
    try writeFile(Data(text.utf8), to: url)
}

func readText(_ url: URL) -> String? { try? String(contentsOf: url, encoding: .utf8) }

/// Runs a program and waits; the output is stdout and stderr together.
@discardableResult
func runTool(_ executable: String, _ arguments: [String], in directory: URL? = nil) throws -> (status: Int32, output: String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    if let directory = directory { process.currentDirectoryURL = directory }
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    try process.run()
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return (process.terminationStatus, String(decoding: data, as: UTF8.self))
}

/// The CRC-32 zip uses (reflected polynomial 0xEDB88320), written here so the test does not trust the code under test.
func referenceCRC32(_ data: Data) -> UInt32 {
    var crc: UInt32 = 0xFFFF_FFFF
    for byte in data {
        crc ^= UInt32(byte)
        for _ in 0..<8 { crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB8_8320 : crc >> 1 }
    }
    return ~crc
}

func sha256Text(_ data: Data) -> String {
    sha256Hex(data)
}
