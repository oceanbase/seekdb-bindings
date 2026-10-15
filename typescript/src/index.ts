import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export interface OpenOptions {
  /** seekdb server parameters. Ordinary parameters apply on first initialization. */
  parameters?: Readonly<Record<string, string>>;
}

export type ConnectionOptions =
  | { transport: "tcp"; user: string; host: string; port: number }
  | { transport: "unix_socket"; user: string; unixSocket: string }
  | { transport: "named_pipe"; user: string; namedPipe: string };

interface NativeBinding {
  open(dbDir: string, parameters: string[]): Promise<object>;
  connectionOptions(handle: object): ConnectionOptions;
  close(handle: object): void;
}

const require = createRequire(import.meta.url);
const nativeDir = resolve(dirname(fileURLToPath(import.meta.url)), "native");
const builtFor = JSON.parse(readFileSync(resolve(nativeDir, "platform.json"), "utf8")) as {
  platform: string;
  arch: string;
  libc?: string;
};
if (builtFor.platform !== process.platform || builtFor.arch !== process.arch) {
  throw new Error(
    `seekdb native files target ${builtFor.platform}/${builtFor.arch}, ` +
    `but this runtime is ${process.platform}/${process.arch}`,
  );
}
if (builtFor.libc === "glibc") {
  const report = process.report.getReport() as { header?: { glibcVersionRuntime?: string } };
  if (!report.header?.glibcVersionRuntime) {
    throw new Error("This seekdb native package requires glibc; musl/Alpine is not supported");
  }
}
const native: NativeBinding = require(
  resolve(nativeDir, "seekdb_native.node"),
) as NativeBinding;

/** A failure reported by libseekdb. */
export class seekdbError extends Error {
  readonly returnCode: number;

  constructor(message: string, returnCode: number, options?: ErrorOptions) {
    super(message, options);
    this.name = "seekdbError";
    this.returnCode = returnCode;
  }
}

function parameterPairs(options?: OpenOptions): string[] {
  if (options === undefined) return [];
  if (options === null || typeof options !== "object") {
    throw new TypeError("options must be an object");
  }
  const parameters = options.parameters;
  if (parameters === undefined) return [];
  if (parameters === null || typeof parameters !== "object" || Array.isArray(parameters)) {
    throw new TypeError("parameters must be a record of string values");
  }
  const pairs: string[] = [];
  for (const [key, value] of Object.entries(parameters)) {
    if (!key || typeof value !== "string") {
      throw new TypeError("parameter keys must be nonempty and values must be strings");
    }
    pairs.push(key, value);
  }
  return pairs;
}

/** An open, local seekdb runtime. Close SQL clients before closing this handle. */
export class seekdb {
  readonly dbDir: string;
  #handle: object | undefined;

  private constructor(dbDir: string, handle: object) {
    this.dbDir = dbDir;
    this.#handle = handle;
  }

  static async open(dbDir: string, options?: OpenOptions): Promise<seekdb> {
    if (typeof dbDir !== "string" || dbDir.length === 0) {
      throw new TypeError("dbDir must be a nonempty string");
    }
    const pairs = parameterPairs(options);
    const normalized = resolve(dbDir);
    try {
      const handle = await native.open(normalized, pairs);
      return new seekdb(normalized, handle);
    } catch (error) {
      if (error instanceof Error && "returnCode" in error &&
          typeof error.returnCode === "number") {
        throw new seekdbError(error.message, error.returnCode, { cause: error });
      }
      throw error;
    }
  }

  get closed(): boolean {
    return this.#handle === undefined;
  }

  connectionOptions(): ConnectionOptions {
    if (this.#handle === undefined) {
      throw new Error("seekdb is closed");
    }
    return native.connectionOptions(this.#handle);
  }

  close(): void {
    if (this.#handle !== undefined) {
      native.close(this.#handle);
      this.#handle = undefined;
    }
  }
}
