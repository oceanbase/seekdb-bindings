/**
 * Type definitions for the seekdb Node.js bindings.
 *
 * Value mapping (aligned with the Python bindings):
 * - SQL NULL        -> null
 * - INT64/UINT64    -> number, or bigint outside [-(2^53-1), 2^53-1]
 * - FLOAT           -> number
 * - DECIMAL         -> string
 * - DATE            -> "YYYY-MM-DD"
 * - DATETIME/TIMESTAMP -> "YYYY-MM-DD HH:MM:SS.ffffff"
 * - VARCHAR         -> string
 *
 * Every blocking operation (open/close/connect/begin/commit/rollback/
 * execute/fetchOne/fetchAll) returns a Promise and runs on a libuv worker
 * thread. Only pure in-memory accessors (cursor(), closed, dbDir) are
 * synchronous.
 */

export type Row = Array<number | bigint | string | null>;

export interface ConnectionOptions {
  user: string;
  /** present when transport is "tcp" */
  port?: number;
  /** present when transport is "unix_socket" */
  unix_socket?: string;
}

export class SeekdbError extends Error {
  name: 'SeekdbError';
  /** server errno, or a negative seekdb return code when no server error is available */
  code: number | undefined;
}

export class SeekdbInstance {
  private constructor();

  readonly closed: boolean;
  /** normalized absolute database directory */
  readonly dbDir: string;

  connect(database?: string, autocommit?: boolean): Promise<Connection>;
  connectionOptions(): Promise<ConnectionOptions>;
  close(): Promise<void>;
}

export class Connection {
  private constructor();

  readonly closed: boolean;

  cursor(): Cursor;
  begin(): Promise<void>;
  commit(): Promise<void>;
  rollback(): Promise<void>;
  close(): Promise<void>;
}

export class Cursor {
  private constructor();

  readonly closed: boolean;

  /** Run a statement and return the number of affected rows. */
  execute(sql: string): Promise<number>;
  /** Next row of the current result set, or null at end. */
  fetchOne(): Promise<Row | null>;
  /** All remaining rows of the current result set. */
  fetchAll(): Promise<Row[]>;
  close(): Promise<void>;
}

/**
 * Open a seekdb instance rooted at dbDir and make it the module default.
 */
export function open(dbDir?: string): Promise<SeekdbInstance>;

/** Close the module-default instance, if any. */
export function close(): Promise<void>;

export function connectionOptions(): Promise<ConnectionOptions>;

/** Connect the module-default instance. */
export function connect(database?: string, autocommit?: boolean): Promise<Connection>;