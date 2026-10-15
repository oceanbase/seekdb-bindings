import { execFileSync } from "node:child_process";
import { accessSync, constants } from "node:fs";
import { delimiter, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { compilerEnvironment } from "./compiler-env.mjs";

const packageDir = resolve(fileURLToPath(new URL("..", import.meta.url)));
const rootDir = resolve(packageDir, "..");
const buildDir = join(rootDir, "build", "node-sdk");

function executable(name) {
  for (const directory of (process.env.PATH ?? "").split(delimiter)) {
    const candidate = join(directory, name);
    try {
      accessSync(candidate, constants.X_OK);
      return resolve(candidate);
    } catch {
      // Try the next PATH entry.
    }
  }
  throw new Error(`Cannot find ${name} on PATH; set SEEKDB_BIN to the server executable`);
}

const binary = resolve(process.env.SEEKDB_BIN || executable(process.platform === "win32" ? "seekdb.exe" : "seekdb"));
accessSync(binary, constants.X_OK);

const args = [
  "-S", rootDir,
  "-B", buildDir,
  "-DSEEKDB_BUILD_PYTHON=OFF",
  "-DBUILD_TESTING=OFF",
  `-DSEEKDB_BIN=${binary}`,
];
if (process.platform === "darwin") args.push("-DWITH_EXTERNAL_ZLIB=YES");
const options = { stdio: "inherit", env: compilerEnvironment() };
execFileSync("cmake", args, options);
execFileSync("cmake", ["--build", buildDir, "--target", "seekdb", "--config", "Release", "--parallel"], options);
