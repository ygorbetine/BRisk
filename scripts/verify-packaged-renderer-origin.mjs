import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

const executableArgument = process.argv[2]?.trim();
if (!executableArgument) {
  console.error("Uso: pnpm verify:packaged-origin <caminho-do-executável-empacotado>");
  process.exit(1);
}

const executable = path.resolve(executableArgument);
const temporaryDirectory = await mkdtemp(path.join(tmpdir(), "briks-packaged-origin-"));
const reportFile = path.join(temporaryDirectory, "origin.json");
const userDataDirectory = path.join(temporaryDirectory, "profile");
const args = [`--user-data-dir=${userDataDirectory}`];
// O diretório linux-unpacked do CI não instala chrome-sandbox como root:4755.
// O bypass vale somente para este processo efêmero de smoke test.
const linuxNeedsSandboxBypass = process.platform === "linux"
  && (process.env.CI === "true" || (typeof process.getuid === "function" && process.getuid() === 0));
if (linuxNeedsSandboxBypass) {
  args.push("--no-sandbox");
}

let stdout = "";
let stderr = "";
let timedOut = false;
let timeout;

try {
  const child = spawn(executable, args, {
    env: {
      ...process.env,
      BRIKS_PACKAGED_ORIGIN_REPORT_FILE: reportFile,
    },
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  child.stdout?.setEncoding("utf8");
  child.stderr?.setEncoding("utf8");
  child.stdout?.on("data", (chunk) => { stdout += chunk; });
  child.stderr?.on("data", (chunk) => { stderr += chunk; });

  timeout = setTimeout(() => {
    timedOut = true;
    child.kill();
  }, 45_000);
  const result = await new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve({ code, signal }));
  });
  clearTimeout(timeout);
  timeout = undefined;

  if (timedOut) throw new Error("O aplicativo empacotado não respondeu dentro de 45 segundos.");
  if (result.code !== 0) {
    throw new Error(`O aplicativo empacotado encerrou com code=${result.code ?? "?"}, signal=${result.signal ?? "?"}.`);
  }

  const report = JSON.parse(await readFile(reportFile, "utf8"));
  if (report.packaged !== true) throw new Error("O executável testado não está sendo reconhecido como empacotado pelo Electron.");
  if (report.origin !== "briks://app") throw new Error(`Origem inesperada: ${String(report.origin)}.`);
  if (typeof report.href !== "string" || !report.href.startsWith("briks://app/")) {
    throw new Error(`URL empacotada inesperada: ${String(report.href)}.`);
  }
  if (!Number.isInteger(report.rootChildren) || report.rootChildren < 1 || !Number.isInteger(report.rootTextLength) || report.rootTextLength < 1) {
    throw new Error("A origem foi carregada, mas o React não renderizou conteúdo. Possível tela preta no pacote.");
  }
  if (report.backendLoopback !== true || report.backendHealth !== true) {
    throw new Error("O sidecar Rust empacotado não iniciou saudável no loopback autenticado.");
  }
  if (report.storageWritable !== true || report.indexedDbWritable !== true) {
    throw new Error("A origem empacotada não conseguiu gravar localStorage/IndexedDB; a identidade P2P não seria persistente.");
  }
  console.log(`Renderer, sidecar e armazenamento local validados em ${report.origin} (${report.href}).`);
} catch (error) {
  if (stdout.trim()) console.error(`stdout do aplicativo:\n${stdout.trim()}`);
  if (stderr.trim()) console.error(`stderr do aplicativo:\n${stderr.trim()}`);
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
} finally {
  if (timeout) clearTimeout(timeout);
  await rm(temporaryDirectory, { recursive: true, force: true });
}
