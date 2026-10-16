import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import test from "node:test";
import { compilerEnvironment } from "../scripts/compiler-env.mjs";
import { bundleLinuxRuntime, verifyLinuxRuntime } from "../scripts/linux-runtime.mjs";

test("explicit compilers and linker flags survive macOS compiler selection", () => {
  const env = { CC: "/custom/cc", CXX: "/custom/cxx", LDFLAGS: "-L/custom/lib" };
  assert.deepEqual(compilerEnvironment(env, "darwin", () => {
    throw new Error("Explicit compilers must not invoke xcrun");
  }), env);
  const found = [];
  assert.deepEqual(compilerEnvironment({ CC: "ccache clang", DEVELOPER_DIR: "/sdk" }, "darwin",
    (name) => { found.push(name); return `/sdk/${name}`; }), {
    CC: "ccache clang", CXX: "/sdk/clang++", DEVELOPER_DIR: "/sdk",
  });
  assert.deepEqual(found, ["clang++"]);
  assert.deepEqual(compilerEnvironment({ CC: "gcc", LDFLAGS: "-Wl,--as-needed" }, "linux"),
    { CC: "gcc", LDFLAGS: "-Wl,--as-needed" });
});

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), "seekdb-packaging-test-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const target = join(root, "native");
  mkdirSync(target);
  return { root, target };
}

function compile(args) {
  execFileSync("cc", ["-Wl,-z,max-page-size=0x200000", ...args], { stdio: "pipe" });
}

function executable(root, label, value) {
  const build = join(root, label);
  mkdirSync(build);
  writeFileSync(join(build, "leaf.c"), `int leaf(void) { return ${value}; }\n`);
  writeFileSync(join(build, "middle.c"), "extern int leaf(void); int middle(void) { return leaf(); }\n");
  writeFileSync(join(build, "main.c"),
    '#include <stdio.h>\nextern int middle(void); int main(void) { printf("%d\\n", middle()); return 0; }\n');
  compile(["-shared", "-fPIC", join(build, "leaf.c"), "-Wl,-soname,libseekdb_test_leaf.so",
    "-o", join(build, "libseekdb_test_leaf.so")]);
  compile(["-shared", "-fPIC", join(build, "middle.c"), `-L${build}`, "-lseekdb_test_leaf",
    `-Wl,-rpath,${build}`, "-Wl,-soname,libseekdb_test_middle.so",
    "-o", join(build, "libseekdb_test_middle.so")]);
  const binary = join(build, label);
  compile([join(build, "main.c"), `-L${build}`, "-lseekdb_test_middle", `-Wl,-rpath,${build}`,
    "-o", binary]);
  return { build, binary };
}

test("Linux runtime remains executable after the build libraries are removed", {
  skip: process.platform !== "linux",
}, (t) => {
  const { root, target } = fixture(t);
  const { build, binary } = executable(root, "server", 42);
  // Model node-gyp and CMake placing their outputs in separate build directories.
  const addonDir = join(root, "addon");
  mkdirSync(addonDir);
  const addon = join(addonDir, "server");
  copyFileSync(binary, addon);
  execFileSync("patchelf", ["--page-size", "2097152", "--set-rpath", "$ORIGIN", addon]);
  const sources = [addon, join(build, "libseekdb_test_middle.so")];
  for (const file of sources) copyFileSync(file, join(target, basename(file)));
  bundleLinuxRuntime(sources, target);
  rmSync(build, { recursive: true });
  const env = { ...process.env };
  delete env.LD_LIBRARY_PATH;
  assert.equal(execFileSync(join(target, "server"), { env, encoding: "utf8" }), "42\n");
  assert.equal(execFileSync("patchelf", ["--print-rpath", join(target, "libseekdb_test_middle.so")],
    { encoding: "utf8" }).trim(), "$ORIGIN");
  assert.throws(() => verifyLinuxRuntime(target, "2.0"), /above glibc 2\.0/);
});

test("Linux staging rejects an unresolved transitive dependency", {
  skip: process.platform !== "linux",
}, (t) => {
  const { root, target } = fixture(t);
  const { build, binary } = executable(root, "server", 42);
  copyFileSync(binary, join(target, "server"));
  rmSync(join(build, "libseekdb_test_leaf.so"));
  assert.throws(() => bundleLinuxRuntime([binary], target), /not found/);
});

test("Linux staging rejects different libraries with the same runtime name", {
  skip: process.platform !== "linux",
}, (t) => {
  const { root, target } = fixture(t);
  const first = executable(root, "first", 1);
  const second = executable(root, "second", 2);
  const sources = [first.binary, second.binary];
  for (const file of sources) copyFileSync(file, join(target, basename(file)));
  assert.throws(() => bundleLinuxRuntime(sources, target), /Different runtime libraries share the name/);
});

test("Linux staging and verification reject path-based DT_NEEDED entries", {
  skip: process.platform !== "linux",
}, (t) => {
  const { root, target } = fixture(t);
  const { build, binary } = executable(root, "server", 42);
  const library = "libseekdb_test_middle.so";
  for (const dependency of [join(build, library), `./${library}`]) {
    const staged = join(target, "server");
    copyFileSync(binary, staged);
    execFileSync("patchelf", ["--page-size", "2097152", "--replace-needed",
      library, dependency, staged]);
    assert.throws(() => bundleLinuxRuntime([staged], target), /path-based ELF dependency/);
    assert.throws(() => verifyLinuxRuntime(target, "2.28"), /path-based ELF dependency/);
  }
});
