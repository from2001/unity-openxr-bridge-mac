//
//  Utilities.swift
//  MetalXR
//
//  Created by Eilionoir Tunnicliff on 5/3/23.
//

import Foundation
import SystemConfiguration

struct CommandResult: Sendable {
    let output: String
    let exitCode: Int32

    var succeeded: Bool {
        exitCode == 0
    }
}

struct ADBDevice: Identifiable, Equatable, Sendable {
    let serial: String
    let state: String
    let details: String

    var id: String {
        serial
    }
}

struct DeveloperStatus: Sendable {
    let adbPath: String?
    let devices: [ADBDevice]
    let questClientInstalled: Bool
    let runtimeManifestExists: Bool
    let runtimeDylibExists: Bool
    let hostStreamerExists: Bool
    let protocolProbeExists: Bool
    let protocolMismatchDetected: Bool
    let questAPKExists: Bool
    let unityEditors: [String]

    var readyDevice: ADBDevice? {
        devices.first { $0.state == "device" }
    }

    var unauthorizedDevice: ADBDevice? {
        devices.first { $0.state == "unauthorized" }
    }

    var missingMessages: [String] {
        var messages = [String]()
        if adbPath == nil {
            messages.append("Bundled adb is missing.")
        }
        if unauthorizedDevice != nil {
            messages.append("Quest is connected but unauthorized. Accept USB debugging in the headset.")
        } else if readyDevice == nil {
            messages.append("No authorized Quest/Android device is connected.")
        }
        if !questClientInstalled {
            messages.append("Quest client package is not installed.")
        }
        if !runtimeManifestExists || !runtimeDylibExists {
            messages.append("MetalXR runtime is not built. Run Scripts/build-metalxr-runtime.sh.")
        }
        if !hostStreamerExists {
            messages.append("MetalXR host streamer is not built. Run Scripts/build-metalxr-host.sh.")
        }
        if !protocolProbeExists {
            messages.append("MetalXR protocol probe is not built. Run Scripts/build-metalxr-protocol.sh.")
        }
        if protocolMismatchDetected {
            messages.append("A protocol version mismatch was found in recent streamer logs. Rebuild and reinstall the Quest client APK.")
        }
        if !questAPKExists {
            messages.append("Quest client APK was not found. Run Scripts/build-quest-client-apk.sh.")
        }
        if unityEditors.isEmpty {
            messages.append("Unity Hub editor installation was not found under /Applications/Unity/Hub/Editor.")
        }
        return messages
    }
}

final class Utilities: NSObject, @unchecked Sendable {
    
#if DEBUG
    public let debugName = "debug"
#else
    public let debugName = "release"
#endif
    
    public let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as! String
    
    public let aprilFools = "04/01"
    
    public let appDataFolder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.appending(path: "MetalXR")
    public let appPlatformFolder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.appending(path: "MetalXR/platform-tools")
    public let appLogFolder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.appending(path: "MetalXR/logs")
    public let repositoryRootURL = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    
    // MARK: Downloader functions
    
    func loadFile(url: URL, destinationURL: URL, overwrite: Bool, completion: @escaping (String?, Error?) -> Void) {
        if FileManager.default.fileExists(atPath: destinationURL.path) && !overwrite {
            print("[Downloader] File already exists: \(destinationURL.path)")
            completion(destinationURL.path, nil)
        } else {
            let session = URLSession(configuration: URLSessionConfiguration.default, delegate: nil, delegateQueue: nil)
            var request = URLRequest(url: url)
            request.httpMethod = "GET"
            let task = session.dataTask(with: request, completionHandler: {
                data, response, error in
                if error == nil {
                    if let response = response as? HTTPURLResponse {
                        if response.statusCode == 200 {
                            if let data = data {
                                do {
                                    if overwrite && FileManager.default.fileExists(atPath: destinationURL.path) {
                                        try FileManager.default.removeItem(at: destinationURL)
                                    }
                                    try data.write(to: destinationURL, options: overwrite ? [] : Data.WritingOptions.withoutOverwriting)
                                    completion(destinationURL.path, error)
                                } catch {
                                    completion(destinationURL.path, error)
                                }
                            } else {
                                completion(destinationURL.path, error)
                            }
                        }
                    }
                } else {
                    completion(destinationURL.path, error)
                }
            })
            task.resume()
        }
    }
    
    // MARK: ADB functions

    var adbExecutableURL: URL? {
        Bundle.main.resourceURL?.appending(path: "adb-lib/adb")
    }

    var defaultQuestClientAPKURL: URL {
        repositoryRootURL.appending(path: "TestProjects/UnityOpenXRSmoke/Builds/MetalXRQuestClient.apk")
    }

    var unitySmokeProjectURL: URL {
        repositoryRootURL.appending(path: "TestProjects/UnityOpenXRSmoke")
    }

    var runtimeLogURL: URL {
        URL(fileURLWithPath: NSTemporaryDirectory()).appending(path: "metalxr_unity_runtime.log")
    }

    var streamerLogURL: URL? {
        appLogFolder?.appending(path: "metalxr_host_streamer.log")
    }

    var unityLaunchLogURL: URL? {
        appLogFolder?.appending(path: "metalxr_unity_launch.log")
    }
    
    func launchADBServer() {
        /*
         Function to launch the ADB server
         */
        
        runADBCommandHeadless(command: "start-server", shouldPrintOutput: false)
        print("[ADB Server] Launched ADB server.")
    }
    
    func killADBServer() {
        /*
         Function to kill the ADB server
         */
        
        runADBCommandHeadless(command: "kill-server", shouldPrintOutput: false)
        print("[ADB Server] Killed ADB server.")
    }
    
    func runADBCommandHeadless(command: String, shouldPrintOutput: Bool) {
        /*
         Wrapper around runADBCommand() to log the result
         if a new String variable is not needed
         */
        
        let result = runADBCommand(command: command, shouldPrintOutput: shouldPrintOutput)
        if shouldPrintOutput { print("[ADB Command] Output of command: " + result) }
    }
    
    func runADBCommand(command: String, shouldPrintOutput: Bool) -> String {
        let arguments = command.split(separator: " ").map(String.init)
        return runADBCommand(arguments: arguments, shouldPrintOutput: shouldPrintOutput).output
    }

    func runADBCommand(arguments: [String], shouldPrintOutput: Bool) -> CommandResult {
        guard let adbExecutableURL else {
            return CommandResult(output: "[ADB Command] Bundled adb executable was not found.\n", exitCode: 127)
        }

        let result = runProcess(
            executableURL: adbExecutableURL,
            arguments: arguments,
            currentDirectoryURL: adbExecutableURL.deletingLastPathComponent(),
            shouldPrintOutput: shouldPrintOutput
        )

        if shouldPrintOutput {
            print("[ADB Command] Ran command: \(adbExecutableURL.path) \(arguments.joined(separator: " "))")
        }

        return result
    }

    func listADBDevices() -> [ADBDevice] {
        let result = runADBCommand(arguments: ["devices", "-l"], shouldPrintOutput: false)
        return result.output
            .split(whereSeparator: \.isNewline)
            .dropFirst()
            .compactMap { line in
                let components = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
                guard components.count >= 2 else {
                    return nil
                }
                let details = components.dropFirst(2).joined(separator: " ")
                return ADBDevice(serial: components[0], state: components[1], details: details)
            }
    }

    func isPackageInstalled(packageName: String) -> Bool {
        let result = runADBCommand(arguments: ["shell", "pm", "path", packageName], shouldPrintOutput: true)
        return result.succeeded && result.output.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("package:")
    }

    func collectDeveloperStatus(packageName: String) -> DeveloperStatus {
        let devices = listADBDevices()
        return DeveloperStatus(
            adbPath: adbExecutableURL?.path,
            devices: devices,
            questClientInstalled: devices.contains { $0.state == "device" } && isPackageInstalled(packageName: packageName),
            runtimeManifestExists: FileManager.default.fileExists(atPath: repositoryRootURL.appending(path: "Runtime/MetalXRRuntime/metalxr_runtime.json").path),
            runtimeDylibExists: FileManager.default.fileExists(atPath: repositoryRootURL.appending(path: "Runtime/MetalXRRuntime/build/libmetalxr_runtime.dylib").path),
            hostStreamerExists: FileManager.default.fileExists(atPath: repositoryRootURL.appending(path: "Runtime/MetalXRHost/build/metalxr_host_streamer").path),
            protocolProbeExists: FileManager.default.fileExists(atPath: repositoryRootURL.appending(path: "Runtime/MetalXRProtocol/build/metalxr_protocol_loopback").path),
            protocolMismatchDetected: recentStreamerLogContainsProtocolMismatch(),
            questAPKExists: FileManager.default.fileExists(atPath: defaultQuestClientAPKURL.path),
            unityEditors: installedUnityEditors()
        )
    }

    func recentStreamerLogContainsProtocolMismatch() -> Bool {
        guard let streamerLogURL else {
            return false
        }
        guard let contents = try? String(contentsOf: streamerLogURL, encoding: .utf8) else {
            return false
        }
        return contents.contains("VERSION_MISMATCH") || contents.contains("Protocol major version mismatch")
    }

    func installedUnityEditors() -> [String] {
        let root = URL(fileURLWithPath: "/Applications/Unity/Hub/Editor")
        guard let enumerator = FileManager.default.enumerator(at: root, includingPropertiesForKeys: nil) else {
            return []
        }

        var editors = [String]()
        for case let url as URL in enumerator {
            if url.lastPathComponent == "Unity.app" {
                editors.append(url.path)
            }
        }
        return editors.sorted()
    }
    
    // MARK: Generic command functions
    
    func runCommandHeadless(command: String, shouldPrintOutput: Bool, directory: String) {
        /*
         Wrapper around runCommand() to log the result
         if a new String variable is not needed
         */
        
        let result = runCommand(command: command, shouldPrintOutput: shouldPrintOutput, directory: directory)
        if shouldPrintOutput { print("[Command] Output of command: " + result) }
    }
    
    func runCommand(command: String, shouldPrintOutput: Bool, directory: String) -> String {
        /*
         Function to run generic commands
         Working directory is the user's home folder
         */
        let task = Process()
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        task.executableURL = URL(fileURLWithPath: "/bin/bash")
        task.arguments = ["-lc", command]
        task.currentDirectoryURL = URL(fileURLWithPath: directory)
        task.standardInput = nil
        do {
            try task.run()
        } catch {
            return "[Command] Failed to run command: \(error.localizedDescription)\n"
        }
        if shouldPrintOutput { print("[Command] Ran command: '/bin/bash -c " + command + "'.") }
        let data = pipe.fileHandleForReading
        let output = String(data: data.readDataToEndOfFile(), encoding: .utf8) ?? ""
        task.waitUntilExit()
        return output
    }

    func runProcess(executableURL: URL, arguments: [String], currentDirectoryURL: URL?, shouldPrintOutput: Bool) -> CommandResult {
        let task = Process()
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        task.executableURL = executableURL
        task.arguments = arguments
        task.currentDirectoryURL = currentDirectoryURL
        task.standardInput = nil

        do {
            try task.run()
        } catch {
            return CommandResult(output: error.localizedDescription + "\n", exitCode: 127)
        }

        let output = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        task.waitUntilExit()

        if shouldPrintOutput {
            print("[Command] Exit code: \(task.terminationStatus)")
        }

        return CommandResult(output: output, exitCode: task.terminationStatus)
    }

    func runRepositoryScript(_ scriptPath: String, arguments: [String], shouldPrintOutput: Bool) -> CommandResult {
        let scriptURL = repositoryRootURL.appending(path: scriptPath)
        return runProcess(
            executableURL: scriptURL,
            arguments: arguments,
            currentDirectoryURL: repositoryRootURL,
            shouldPrintOutput: shouldPrintOutput
        )
    }

    func startRepositoryScript(_ scriptPath: String, arguments: [String], environment: [String: String], logURL: URL) throws -> Process {
        try FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        let logHandle = try FileHandle(forWritingTo: logURL)
        let process = Process()
        process.executableURL = repositoryRootURL.appending(path: scriptPath)
        process.arguments = arguments
        process.currentDirectoryURL = repositoryRootURL
        process.standardOutput = logHandle
        process.standardError = logHandle
        var mergedEnvironment = ProcessInfo.processInfo.environment
        for (key, value) in environment {
            mergedEnvironment[key] = value
        }
        process.environment = mergedEnvironment
        process.terminationHandler = { _ in
            try? logHandle.close()
        }
        try process.run()
        return process
    }

    func createDiagnosticsBundle(packageName: String, unityLaunchLogURL: URL?, streamerLogURL: URL?) -> Result<URL, Error> {
        do {
            guard let appLogFolder else {
                return .failure(URLError(.cannotCreateFile))
            }

            let timestamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
            let bundleURL = appLogFolder.appending(path: "diagnostics-\(timestamp)")
            try FileManager.default.createDirectory(at: bundleURL, withIntermediateDirectories: true)

            let status = runRepositoryScript("Scripts/metalxr-status.sh", arguments: [], shouldPrintOutput: false)
            try status.output.write(to: bundleURL.appending(path: "status.txt"), atomically: true, encoding: .utf8)

            let launchEnvironment = [
                "Repository: \(repositoryRootURL.path)",
                "Unity project: \(unitySmokeProjectURL.path)",
                "XR_RUNTIME_JSON: \(repositoryRootURL.appending(path: "Runtime/MetalXRRuntime/metalxr_runtime.json").path)",
                "METALXR_RUNTIME_LOG: \(runtimeLogURL.path)",
                "METALXR_TRACKING_STATE_PATH: /tmp/metalxr_tracking_state.txt",
                "METALXR_HAPTIC_COMMAND_PATH: /tmp/metalxr_haptic_command.txt",
                "METALXR_TIMING_STATE_PATH: /tmp/metalxr_timing_state.txt"
            ].joined(separator: "\n")
            try launchEnvironment.write(to: bundleURL.appending(path: "unity-launch-environment.txt"), atomically: true, encoding: .utf8)

            copyIfExists(runtimeLogURL, to: bundleURL.appending(path: "metalxr-runtime.log"))
            if let unityLaunchLogURL {
                copyIfExists(unityLaunchLogURL, to: bundleURL.appending(path: "unity-launch.log"))
            }
            if let streamerLogURL {
                copyIfExists(streamerLogURL, to: bundleURL.appending(path: "host-streamer.log"))
            }

            let logcat = runADBCommand(arguments: ["logcat", "-d", "Unity:D", "*:S"], shouldPrintOutput: false)
            let filteredLogcat = logcat.output
                .split(whereSeparator: \.isNewline)
                .filter { $0.contains("MetalXRQuestClient") || $0.contains(packageName) }
                .joined(separator: "\n")
            try filteredLogcat.write(to: bundleURL.appending(path: "quest-logcat.txt"), atomically: true, encoding: .utf8)

            return .success(bundleURL)
        } catch {
            return .failure(error)
        }
    }

    private func copyIfExists(_ sourceURL: URL, to destinationURL: URL) {
        guard FileManager.default.fileExists(atPath: sourceURL.path) else {
            return
        }
        try? FileManager.default.removeItem(at: destinationURL)
        try? FileManager.default.copyItem(at: sourceURL, to: destinationURL)
    }
    
    // MARK: App setup functions
    @Sendable func createAllDirectories() async {
        /*
         Creates the directories needed for the app to run properly
         */
        guard let appDataFolder, let appPlatformFolder, let appLogFolder else {
            print("[App Data] Unable to resolve application support directories.")
            return
        }

        try? FileManager.default.createDirectory(at: appDataFolder, withIntermediateDirectories: true)
        print("[App Data] Ensured presence of app documents directory.")
        try? FileManager.default.createDirectory(at: appPlatformFolder, withIntermediateDirectories: true)
        print("[App Data] Ensured presence of app platform-tools directory.")
        try? FileManager.default.createDirectory(at: appLogFolder, withIntermediateDirectories: true)
        print("[App Data] Ensured presence of app logging directory.")
    }
    
    func isConnectedToNetwork() -> Bool {
        /*
         Function to see if the device is connected to the internet.
         */

        var zeroAddress = sockaddr_in(sin_len: 0, sin_family: 0, sin_port: 0, sin_addr: in_addr(s_addr: 0), sin_zero: (0, 0, 0, 0, 0, 0, 0, 0))
        zeroAddress.sin_len = UInt8(MemoryLayout.size(ofValue: zeroAddress))
        zeroAddress.sin_family = sa_family_t(AF_INET)

        let defaultRouteReachability = withUnsafePointer(to: &zeroAddress) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {zeroSockAddress in
                SCNetworkReachabilityCreateWithAddress(nil, zeroSockAddress)
            }
        }

        var flags: SCNetworkReachabilityFlags = SCNetworkReachabilityFlags(rawValue: 0)
        guard let defaultRouteReachability else {
            return false
        }

        if SCNetworkReachabilityGetFlags(defaultRouteReachability, &flags) == false {
            return false
        }
        
        let isReachable = (flags.rawValue & UInt32(kSCNetworkFlagsReachable)) != 0
        let needsConnection = (flags.rawValue & UInt32(kSCNetworkFlagsConnectionRequired)) != 0
        let ret = (isReachable && !needsConnection)

        return ret
    }
}
