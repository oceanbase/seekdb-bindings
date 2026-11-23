import { execFileSync } from "node:child_process";
import { readdirSync, statSync } from "node:fs";
import { isAbsolute, join } from "node:path";

export function classifyMacOSDependency(importer, dependency) {
  if (dependency.startsWith("/usr/lib/") ||
      dependency.startsWith("/System/Library/")) return "system";
  if (dependency.startsWith("@")) return "relative";
  if (isAbsolute(dependency)) return "external";
  throw new Error(`${importer} references unsupported macOS dependency ${dependency}`);
}

export function macOSDependencies(file) {
  const output = execFileSync("otool", ["-L", file], { encoding: "utf8" });
  const dependencies = output.split("\n").slice(1).flatMap((line) => {
    const match = line.match(/^\s+(.+?) \(compatibility version /);
    return match ? [match[1]] : [];
  });
  if (file.endsWith(".dylib")) {
    const installID = macOSInstallID(file);
    if (dependencies[0] !== installID) {
      throw new Error(`${file} has an unexpected macOS install ID ${installID}`);
    }
    return dependencies.slice(1);
  }
  return dependencies;
}

export function macOSInstallID(file) {
  const output = execFileSync("otool", ["-D", file], { encoding: "utf8" });
  const installID = output.split("\n").slice(1).map((line) => line.trim()).find(Boolean);
  if (!installID) throw new Error(`${file} has no macOS install ID`);
  return installID;
}

export function verifyStagedMacOSRuntime(directory) {
  for (const name of readdirSync(directory)) {
    if (name !== "seekdb" && !name.endsWith(".dylib") && !name.endsWith(".node")) continue;
    const file = join(directory, name);
    if (!statSync(file).isFile()) continue;
    if (name.endsWith(".dylib")) {
      const installID = macOSInstallID(file);
      if (installID !== `@loader_path/${name}` && installID !== `@rpath/${name}`) {
        throw new Error(`${file} has an unbundled macOS install ID ${installID}`);
      }
    }
    for (const dependency of macOSDependencies(file)) {
      const kind = classifyMacOSDependency(file, dependency);
      if (kind === "external") {
        throw new Error(`${file} retains an absolute macOS dependency ${dependency}`);
      }
    }
  }
}
