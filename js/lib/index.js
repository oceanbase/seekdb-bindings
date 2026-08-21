'use strict';

// Node.js bindings for the seekdb C client library.
//
// The native addon (built by node-gyp) is wrapped below: every blocking call
// (open/close/connect/trx/execute/fetch*) runs on a libuv worker thread and
// returns a Promise; only pure in-memory operations (cursor(), closed, dbDir)
// are synchronous. All failures are normalized to SeekdbError, and Cursor
// operations are serialized.

const binding = require('../build/Release/seekdb.node');

/** Error raised by seekdb operations. Carries the server `code`. */
class SeekdbError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'SeekdbError';
    this.code = code;
  }
}

function normalizeError(err) {
  if (err instanceof SeekdbError) return err;
  const normalized = new SeekdbError(
    err && typeof err.code === 'number' ? err.code : undefined,
    (err && err.message) || String(err),
  );
  if (err && err.stack) normalized.stack = err.stack;
  return normalized;
}

class SeekdbInstance {
  constructor(native) {
    this._native = native;
  }

  connect(database = 'test', autocommit = false) {
    return this._native
      .connect(database, autocommit)
      .then((connection) => new Connection(connection))
      .catch(rethrow);
  }

  connectionOptions() {
    return this._native.connectionOptions().catch(rethrow);
  }

  close() {
    return this._native.close().catch(rethrow);
  }

  get closed() {
    return this._native.closed;
  }

  get dbDir() {
    return this._native.dbDir;
  }
}

// Rejection handler: rethrows so `.catch()` keeps rejecting with a
// normalized SeekdbError instead of fulfilling with it.
function rethrow(err) {
  throw normalizeError(err);
}

class Connection {
  constructor(native) {
    this._native = native;
  }

  cursor() {
    return new Cursor(this._native.cursor());
  }

  begin() {
    return this._native.begin().catch(rethrow);
  }

  commit() {
    return this._native.commit().catch(rethrow);
  }

  rollback() {
    return this._native.rollback().catch(rethrow);
  }

  close() {
    return this._native.close().catch(rethrow);
  }

  get closed() {
    return this._native.closed;
  }
}

// Serialize execute/fetch* on one cursor: they replace or advance the shared
// native result set, so concurrent calls would interleave rows. Chaining keeps
// them strictly ordered even when an earlier call rejects.
class Cursor {
  constructor(native) {
    this._native = native;
    this._chain = Promise.resolve();
  }

  _enqueue(task) {
    const next = this._chain.then(task);
    this._chain = next.catch(() => {});
    return next;
  }

  execute(sql) {
    return this._enqueue(() => this._native.execute(sql).catch(rethrow));
  }

  fetchOne() {
    return this._enqueue(() => this._native.fetchOne().catch(rethrow));
  }

  fetchAll() {
    return this._enqueue(() => this._native.fetchAll().catch(rethrow));
  }

  close() {
    return this._native.close().catch(rethrow);
  }

  get closed() {
    return this._native.closed;
  }
}

// ---------------------------------------------------------------------------
// Module-level default instance (mirrors the Python module API)
// ---------------------------------------------------------------------------

let defaultInstance = null;

/**
 * Open a seekdb instance rooted at dbDir and make it the module default.
 * @param {string} [dbDir='./seekdb.db']
 * @returns {Promise<SeekdbInstance>}
 */
function open(dbDir = './seekdb.db') {
  if (defaultInstance && !defaultInstance.closed && defaultInstance.dbDir === dbDir) {
    return Promise.resolve(defaultInstance);
  }
  defaultInstance = null;
  return binding.open(dbDir).then((instance) => {
    defaultInstance = new SeekdbInstance(instance);
    return defaultInstance;
  }, rethrow);
}

/**
 * Close the module-default instance, if any.
 * @returns {Promise<void>}
 */
function close() {
  if (!defaultInstance) return Promise.resolve();
  const instance = defaultInstance;
  defaultInstance = null;
  return instance.close();
}

/**
 * Connection options of the module-default instance.
 * @returns {Promise<{user: string, port?: number, unix_socket?: string}>}
 */
function connectionOptions() {
  if (!defaultInstance) {
    return Promise.reject(normalizeError(new SeekdbError(-2, 'seekdb not opened')));
  }
  return defaultInstance.connectionOptions();
}

/**
 * Connect the module-default instance.
 * @param {string} [database='test']
 * @param {boolean} [autocommit=false]
 * @returns {Promise<Connection>}
 */
function connect(database = 'test', autocommit = false) {
  if (!defaultInstance) {
    return Promise.reject(normalizeError(new SeekdbError(-2, 'seekdb not opened')));
  }
  return defaultInstance.connect(database, autocommit);
}

module.exports = {
  SeekdbInstance,
  Connection,
  Cursor,
  SeekdbError,
  open,
  close,
  connectionOptions,
  connect,
};