//
//  InitialView.swift
//  MetalXR
//
//  Created by Darien Johnson on 5/2/23.
//

import SwiftUI
import Foundation
import UniformTypeIdentifiers

@MainActor
struct InitialView: View {
    private let utils = Utilities()
    private let packageName = "dev.peapods.MetalXR"

    @State private var showFailureAlert = false
    @State private var operationIsActive = false
    @State private var isInstalled = false
    @State private var isDeviceConnected = false
    @State private var failureAlertMessage = String()
    @State private var connectedDeviceSummary = "No devices connected"
    @State private var showAPKImporter = false
    
    var body: some View {
        
        NavigationStack {
            VStack {
                Text("MetalXR")
                    .font(.title)
                    .fontWeight(.bold)
                    .padding(.bottom, 10)
                    .task {
                        await runStartupChecks()
                    }
                    .task {
                        await beginScanningForDevices()
                    }
                    .task(utils.createAllDirectories)
                
                HStack {
                    if isDeviceConnected && isInstalled {
                        NavigationLink { HomeView() } label: { Text("Open MetalXR") }
                    } else {
                        Button(action: { showAPKImporter = true }) {
                            Text(isDeviceConnected ? operationIsActive ? "Installing APK" : "Install APK..." : "No devices connected")
                        }
                        .alert(isPresented: $showFailureAlert) {
                            Alert(
                                title: Text("MetalXR can't continue."),
                                message: Text("Something went wrong while we were getting things ready for you:\n\n" + failureAlertMessage)
                            )
                        }
                        .disabled(!isDeviceConnected || operationIsActive)

                        Button(action: { downloadAndInstallAPK() }) {
                            Text("Download Legacy Client")
                        }
                        .disabled(!isDeviceConnected || operationIsActive)

                        if operationIsActive {
                            ProgressView()
                                .padding(.leading, 5)
                                .scaleEffect(0.5)
                        }
                    }
                }

                Text(connectedDeviceSummary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(.top, 6)
            }
            .padding()
        }
        .fileImporter(
            isPresented: $showAPKImporter,
            allowedContentTypes: [.item],
            allowsMultipleSelection: false
        ) { result in
            Task {
                await handleAPKImport(result)
            }
        }
#if DEBUG
        .navigationTitle("MetalXR (" + utils.version + "-" + utils.debugName + ")")
#else
        .navigationTitle("MetalXR")
#endif
    }
    
    func runStartupChecks() async {
        await refreshDeviceState()
    }

    func checkForInstalledApp() async {
        guard isDeviceConnected else {
            isInstalled = false
            return
        }

        let installed = await Task.detached(priority: .utility) {
            utils.isPackageInstalled(packageName: packageName)
        }.value
        isInstalled = installed
        print(isInstalled ? "[Installer] MetalXR is already installed on device." :
                            "[Installer] MetalXR is not installed on device.")
    }
    
    func beginScanningForDevices() async {
        while !Task.isCancelled {
            await refreshDeviceState()
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    func refreshDeviceState() async {
        let devices = await Task.detached(priority: .utility) {
            utils.listADBDevices()
        }.value

        let readyDevices = devices.filter { $0.state == "device" }
        isDeviceConnected = !readyDevices.isEmpty

        if let firstDevice = readyDevices.first {
            connectedDeviceSummary = "Connected: \(firstDevice.serial)"
        } else if let firstDevice = devices.first {
            connectedDeviceSummary = "Device \(firstDevice.serial) is \(firstDevice.state)"
        } else {
            connectedDeviceSummary = "No devices connected"
        }

        await checkForInstalledApp()
    }
    
    func downloadAndInstallAPK() {
        Task {
            await downloadAndInstallDefaultAPK()
        }
    }

    func downloadAndInstallDefaultAPK() async {
        let connected = await Task.detached(priority: .utility) {
            utils.isConnectedToNetwork()
        }.value

        guard connected else {
            showFailureAlert = true
            failureAlertMessage = "MetalXR requires an internet connection to download the legacy client APK. You can still install a local APK."
            return
        }

        guard let packageURL = utils.appDataFolder?.appending(path: "metalxr.apk") else {
            showFailureAlert = true
            failureAlertMessage = "MetalXR could not resolve its application support folder."
            return
        }

        operationIsActive = true
        defer { operationIsActive = false }

#if DEBUG
        let downloadURL = URL(string: "https://github.com/PeaPodDevs/MetalXRClient/releases/download/latest/dev.peapods.MetalXR.apk")
#else
        let downloadURL = URL(string: "https://github.com/PeaPodDevs/MetalXRClient/releases/download/" + utils.version + "/dev.peapods.MetalXR.apk")
#endif

        guard let downloadURL else {
            showFailureAlert = true
            failureAlertMessage = "MetalXR could not create the client APK download URL."
            return
        }

        do {
            if !FileManager.default.fileExists(atPath: packageURL.path) {
                let (temporaryURL, response) = try await URLSession.shared.download(from: downloadURL)
                if let httpResponse = response as? HTTPURLResponse, httpResponse.statusCode != 200 {
                    throw URLError(.badServerResponse)
                }
                try? FileManager.default.removeItem(at: packageURL)
                try FileManager.default.moveItem(at: temporaryURL, to: packageURL)
            }
        } catch {
            print("[Downloader] MetalXR failed to download: \(error.localizedDescription)")
            showFailureAlert = true
            failureAlertMessage = error.localizedDescription
            return
        }

        await installAPK(at: packageURL)
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
            print("[Installer] MetalXR was installed.")
            isInstalled = true
        } else {
            print("[Installer] MetalXR failed to install, posting ADB output: " + result.output)
            showFailureAlert = true
            failureAlertMessage = result.output.isEmpty ? "adb install failed with exit code \(result.exitCode)." : result.output
        }
    }
}

struct InitialView_Previews: PreviewProvider {
    static var previews: some View {
        InitialView()
    }
}
