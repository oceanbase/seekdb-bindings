import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { bundleLinuxRuntime } from "./linux-runtime.mjs";

const packageDir = resolve(fileURLToPath(new URL("..", import.meta.url)));
const libDir = resolve(packageDir, "..", "build", "node-sdk");
const targetDir = join(packageDir, "dist", "native");
rmSync(targetDir, { recursive: true, force: true });
mkdirSync(targetDir, { recursive: true });
const platform = {
  platform: process.platform,
  arch: process.arch,
};

function findFile(directory, name) {
  for (const candidate of [join(directory, name), join(directory, "Release", name)]) {
    try {
      if (statSync(candidate).isFile()) return candidate;
    } catch {
      // Try the next build layout.
    }
  }
  throw new Error(`Missing ${name} under ${directory}`);
}

const libName = process.platform === "darwin" ? "libseekdb.dylib" :
  process.platform === "win32" ? "seekdb.dll" : "libseekdb.so";
const serverName = process.platform === "win32" ? "seekdb.exe" : "seekdb";
const nativeLibrary = findFile(libDir, libName);
const files = [
  findFile(join(packageDir, "build"), "seekdb_native.node"),
  nativeLibrary,
  findFile(libDir, serverName),
];
if (process.platform === "win32") {
  const runtimeDir = dirname(nativeLibrary);
  for (const name of readdirSync(runtimeDir).filter((name) => name.toLowerCase().endsWith(".dll"))) {
    if (name !== libName) files.push(join(runtimeDir, name));
  }
}
for (const source of files) {
  const destination = join(targetDir, basename(source));
  copyFileSync(source, destination);
  console.log(`Staged ${destination}`);
}

if (process.platform === "linux") {
  if (!process.report.getReport().header.glibcVersionRuntime) {
    throw new Error("Linux native packaging currently requires a glibc build environment");
  }
  platform.libc = "glibc";
  platform.minimumGlibc = bundleLinuxRuntime(files, targetDir);
}

if (process.platform === "darwin") {
  const scanned = new Set();
  const queue = files.map((source) => join(targetDir, basename(source)));
  while (queue.length > 0) {
    const file = queue.shift();
    if (scanned.has(file)) continue;
    scanned.add(file);
    const output = execFileSync("otool", ["-L", file], { encoding: "utf8" });
    for (const line of output.split("\n").slice(1)) {
      const dependency = line.match(/^\s+(.+?) \(compatibility version /)?.[1];
      if (!dependency || dependency.startsWith("/usr/lib/") ||
          dependency.startsWith("/System/Library/") || dependency.startsWith("@")) continue;
      const copied = join(targetDir, basename(dependency));
      if (file === copied) continue; // The dylib's own install name is set below.
      try {
        statSync(copied);
      } catch {
        copyFileSync(dependency, copied);
        queue.push(copied);
        console.log(`Staged ${copied}`);
      }
      execFileSync("install_name_tool", ["-change", dependency,
        `@loader_path/${basename(dependency)}`, file]);
    }
    if (file.endsWith(".dylib")) {
      execFileSync("install_name_tool", ["-id", `@loader_path/${basename(file)}`, file]);
    }
    execFileSync("codesign", ["--force", "--sign", "-", file], { stdio: "inherit" });
  }
}
writeFileSync(join(targetDir, "platform.json"), JSON.stringify(platform));
