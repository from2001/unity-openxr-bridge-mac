//
//  InitialView.swift
//  MetalXR
//
//  Created by Darien Johnson on 5/2/23.
//

import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
struct InitialView: View {
    private let utils = Utilities()
    private let packageName = "com.metalxr.questclient"

    @State private var showFailureAlert = false
    @State private var operationIsActive = false
    @State private var showAPKImporter = false
    @State private var failureAlertMessage = String()
    @State private var connectedDeviceSummary = "No devices connected"
    @State private var developerStatus: DeveloperStatus?
    @State private var unityProcess: Process?
    @State private var streamProcess: Process?
    @State private var diagnosticsURL: URL?
    @State private var lastActionMessage = "Ready"

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    header
                    statusGrid
                    workflowControls
                    troubleshooting
                    diagnostics
                }
                .frame(maxWidth: 860, alignment: .leading)
                .padding()
            }
            .task {
                await runStartupChecks()
            }
            .task {
                await beginScanningForDevices()
            }
            .task(utils.createAllDirectories)
            .fileImporter(
                isPresented: $showAPKImporter,
                allowedContentTypes: [.item],
                allowsMultipleSelection: false
            ) { result in
                Task {
                    await handleAPKImport(result)
                }
            }
            .alert(isPresented: $showFailureAlert) {
                Alert(
                    title: Text("MetalXR can't continue."),
                    message: Text(failureAlertMessage)
                )
            }
        }
#if DEBUG
        .navigationTitle("MetalXR (" + utils.version + "-" + utils.debugName + ")")
#else
        .navigationTitle("MetalXR")
#endif
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("MetalXR")
                    .font(.title)
                    .fontWeight(.bold)
                Spacer()
                Button("Refresh") {
                    Task { await refreshDeviceState() }
                }
                .disabled(operationIsActive)
            }

            Text(connectedDeviceSummary)
                .foregroundStyle(.secondary)
            Text(lastActionMessage)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var statusGrid: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 220), spacing: 12)], alignment: .leading, spacing: 12) {
            StatusTile(title: "Quest", value: questStatusText, detail: questDetailText, isReady: developerStatus?.readyDevice != nil)
            StatusTile(title: "Quest Client", value: questClientStatusText, detail: packageName, isReady: developerStatus?.questClientRunning == true)
            StatusTile(title: "Runtime", value: runtimeStatusText, detail: "MetalXR OpenXR runtime", isReady: runtimeIsReady)
            StatusTile(title: "Protocol", value: protocolStatusText, detail: "Host/client packet version", isReady: protocolIsReady)
            StatusTile(title: "Host Streamer", value: streamerStatusText, detail: "VideoToolbox stream host", isReady: developerStatus?.hostStreamerExists == true)
            StatusTile(title: "Unity", value: unityStatusText, detail: "UnityOpenXRSmoke project", isReady: !(developerStatus?.unityEditors.isEmpty ?? true))
            StatusTile(title: "Stream", value: streamStatusText, detail: reverseStatusText, isReady: streamProcess?.isRunning == true && developerStatus?.adbReverseConfigured == true)
        }
    }

    private var workflowControls: some View {
        SectionBox(title: "Workflow") {
            HStack(spacing: 10) {
                Button("Install Built APK") {
                    Task { await installDefaultQuestClient() }
                }
                .disabled(operationIsActive || developerStatus?.readyDevice == nil || developerStatus?.questAPKExists != true)

                Button("Install APK...") {
                    showAPKImporter = true
                }
                .disabled(operationIsActive || developerStatus?.readyDevice == nil)

                Button("Launch Client") {
                    Task { await launchQuestClient() }
                }
                .disabled(operationIsActive || developerStatus?.readyDevice == nil || developerStatus?.questClientInstalled != true)

                Button("Launch Unity") {
                    launchUnity()
                }
                .disabled(operationIsActive || unityProcess?.isRunning == true || !runtimeIsReady)

                Button(streamProcess?.isRunning == true ? "Stop Stream" : "Start Stream") {
                    if streamProcess?.isRunning == true {
                        stopStream()
                    } else {
                        startStream()
                    }
                }
                .disabled(operationIsActive || developerStatus?.hostStreamerExists != true || developerStatus?.readyDevice == nil)

                if operationIsActive {
                    ProgressView()
                        .controlSize(.small)
                }
            }
            .buttonStyle(.bordered)
        }
    }

    private var troubleshooting: some View {
        SectionBox(title: "Troubleshooting") {
            let messages = developerStatus?.missingMessages ?? ["Status has not been refreshed yet."]
            VStack(alignment: .leading, spacing: 6) {
                ForEach(messages, id: \.self) { message in
                    Text(message)
                        .font(.callout)
                }
            }
        }
    }

    private var diagnostics: some View {
        SectionBox(title: "Diagnostics") {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 10) {
                    Button("Export Diagnostics") {
                        exportDiagnostics()
                    }
                    .disabled(operationIsActive)

                    if let diagnosticsURL {
                        Button("Reveal") {
                            NSWorkspace.shared.activateFileViewerSelecting([diagnosticsURL])
                        }
                    }
                }
                .buttonStyle(.bordered)

                Text("Includes status output, Unity launch environment, frame export manifests, runtime log, streamer log, Quest screenshot checks, and MetalXR Quest logcat excerpts.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var questStatusText: String {
        guard let status = developerStatus else {
            return "Checking"
        }
        if status.readyDevice != nil {
            return "Connected"
        }
        if status.unauthorizedDevice != nil {
            return "Unauthorized"
        }
        return "Missing"
    }

    private var questDetailText: String {
        guard let status = developerStatus else {
            return "adb scan pending"
        }
        if let readyDevice = status.readyDevice {
            return readyDevice.serial
        }
        if let unauthorizedDevice = status.unauthorizedDevice {
            return unauthorizedDevice.serial
        }
        return "Connect Quest with USB debugging enabled"
    }

    private var questClientStatusText: String {
        if developerStatus?.questClientRunning == true {
            return "Running"
        }
        return developerStatus?.questClientInstalled == true ? "Installed" : "Missing"
    }

    private var runtimeStatusText: String {
        runtimeIsReady ? "Built" : "Missing"
    }

    private var streamerStatusText: String {
        developerStatus?.hostStreamerExists == true ? "Built" : "Missing"
    }

    private var protocolStatusText: String {
        if developerStatus?.protocolMismatchDetected == true {
            return "Mismatch"
        }
        return developerStatus?.protocolProbeExists == true ? "Ready" : "Missing"
    }

    private var unityStatusText: String {
        if unityProcess?.isRunning == true {
            return "Launching"
        }
        return (developerStatus?.unityEditors.isEmpty ?? true) ? "Missing" : "Available"
    }

    private var streamStatusText: String {
        streamProcess?.isRunning == true ? "Running" : "Stopped"
    }

    private var reverseStatusText: String {
        developerStatus?.adbReverseConfigured == true ? "adb reverse TCP 47000 ready" : "adb reverse TCP 47000 missing"
    }

    private var runtimeIsReady: Bool {
        developerStatus?.runtimeManifestExists == true && developerStatus?.runtimeDylibExists == true
    }

    private var protocolIsReady: Bool {
        developerStatus?.protocolProbeExists == true && developerStatus?.protocolMismatchDetected != true
    }

    func runStartupChecks() async {
        await refreshDeviceState()
    }

    func beginScanningForDevices() async {
        while !Task.isCancelled {
            await refreshDeviceState()
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }

    func refreshDeviceState() async {
        let status = await Task.detached(priority: .utility) {
            utils.collectDeveloperStatus(packageName: packageName)
        }.value
        developerStatus = status

        if let readyDevice = status.readyDevice {
            connectedDeviceSummary = "Connected: \(readyDevice.serial)"
        } else if let firstDevice = status.devices.first {
            connectedDeviceSummary = "Device \(firstDevice.serial) is \(firstDevice.state)"
        } else {
            connectedDeviceSummary = "No devices connected"
        }
    }

    func installDefaultQuestClient() async {
        await installAPK(at: utils.defaultQuestClientAPKURL)
    }

    func handleAPKImport(_ result: Result<[URL], Error>) async {
        switch result {
        case .success(let urls):
            guard let apkURL = urls.first else {
                return
            }
            await installAPK(at: apkURL)
        case .failure(let error):
            showFailureAlert = true
            failureAlertMessage = error.localizedDescription
        }
    }

    func installAPK(at apkURL: URL) async {
        operationIsActive = true
        defer { operationIsActive = false }

        guard FileManager.default.fileExists(atPath: apkURL.path) else {
            showFailureAlert = true
            failureAlertMessage = "APK file does not exist: \(apkURL.path)"
            return
        }

        let didStartAccess = apkURL.startAccessingSecurityScopedResource()
        defer {
            if didStartAccess {
                apkURL.stopAccessingSecurityScopedResource()
            }
        }

        let result = await Task.detached(priority: .utility) {
            utils.runADBCommand(arguments: ["install", "-r", apkURL.path], shouldPrintOutput: true)
        }.value

        if result.succeeded && result.output.contains("Success") {
            lastActionMessage = "Quest client installed."
            await refreshDeviceState()
        } else {
            showFailureAlert = true
            failureAlertMessage = result.output.isEmpty ? "adb install failed with exit code \(result.exitCode)." : result.output
        }
    }

    func launchQuestClient() async {
        operationIsActive = true
        defer { operationIsActive = false }

        let result = await Task.detached(priority: .utility) {
            let reverseRemove = utils.runADBCommand(arguments: ["reverse", "--remove", "tcp:47000"], shouldPrintOutput: true)
            _ = reverseRemove
            let reverse = utils.runADBCommand(arguments: ["reverse", "tcp:47000", "tcp:47000"], shouldPrintOutput: true)
            if !reverse.succeeded {
                return reverse
            }

            _ = utils.runADBCommand(arguments: ["logcat", "-c"], shouldPrintOutput: false)
            _ = utils.runADBCommand(arguments: ["shell", "am", "force-stop", packageName], shouldPrintOutput: true)
            return utils.runADBCommand(
                arguments: ["shell", "monkey", "-p", packageName, "-c", "com.oculus.intent.category.VR", "1"],
                shouldPrintOutput: true
            )
        }.value

        if result.succeeded {
            lastActionMessage = "Quest client launched with adb reverse tcp:47000."
            await refreshDeviceState()
        } else {
            showFailureAlert = true
            failureAlertMessage = result.output.isEmpty ? "Quest client launch failed with exit code \(result.exitCode)." : result.output
        }
    }

    func launchUnity() {
        guard let logURL = utils.unityLaunchLogURL else {
            showFailureAlert = true
            failureAlertMessage = "MetalXR could not resolve the Unity launch log path."
            return
        }

        do {
            unityProcess = try utils.startRepositoryScript(
                "Scripts/launch-unity-openxr.sh",
                arguments: [utils.unitySmokeProjectURL.path],
                environment: [
                    "METALXR_RUNTIME_LOG": utils.runtimeLogURL.path,
                    "METALXR_FRAME_DUMP_DIR": utils.frameDumpDirURL.path,
                    "METALXR_FRAME_EXPORT_DIR": utils.frameExportDirURL.path,
                    "METALXR_FRAME_EXPORT_MODE": "readback",
                    "METALXR_SWAPCHAIN_STORAGE_MODE": "shared",
                    "METALXR_TRACKING_STATE_PATH": utils.trackingStateURL.path,
                    "METALXR_HAPTIC_COMMAND_PATH": utils.hapticCommandURL.path,
                    "METALXR_TIMING_STATE_PATH": utils.timingStateURL.path
                ],
                logURL: logURL
            )
            lastActionMessage = "Unity launch started with frame export enabled. Log: \(logURL.path)"
        } catch {
            showFailureAlert = true
            failureAlertMessage = error.localizedDescription
        }
    }

    func startStream() {
        guard let logURL = utils.streamerLogURL else {
            showFailureAlert = true
            failureAlertMessage = "MetalXR could not resolve the streamer log path."
            return
        }

        do {
            streamProcess = try utils.startRepositoryScript(
                "Scripts/run-metalxr-frame-stream.sh",
                arguments: [],
                environment: [
                    "METALXR_TRANSPORT": "usb",
                    "METALXR_FRAME_SOURCE": "unity-export",
                    "METALXR_FRAME_EXPORT_DIR": utils.frameExportDirURL.path,
                    "METALXR_TRACKING_STATE_PATH": utils.trackingStateURL.path,
                    "METALXR_HAPTIC_COMMAND_PATH": utils.hapticCommandURL.path,
                    "METALXR_TIMING_STATE_PATH": utils.timingStateURL.path
                ],
                logURL: logURL
            )
            lastActionMessage = "Host streamer started in unity-export mode. Log: \(logURL.path)"
            Task { await refreshDeviceState() }
        } catch {
            showFailureAlert = true
            failureAlertMessage = error.localizedDescription
        }
    }

    func stopStream() {
        streamProcess?.terminate()
        streamProcess = nil
        lastActionMessage = "Host streamer stopped."
        Task { await refreshDeviceState() }
    }

    func exportDiagnostics() {
        operationIsActive = true
        Task {
            let result = await Task.detached(priority: .utility) {
                utils.createDiagnosticsBundle(
                    packageName: packageName,
                    unityLaunchLogURL: utils.unityLaunchLogURL,
                    streamerLogURL: utils.streamerLogURL
                )
            }.value

            operationIsActive = false
            switch result {
            case .success(let url):
                diagnosticsURL = url
                lastActionMessage = "Diagnostics exported: \(url.path)"
            case .failure(let error):
                showFailureAlert = true
                failureAlertMessage = error.localizedDescription
            }
        }
    }
}

private struct StatusTile: View {
    let title: String
    let value: String
    let detail: String
    let isReady: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack {
                Circle()
                    .fill(isReady ? Color.green : Color.orange)
                    .frame(width: 8, height: 8)
                Text(value)
                    .font(.headline)
            }
            Text(detail)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct SectionBox<Content: View>: View {
    let title: String
    let content: Content

    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.headline)
            content
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.25), in: RoundedRectangle(cornerRadius: 8))
    }
}

struct InitialView_Previews: PreviewProvider {
    static var previews: some View {
        InitialView()
    }
}
