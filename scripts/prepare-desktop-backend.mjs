import { spawnSync } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, rmSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const release = !process.argv.includes("--debug");
const profile = release ? "release" : "debug";
const executableName = process.platform === "win32" ? "briks-desktop-backend.exe" : "briks-desktop-backend";
const manifestPath = path.join(root, "desktop-backend", "Cargo.toml");
const source = path.join(root, "desktop-backend", "target", profile, executableName);
const destinationDir = path.join(root, "apps", "desktop-cui", "backend");
const destination = path.join(destinationDir, executableName);

const args = ["build", "--manifest-path", manifestPath];
if (release) args.push("--release");
const result = spawnSync("cargo", args, { cwd: root, stdio: "inherit", shell: false });
if (result.error) {
  console.error(`Falha ao iniciar cargo: ${result.error.message}`);
  process.exit(1);
}
if (result.status !== 0) process.exit(result.status ?? 1);
if (!existsSync(source)) {
  console.error(`Binário esperado não encontrado: ${source}`);
  process.exit(1);
}
rmSync(destinationDir, { recursive: true, force: true });
mkdirSync(destinationDir, { recursive: true });
copyFileSync(source, destination);
if (process.platform !== "win32") chmodSync(destination, 0o755);
console.log(`Backend desktop preparado em ${destination}`);
