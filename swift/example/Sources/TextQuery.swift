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

import MySQLNIO

final class TextQuery: MySQLCommand {
    private enum Stage {
        case ready
        case columns
        case columnsEnd
        case rows
    }

    private let sql: String
    private var stage = Stage.ready
    private var columnCount = 0
    private var columns: [MySQLProtocol.ColumnDefinition41] = []
    private(set) var rows: [MySQLRow] = []

    init(sql: String) {
        self.sql = sql
    }

    func activate(capabilities: MySQLProtocol.CapabilityFlags) throws -> MySQLCommandState {
        MySQLCommandState(response: [try .encode(MySQLProtocol.COM_QUERY(query: sql), capabilities: capabilities)])
    }

    func handle(packet: inout MySQLPacket, capabilities: MySQLProtocol.CapabilityFlags) throws -> MySQLCommandState {
        if packet.isError {
            throw MySQLError.server(try packet.decode(MySQLProtocol.ERR_Packet.self, capabilities: capabilities))
        }
        switch stage {
        case .ready:
            if packet.isOK {
                return MySQLCommandState(done: true)
            }
            columnCount = Int(try packet.decode(MySQLProtocol.COM_QUERY_Response.self, capabilities: capabilities).columnCount)
            stage = .columns
        case .columns:
            columns.append(try packet.decode(MySQLProtocol.ColumnDefinition41.self, capabilities: capabilities))
            if columns.count == columnCount {
                stage = capabilities.contains(.CLIENT_DEPRECATE_EOF) ? .rows : .columnsEnd
            }
        case .columnsEnd:
            stage = .rows
        case .rows:
            if packet.isEOF {
                return MySQLCommandState(done: true)
            }
            let row = try MySQLProtocol.TextResultSetRow.decode(from: &packet, columnCount: columns.count)
            rows.append(MySQLRow(format: .text, columnDefinitions: columns, values: row.values))
        }
        return MySQLCommandState()
    }
}
