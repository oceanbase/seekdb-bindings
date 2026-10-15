import { execFileSync } from "node:child_process";

// Honor caller-selected toolchains, including SDK selection via DEVELOPER_DIR.
export function compilerEnvironment(env = process.env, platform = process.platform,
  findCompiler = (name) => execFileSync("xcrun", ["--find", name], {
    env, encoding: "utf8",
  }).trim()) {
  const result = { ...env };
  if (platform === "darwin") {
    result.CC ||= findCompiler("clang");
    result.CXX ||= findCompiler("clang++");
  }
  return result;
}
