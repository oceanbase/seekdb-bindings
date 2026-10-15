import { accessSync, constants, readFileSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { verifyLinuxRuntime } from "./linux-runtime.mjs";

export function checkPackage(directory, maximumGlibc) {
  const required = ["dist/index.js", "dist/index.d.ts", "dist/native/platform.json",
    "dist/native/seekdb_native.node"];
  for (const name of required) {
    if (!statSync(join(directory, name)).isFile()) throw new Error(`Missing package file: ${name}`);
  }
  const nativeDir = join(directory, "dist", "native");
  const platform = JSON.parse(readFileSync(join(nativeDir, "platform.json"), "utf8"));
  if (platform.platform !== process.platform || platform.arch !== process.arch) {
    throw new Error("Package validation must run on the target OS and architecture");
  }
  const library = platform.platform === "darwin" ? "libseekdb.dylib" :
    platform.platform === "win32" ? "seekdb.dll" : "libseekdb.so";
  accessSync(join(nativeDir, library));
  accessSync(join(nativeDir, platform.platform === "win32" ? "seekdb.exe" : "seekdb"), constants.X_OK);
  if (platform.platform === "linux") {
    if (platform.libc !== "glibc") throw new Error("Rebuild the Linux package with libc metadata");
    platform.minimumGlibc = verifyLinuxRuntime(nativeDir, maximumGlibc);
  }
  return platform;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    checkPackage(fileURLToPath(new URL("..", import.meta.url)));
  } catch (error) {
    throw new Error(`Package validation failed; run npm run build first. ${error.message}`, { cause: error });
  }
}
