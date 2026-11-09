import { createHash } from "node:crypto";
import { execFile } from "node:child_process";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

const root = process.cwd();
// Migrações imutáveis: do backend local (SQLite) e do Supabase (Postgres). Cada pasta tem seu checksums.json.
const migrationDirectories = ["desktop-backend/migrations", "supabase/migrations"];
const errors = [];
let total = 0;

const baseArgumentIndex = process.argv.indexOf("--base");
const baseRevision = baseArgumentIndex >= 0 ? process.argv[baseArgumentIndex + 1]?.trim() : undefined;

for (const relativeDirectory of migrationDirectories) {
  const migrationsDirectory = path.join(root, relativeDirectory);
  const manifestPath = path.join(migrationsDirectory, "checksums.json");
  const expected = JSON.parse(await readFile(manifestPath, "utf8"));
  const files = (await readdir(migrationsDirectory)).filter((name) => /^\d{4}_.+\.sql$/.test(name)).sort();
  const expectedFiles = Object.keys(expected).sort();
  total += files.length;

  for (const missing of expectedFiles.filter((name) => !files.includes(name))) {
    errors.push(`${relativeDirectory}/${missing}: migração registrada não foi encontrada`);
  }
  for (const unregistered of files.filter((name) => !expectedFiles.includes(name))) {
    errors.push(`${relativeDirectory}/${unregistered}: migração nova precisa ser registrada em checksums.json`);
  }
  for (const filename of files.filter((name) => expectedFiles.includes(name))) {
    const digest = createHash("sha384").update(await readFile(path.join(migrationsDirectory, filename))).digest("hex");
    if (digest !== expected[filename]) errors.push(`${relativeDirectory}/${filename}: conteúdo de uma migração imutável foi alterado`);
  }

  if (baseRevision && !/^0+$/.test(baseRevision)) {
    try {
      const { stdout } = await execFileAsync("git", [
        "diff",
        "--name-status",
        "--find-renames",
        baseRevision,
        "HEAD",
        "--",
        relativeDirectory,
      ], { cwd: root });
      const pattern = new RegExp(`^${relativeDirectory.replace(/[/.]/g, "\\$&")}/\\d{4}_.+\\.sql$`);
      for (const line of stdout.split(/\r?\n/).filter(Boolean)) {
        const [status, source, destination] = line.split("\t");
        const target = destination ?? source;
        if (!target || !pattern.test(target)) continue;
        if (status !== "A") errors.push(`${target}: uma migração já publicada não pode ser alterada, removida ou renomeada`);
      }
    } catch (error) {
      errors.push(`não foi possível comparar migrações com a revisão base ${baseRevision}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
}

if (errors.length) {
  console.error("Integridade das migrações SQL falhou:\n- " + errors.join("\n- "));
  console.error("Não edite migrações publicadas. Crie o próximo arquivo numerado e registre seu SHA-384.");
  process.exit(1);
}
console.log(`${total} migrações SQL imutáveis validadas.`);
