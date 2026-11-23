import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import mysql from "mysql2/promise";
import { seekdb } from "@oceanbase/seekdb-node";

const manifest = JSON.parse(readFileSync(join(process.cwd(), "node_modules/@oceanbase/seekdb-node/package.json"), "utf8"));
assert.equal(manifest.version, process.argv[2]);
seekdb.checkPlatform();
const db = await seekdb.open(join(process.cwd(), "database"));
let connection;
try {
  const endpoint = db.connectionOptions();
  const location = endpoint.transport === "tcp"
    ? { host: endpoint.host, port: endpoint.port }
    : { socketPath: endpoint.transport === "unix_socket" ? endpoint.unixSocket : endpoint.namedPipe };
  connection = await mysql.createConnection({ ...location, user: endpoint.user, database: "test" });
  await connection.query("CREATE TABLE smoke_test (value INT)");
  await connection.query("INSERT INTO smoke_test VALUES (42)");
  const [rows] = await connection.query("SELECT value FROM smoke_test");
  assert.equal(rows[0].value, 42);
} finally {
  if (connection) await connection.end();
  db.close();
}
assert.equal(db.closed, true);
console.log("seekdb installed npm tarball smoke passed");
