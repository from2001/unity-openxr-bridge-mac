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
