// Copyright (c) 2025 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import Foundation
import MySQLNIO
import NIOCore
import NIOPosix
import SeekDB
import SwiftUI

@main
struct SeekDBExampleApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @State private var status = "Running SELECT 1..."

    var body: some View {
        Text(status)
            .padding()
            .task {
                status = await Task.detached { runSelectOne() }.value
            }
    }
}

enum ExampleError: Error {
    case missingSocket
    case unexpectedRows(String)
}

func runSelectOne() -> String {
    let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    let report: [String: Any]
    do {
        report = ["select_1": try selectOne(in: documents.appendingPathComponent("seekdb").path)]
    } catch {
        report = ["error": String(describing: error)]
    }
    let data = (try? JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])) ?? Data()
    try? data.write(to: documents.appendingPathComponent("select1.json"), options: .atomic)
    let text = String(decoding: data, as: UTF8.self)
    print(text)
    return text
}

func selectOne(in directory: String) throws -> Int {
    let db = try SeekDB.open(directory)
    defer { try? db.close() }
    let options = try db.connectionOptions()
    guard let socket = options.unix_socket else {
        throw ExampleError.missingSocket
    }
    let group = MultiThreadedEventLoopGroup(numberOfThreads: 1)
    defer { try? group.syncShutdownGracefully() }
    let connection = try MySQLConnection.connect(
        to: try SocketAddress(unixDomainSocketPath: socket),
        username: options.user ?? "root",
        database: "",
        password: nil,
        tlsConfiguration: nil,
        on: group.next()
    ).wait()
    defer { try? connection.close().wait() }
    let query = TextQuery(sql: "SELECT 1")
    try connection.send(query, logger: connection.logger).wait()
    guard query.rows.count == 1, let value = query.rows[0].column("1")?.int else {
        throw ExampleError.unexpectedRows(String(describing: query.rows))
    }
    return value
}
