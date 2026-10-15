import { execFileSync } from "node:child_process";
import { cpSync, copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { checkPackage } from "./check-package.mjs";

const { values } = parseArgs({ options: {
  "out-dir": { type: "string" },
  "glibc-baseline": { type: "string", default: "2.28" },
} });
if (process.platform !== "linux" || process.arch !== "x64") {
  throw new Error("pack:linux currently targets Linux x64 only");
}
const packageDir = fileURLToPath(new URL("..", import.meta.url));
const platform = checkPackage(packageDir, values["glibc-baseline"]);
const output = resolve(values["out-dir"] ?? join(packageDir, "build", "npm"));
mkdirSync(output, { recursive: true });
const staging = mkdtempSync(join(tmpdir(), "seekdb-node-pack-"));
try {
  const manifest = JSON.parse(readFileSync(join(packageDir, "package.json"), "utf8"));
  delete manifest.private;
  delete manifest.scripts;
  delete manifest.devDependencies;
  Object.assign(manifest, { os: ["linux"], cpu: ["x64"], libc: ["glibc"],
    publishConfig: { registry: "https://registry.npmjs.org", access: "public" } });
  writeFileSync(join(staging, "package.json"), JSON.stringify(manifest, null, 2) + "\n");
  cpSync(join(packageDir, "dist"), join(staging, "dist"), { recursive: true });
  writeFileSync(join(staging, "dist", "native", "platform.json"), JSON.stringify(platform));
  copyFileSync(join(packageDir, "README.md"), join(staging, "README.md"));
  copyFileSync(join(packageDir, "..", "LICENSE"), join(staging, "LICENSE"));
  const args = ["pack", "--ignore-scripts", "--json", "--pack-destination", output];
  const npm = process.env.npm_execpath;
  const result = execFileSync(npm ? process.execPath : "npm", npm ? [npm, ...args] : args,
    { cwd: staging, encoding: "utf8" });
  console.log(result);
} finally {
  rmSync(staging, { recursive: true, force: true });
}
