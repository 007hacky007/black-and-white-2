import AppKit
import BuilderCore
import SwiftUI

/// The build: the step the CLI announced, the bar, the log's tail, and Cancel.
struct BuildView: View {
    @ObservedObject var model: BuilderModel
    let title: String
    let fraction: Double
    @State private var showDetails = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Building Black & White 2")
                .font(.title2)
                .fontWeight(.semibold)
            Text(title)
                .font(.headline)
            ProgressView(value: fraction)
            Text("This takes about 10 to 15 minutes. You can keep using your Mac.")
                .font(.callout)
                .foregroundColor(.secondary)
            DisclosureGroup("Show details", isExpanded: $showDetails) {
                LogView(lines: model.log)
            }
            HStack {
                Spacer()
                Button("Cancel", action: model.cancelBuild)
                    .keyboardShortcut(.cancelAction)
                    .disabled(model.cancelDisabled)
            }
        }
    }
}

/// The last `shown` lines of a log, scrolled to the end.
struct LogView: View {
    let lines: [String]
    var shown = 200
    var height: CGFloat = 200

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    Text(lines.suffix(shown).joined(separator: "\n"))
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(6)
                    Color.clear.frame(height: 1).id("end")
                }
            }
            .frame(height: height)
            .background(Color(NSColor.textBackgroundColor))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color(NSColor.separatorColor)))
            .onAppear { proxy.scrollTo("end", anchor: .bottom) }
            .onChange(of: lines.count) { _ in proxy.scrollTo("end", anchor: .bottom) }
        }
    }
}
