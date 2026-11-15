import AppKit
import BuilderCore
import SwiftUI
import UniformTypeIdentifiers

/// The rows, each with its state and the button that fixes it, then Build.
struct ChecklistView: View {
    @ObservedObject var model: BuilderModel
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(CheckID.allCases, id: \.self) { id in
                CheckRow(title: id.title, state: model.states[id] ?? .checking("Checking..."), note: note(for: id),
                         buttons: buttons(for: id))
                if id != CheckID.allCases.last { Divider() }
            }
            Button(action: model.build) {
                Text(model.buildTitle)
                    .fontWeight(.semibold)
                    .frame(maxWidth: .infinity)
            }
            .controlSize(.large)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.buildEnabled)
            .padding(.top, 16)
            Text("Your saves and settings are kept outside the app; building again never touches them.")
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.top, 6)
        }
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.accentColor, lineWidth: 3)
                .padding(-10)
                .opacity(dropTargeted ? 1 : 0)
        )
        .onDrop(of: [UTType.fileURL], isTargeted: $dropTargeted, perform: drop)
    }

    private func note(for id: CheckID) -> String? {
        switch id {
        case .patch11, .patch12: return "Downloads from PCGamingWiki and accepts its download terms for you"
        case .fanPatch: return "Downloads from bwgame.net"
        default: return nil
        }
    }

    private func buttons(for id: CheckID) -> [RowButton] {
        let state = model.states[id]
        switch (id, state) {
        case (.commandLineTools, .blocked?):
            return [RowButton(label: "Install", action: model.installCommandLineTools)]
        case (.buildTools, .blocked?):
            return [RowButton(label: "Set Up", action: model.setUpBuildTools)]
        case (.discs, _):
            return [RowButton(label: "Choose...", action: chooseDiscs)]
        case (.patch11, _), (.patch12, _), (.fanPatch, _):
            let busy = model.isDownloading(id)
            return [RowButton(label: "Download", enabled: model.toolsReady && !busy, action: { model.download(id) }),
                    RowButton(label: "I have it...", enabled: !busy, action: { choosePatch(id) })]
        case (.destination, _):
            return [RowButton(label: "Change...", action: chooseDestination)]
        default:
            return []
        }
    }

    private func chooseDiscs() {
        let panel = NSOpenPanel()
        panel.message = "Choose the four discs: their zips, .bin and .cue files or .iso files, or a folder that holds them"
        panel.prompt = "Choose"
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = ["zip", "bin", "cue", "iso"].compactMap { UTType(filenameExtension: $0) } + [.folder]
        if let current = model.choices.discURLs.first { panel.directoryURL = current.deletingLastPathComponent() }
        if panel.runModal() == .OK, !panel.urls.isEmpty { model.setDiscs(panel.urls) }
    }

    private func choosePatch(_ id: CheckID) {
        let panel = NSOpenPanel()
        panel.message = "Choose \(id.title.lowercased()) (\(id.expectedFile))"
        panel.prompt = "Choose"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [UTType(filenameExtension: "exe")].compactMap { $0 }
        if let current = model.choices.patch(id) { panel.directoryURL = current.deletingLastPathComponent() }
        if panel.runModal() == .OK, let url = panel.url { model.setPatch(id, url) }
    }

    private func chooseDestination() {
        let panel = NSOpenPanel()
        panel.message = "Choose where to put \(gameAppName)"
        panel.prompt = "Choose"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = model.destination
        if panel.runModal() == .OK, let url = panel.url { model.setDestination(url) }
    }

    private func drop(_ providers: [NSItemProvider]) -> Bool {
        guard model.phase == .checklist, !providers.isEmpty else { return false }
        var urls: [URL] = []
        let group = DispatchGroup()
        let lock = NSLock()
        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                let url = (item as? Data).flatMap { URL(dataRepresentation: $0, relativeTo: nil) } ?? (item as? URL)
                if let url = url {
                    lock.lock()
                    urls.append(url)
                    lock.unlock()
                }
                group.leave()
            }
        }
        group.notify(queue: .main) { model.choose(urls) }
        return true
    }
}

struct RowButton {
    let label: String
    var enabled = true
    let action: () -> Void
}

func copyToPasteboard(_ text: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
}

struct CheckRow: View {
    let title: String
    let state: CheckState
    let note: String?
    let buttons: [RowButton]

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(state.color)
                .frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).fontWeight(.semibold)
                Text(BuilderModel.plain(state.text))
                    .font(.callout)
                    .foregroundColor(state.isBlocked ? Color(NSColor.systemRed) : .secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                if let note = note, !state.isReady {
                    Text(note)
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            ForEach(buttons, id: \.label) { button in
                Button(button.label, action: button.action).disabled(!button.enabled)
            }
        }
        .padding(.vertical, 8)
    }
}

extension CheckID {
    var title: String {
        switch self {
        case .mac: return "Mac"
        case .commandLineTools: return "Xcode Command Line Tools"
        case .buildTools: return "Build tools (Python)"
        case .discs: return "Discs 1-4"
        case .patch11: return "Patch 1.1"
        case .patch12: return "Patch 1.2"
        case .fanPatch: return "Fan patch 1.42"
        case .destination: return "Destination"
        case .diskSpace: return "Free disk space"
        }
    }

    /// The file a patch row wants, as the patch's site names it.
    var expectedFile: String {
        switch self {
        case .patch11: return "BW2Patch1_1.exe"
        case .patch12: return "BW2Patch1_2.exe"
        case .fanPatch: return "B&W2 Fan Patch v1.42 Installer.exe"
        default: return ""
        }
    }
}

extension CheckState {
    var isChecking: Bool {
        if case .checking = self { return true }
        return false
    }

    var color: Color {
        switch self {
        case .checking: return Color(NSColor.systemGray)
        case .ready: return Color(NSColor.systemGreen)
        case .automatic: return Color(NSColor.systemOrange)
        case .blocked: return Color(NSColor.systemRed)
        }
    }
}
