import { execFileSync } from "node:child_process";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { compilerEnvironment } from "./compiler-env.mjs";

const packageDir = resolve(fileURLToPath(new URL("..", import.meta.url)));
const nodeGyp = resolve(packageDir, "node_modules", "node-gyp", "bin", "node-gyp.js");
execFileSync(process.execPath, [nodeGyp, "rebuild"], {
  cwd: packageDir,
  env: compilerEnvironment(),
  stdio: "inherit",
});
