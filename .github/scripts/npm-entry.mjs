// The platform packages contain the original tested JS API, addon and runtime.
const packages = {
  "linux/x64": "@oceanbase/seekdb-node-linux-x64-gnu",
  "linux/arm64": "@oceanbase/seekdb-node-linux-arm64-gnu",
  "darwin/arm64": "@oceanbase/seekdb-node-darwin-arm64",
  "win32/x64": "@oceanbase/seekdb-node-win32-x64",
};

const packageName = packages[`${process.platform}/${process.arch}`];
if (!packageName) {
  throw new Error(`seekdb does not support ${process.platform}/${process.arch}`);
}
if (process.platform === "linux" && !process.report.getReport().header.glibcVersionRuntime) {
  throw new Error("seekdb requires glibc; musl/Alpine is not supported");
}

let implementation;
try {
  implementation = await import(packageName);
} catch (error) {
  throw new Error(`Cannot load ${packageName}; reinstall @oceanbase/seekdb-node with optional dependencies enabled`,
    { cause: error });
}

export const seekdb = implementation.seekdb;
export const seekdbError = implementation.seekdbError;
