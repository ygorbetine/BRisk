import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const executable = process.argv[2]?.trim();
if (!executable) {
  console.error("Uso: pnpm verify:packaged-two-instances <executável>");
  process.exit(1);
}

const script = fileURLToPath(new URL("./verify-packaged-renderer-origin.mjs", import.meta.url));
const run = () => new Promise((resolve, reject) => {
  const child = spawn(process.execPath, [script, executable], { stdio: "inherit", windowsHide: true });
  child.once("error", reject);
  child.once("exit", (code) => code === 0 ? resolve() : reject(new Error(`Instância empacotada encerrou com ${code}`)));
});

await Promise.all([run(), run()]);
console.log("Duas instâncias isoladas do Briks iniciaram e renderizaram corretamente.");
