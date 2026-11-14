import Foundation

/// One output line of `python -m bw2.builder_cli` (bw2/builder_cli.py `emit`).
public enum PipelineEvent: Equatable {
    /// A step starts: its id (one of `buildSteps`) and the title the window shows.
    case step(id: String, title: String)
    /// How far the current step is (0...1).
    case progress(Double)
    /// The command's result: the built app (build), the venv (setup), the downloaded file (download)
    /// or the workspace (clean).
    case done(URL)
    /// The command failed; it exits 1 after this.
    case error(String)
    /// Any other line: plain log text.
    case log(String)
}

public let eventPrefix = "BW2-EVENT "

/// `BW2-EVENT <json>` as one of the four shapes, or the line as log text.
public func parsePipelineLine(_ line: String) -> PipelineEvent {
    guard line.hasPrefix(eventPrefix),
          let data = String(line.dropFirst(eventPrefix.count)).data(using: .utf8),
          let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    else { return .log(line) }

    if let id = obj["step"] as? String, let title = obj["title"] as? String {
        return .step(id: id, title: title)
    }
    if let number = obj["progress"] as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() {
        return .progress(number.doubleValue)
    }
    if let path = obj["done"] as? String { return .done(URL(fileURLWithPath: path)) }
    if let message = obj["error"] as? String { return .error(message) }
    return .log(line)
}
