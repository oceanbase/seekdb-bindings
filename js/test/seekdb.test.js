'use strict';

const { after, test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

// ---------------------------------------------------------------------------
// Layout: make sure libseekdb and the seekdb server binary sit next to the
// addon. libseekdb spawns the server as "<libseekdb dir>/seekdb". This must
// happen BEFORE requiring lib/index.js: the addon links libseekdb at load
// time via @loader_path/$ORIGIN.
//
// Copies are atomic (write to a temp file, then rename). In-place overwrite
// of a binary that is being exec'd by a running seekdb server makes dyld read
// a truncated file and the process hangs in an unkillable state (stat "UE").
// ---------------------------------------------------------------------------

function atomicCopy(src, dest) {
  const tmp = `${dest}.tmp-${process.pid}`;
  fs.copyFileSync(src, tmp);
  fs.renameSync(tmp, dest);
}

const addonDir = path.join(__dirname, '..', 'build', 'Release');
const libDir = process.env.SEEKDB_LIB_DIR || path.join(__dirname, '..', '..', 'build');

if (!fs.existsSync(addonDir)) {
  fs.mkdirSync(addonDir, { recursive: true });
}
for (const entry of fs.readdirSync(libDir)) {
  if (entry.startsWith('libseekdb')) {
    atomicCopy(path.join(libDir, entry), path.join(addonDir, entry));
  }
}
const seekdbBin = process.env.SEEKDB_BIN || path.join(libDir, 'seekdb');
if (fs.existsSync(seekdbBin)) {
  atomicCopy(seekdbBin, path.join(addonDir, 'seekdb'));
  fs.chmodSync(path.join(addonDir, 'seekdb'), 0o755);
} else {
  throw new Error(
    `seekdb server binary not found at ${seekdbBin}. ` +
      'Build the C SDK first (SEEKDB_BIN=<path-to-seekdb> cmake -S . -B build && cmake --build build --target seekdb), ' +
      'or point SEEKDB_BIN at an existing seekdb binary before running tests.',
  );
}

const seekdb = require('../lib/index.js');

const dbDir = fs.mkdtempSync(path.join(os.tmpdir(), 'seekdb-node-test-'));

after(async () => {
  // The seekdb server process spawned by libseekdb shuts down asynchronously
  // after the last instance close; until then it may still be writing to
  // <dbDir>/log. Retry the removal so the Linux CI runner does not hit
  // ENOTEMPTY while the server is flushing its final log lines.
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  for (let attempt = 0; attempt < 20; attempt++) {
    try {
      fs.rmSync(dbDir, { recursive: true, force: true });
      return;
    } catch (err) {
      if (err.code !== 'ENOTEMPTY' && err.code !== 'EBUSY' && err.code !== 'EAGAIN') {
        throw err;
      }
      await sleep(100);
    }
  }
  fs.rmSync(dbDir, { recursive: true, force: true });
});

// ---------------------------------------------------------------------------

test('smoke: open, connect, ddl/dml, hybrid search', async () => {
  const instance = await seekdb.open(dbDir);
  assert.equal(instance.closed, false);
  assert.equal(instance.dbDir, dbDir);

  const connection = await instance.connect('test', false);
  const cursor = connection.cursor();

  await cursor.execute('drop table if exists doc_table');
  await cursor.execute(`create table doc_table(c1 int,
                                  vector vector(3),
                                  query varchar(255),
                                  content varchar(255),
                                  vector index idx1(vector) with
                                       (distance=l2, type=hnsw, lib=vsag),
                                  fulltext idx2(query),
                                  fulltext idx3(content))`);

  const affected = await cursor.execute(`insert into doc_table values
              (1, '[1,2,3]', "hello world", "oceanbase Elasticsearch database"),
              (2, '[1,2,1]', "hello world, what is your name", "oceanbase mysql database"),
              (3, '[1,1,1]', "hello world, how are you", "oceanbase oracle database"),
              (4, '[1,3,1]', "real world, where are you from", "postgres oracle database"),
              (5, '[1,3,2]', "real world, how old are you", "redis oracle database"),
              (6, '[2,1,1]', "hello world, where are you from", "starrocks oceanbase database")`);
  // execute() returns the result-set row count; INSERT has no result set, so
  // it is 0 (same as Python). The rows are verified by the SELECT below.
  assert.equal(affected, 0);
  await connection.commit();

  await cursor.execute(`SET @parm = '{
      "query": {
        "bool": {
          "should": [
            {"match": {"query": "hi hello"}},
            {"match": { "content": "oceanbase mysql" }}
          ]
        }
      },
       "knn" : {
          "field": "vector",
          "k": 5,
          "query_vector": [1,2,3]
      },
      "_source" : ["query", "content", "_keyword_score", "_semantic_score"]
    }'`);
  await connection.commit();
  await cursor.execute(`SELECT json_pretty(DBMS_HYBRID_SEARCH.SEARCH('doc_table', @parm))`);

  const rows = await cursor.fetchAll();
  assert.equal(rows.length, 1);
  assert.ok(typeof rows[0][0] === 'string');

  assert.equal(await cursor.fetchOne(), null);

  await cursor.close();
  await connection.close();
  await instance.close();
  assert.equal(instance.closed, true);
});

test('types: number, bigint, string, null mapping', async () => {
  const instance = await seekdb.open(dbDir);
  const connection = await instance.connect('test', true);
  const cursor = connection.cursor();

  await cursor.execute('drop table if exists types_table');
  await cursor.execute(`create table types_table(c1 int,
                                c2 bigint,
                                c3 double,
                                c4 varchar(64))`);
  await cursor.execute(`insert into types_table values
                      (42, 9007199254740993, 3.5, 'hello'),
                      (null, null, null, null)`);

  await cursor.execute('select c1, c2, c3, c4 from types_table order by c1 desc');
  const rows = await cursor.fetchAll();
  assert.equal(rows.length, 2);

  // int -> number
  assert.equal(rows[0][0], 42);
  // bigint beyond Number.MAX_SAFE_INTEGER -> bigint
  assert.equal(rows[0][1], 9007199254740993n);
  // double -> number
  assert.equal(rows[0][2], 3.5);
  // varchar -> string
  assert.equal(rows[0][3], 'hello');

  // SQL NULL -> null
  assert.deepEqual(rows[1], [null, null, null, null]);

  await cursor.close();
  await connection.close();
  await instance.close();
  assert.equal(connection.closed, true);
  assert.equal(cursor.closed, true);
});

test('module-level default instance and transaction APIs', async () => {
  const instance = await seekdb.open(dbDir);
  assert.equal(instance, await seekdb.open(dbDir), 'second open reuses shared instance');

  const options = await seekdb.connectionOptions();
  assert.equal(options.user, 'root');

  const connection = await seekdb.connect('test', true);
  await connection.begin();
  await connection.rollback();
  await connection.commit();

  await seekdb.close();
  assert.equal(instance.closed, true);
  await seekdb.close(); // no-op after default instance is closed
});

test('SeekdbError is raised with a code', async () => {
  const instance = await seekdb.open(dbDir);
  const connection = await instance.connect('test', true);
  const cursor = connection.cursor();

  await assert.rejects(cursor.execute('select * from no_such_table'), (err) => {
    assert.ok(err instanceof seekdb.SeekdbError);
    assert.ok(typeof err.code === 'number');
    assert.ok(err.message.length > 0);
    return true;
  });

  await cursor.close();
  await connection.close();
  await instance.close();
});