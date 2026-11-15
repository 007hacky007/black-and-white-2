import AppKit
import BuilderCore
import SwiftUI

/// After the build: Play and Show in Finder, or the failed step, its error and the log's last 200 lines.
struct ResultView: View {
    enum Outcome {
        case success(URL)
        case failure(step: String, message: String)
    }

    @ObservedObject var model: BuilderModel
    let outcome: Outcome

    var body: some View {
        switch outcome {
        case let .success(app): success(app)
        case let .failure(step, message): failure(step: step, message: message)
        }
    }

    private func success(_ app: URL) -> some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 56))
                .foregroundColor(Color(NSColor.systemGreen))
            Text("Ready").font(.title).fontWeight(.semibold)
            Text("\(gameAppName) is in \(abbreviated(app.deletingLastPathComponent())).")
                .foregroundColor(.secondary)
            Text("The game is inside the app. Your saves and settings are kept in ~/Library/Application Support/Black & White 2.")
                .font(.callout)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([app]) }
                Button("Play") { model.play(app) }
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(.large)
            if let note = model.cleanNote {
                Text(note)
                    .font(.caption)
                    .foregroundColor(Color(NSColor.systemOrange))
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            Button("Back to the checklist", action: model.backToChecklist)
                .controlSize(.small)
        }
        .frame(maxWidth: .infinity, minHeight: 500)
    }

    private func failure(step: String, message: String) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "xmark.octagon.fill")
                    .font(.system(size: 28))
                    .foregroundColor(Color(NSColor.systemRed))
                Text("\(step) failed").font(.title2).fontWeight(.semibold)
            }
            Text(message)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
            Text("The log's last 200 lines").font(.headline).padding(.top, 4)
            LogView(lines: model.log, shown: 200, height: 300)
            HStack {
                Button("Copy Log") { copyToPasteboard(model.log.joined(separator: "\n")) }
                Spacer()
                Button("Back to the checklist", action: model.backToChecklist)
                    .keyboardShortcut(.defaultAction)
            }
        }
    }
}
