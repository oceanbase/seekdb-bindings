import { execFileSync } from "node:child_process";
import { closeSync, copyFileSync, openSync, readFileSync, readdirSync, readSync,
  realpathSync } from "node:fs";
import { basename, delimiter, dirname, join, sep } from "node:path";

// These libraries must come from the target system's glibc installation.
const glibcRuntime = /^(?:ld-linux.*|ld64.*|lib(?:c|m|mvec|dl|pthread|rt|resolv|util|anl|BrokenLocale|nss_[\w-]+|thread_db|c_malloc_debug)\.so(?:\..*)?)$/;

function run(command, args, env = process.env) {
  try {
    return execFileSync(command, args, { encoding: "utf8", env: { ...env, LC_ALL: "C" },
      stdio: ["ignore", "pipe", "pipe"] });
  } catch (error) {
    throw new Error(`${command} failed for ${args.at(-1)}: ${error.stderr || error.stdout || error.message}`,
      { cause: error });
  }
}

function elfFiles(directory) {
  return readdirSync(directory).filter((name) => {
    const fd = openSync(join(directory, name), "r");
    try {
      const magic = Buffer.alloc(4);
      return readSync(fd, magic, 0, 4, 0) === 4 && magic.equals(Buffer.from([0x7f, 69, 76, 70]));
    } finally {
      closeSync(fd);
    }
  }).map((name) => join(directory, name));
}

function dependencies(file, env) {
  const dynamic = run("readelf", ["-d", file]);
  if (!dynamic.includes("(NEEDED)")) return [];
  // A DT_NEEDED entry containing '/' bypasses RPATH. ldd prints these entries
  // without '=>', so ignoring them would let host-only dependencies pass checks.
  for (const match of dynamic.matchAll(/\(NEEDED\).*Shared library: \[([^\]]+)\]/g)) {
    if (match[1].includes("/")) {
      throw new Error(`${file}: path-based ELF dependency ${match[1]}; relink using a SONAME before packaging`);
    }
  }
  const output = run("ldd", [file], env);
  if (/=>\s+not found/.test(output)) throw new Error(`Unresolved dependency of ${file}:\n${output}`);
  return output.split("\n").flatMap((line) => {
    const match = line.match(/^\s*(\S+)\s+=>\s+(\/.*?)\s+\(0x[\da-f]+\)/i);
    return match ? [{ name: match[1], path: match[2] }] : [];
  });
}

function versionParts(version) {
  if (!/^\d+\.\d+(?:\.\d+)?$/.test(version)) throw new Error(`Invalid glibc version: ${version}`);
  return version.split(".").map(Number);
}

function newer(left, right) {
  const a = versionParts(left);
  const b = versionParts(right);
  for (let i = 0; i < Math.max(a.length, b.length); ++i) {
    if ((a[i] ?? 0) !== (b[i] ?? 0)) return (a[i] ?? 0) > (b[i] ?? 0);
  }
  return false;
}

export function verifyLinuxRuntime(directory, maximumGlibc) {
  if (maximumGlibc !== undefined) versionParts(maximumGlibc);
  const root = realpathSync(directory) + sep;
  const environment = { ...process.env };
  delete environment.LD_LIBRARY_PATH;
  delete environment.LD_PRELOAD;
  delete environment.LD_AUDIT;
  let minimumGlibc = "2.2.5";
  for (const file of elfFiles(directory)) {
    for (const dependency of dependencies(file, environment)) {
      if (!glibcRuntime.test(dependency.name) && !realpathSync(dependency.path).startsWith(root)) {
        throw new Error(`${file}: ${dependency.name} resolves outside the package: ${dependency.path}`);
      }
    }
    const versions = run("readelf", ["--version-info", file]);
    if (versions.includes("GLIBC_PRIVATE")) throw new Error(`${file} requires private glibc symbols`);
    for (const match of versions.matchAll(/\bGLIBC_(\d+(?:\.\d+)+)/g)) {
      if (newer(match[1], minimumGlibc)) minimumGlibc = match[1];
      if (maximumGlibc !== undefined && newer(match[1], maximumGlibc)) {
        throw new Error(`${basename(file)} requires GLIBC_${match[1]}, above glibc ${maximumGlibc}`);
      }
    }
  }
  return minimumGlibc;
}

export function bundleLinuxRuntime(sources, directory) {
  const copies = new Map(sources.map((file) => [basename(file), realpathSync(file)]));
  const environment = { ...process.env };
  // The addon uses $ORIGIN, but its build directory is separate from libseekdb's.
  const libraryDirs = sources.filter((file) => /\.so(?:\.|$)/.test(basename(file))).map(dirname);
  environment.LD_LIBRARY_PATH = [...new Set(libraryDirs), environment.LD_LIBRARY_PATH]
    .filter(Boolean).join(delimiter);
  const queue = [...sources];
  const seen = new Set();
  // Resolve against the original build layout before rewriting any RPATHs.
  while (queue.length > 0) {
    const file = queue.shift();
    if (seen.has(realpathSync(file))) continue;
    seen.add(realpathSync(file));
    for (const dependency of dependencies(file, environment)) {
      if (glibcRuntime.test(dependency.name)) continue;
      const source = realpathSync(dependency.path);
      const previous = copies.get(dependency.name);
      if (previous !== undefined) {
        if (previous !== source && !readFileSync(previous).equals(readFileSync(source))) {
          throw new Error(`Different runtime libraries share the name ${dependency.name}`);
        }
        continue;
      }
      copies.set(dependency.name, source);
      copyFileSync(source, join(directory, dependency.name));
      queue.push(source);
      console.log(`Staged dependency ${dependency.name}`);
    }
  }
  for (const file of elfFiles(directory)) {
    if (!run("readelf", ["-d", file]).includes("There is no dynamic section")) {
      // Preserve large LOAD alignment (e.g. 2 MiB in some GCC runtimes).
      const headers = run("readelf", ["-lW", file]);
      const alignment = Math.max(4096, ...Array.from(
        headers.matchAll(/^\s*LOAD\b.*\s(0x[\da-f]+)\s*$/gmi), (match) => Number(match[1])));
      run("patchelf", ["--page-size", String(alignment), "--set-rpath", "$ORIGIN", file]);
    }
  }
  return verifyLinuxRuntime(directory);
}
