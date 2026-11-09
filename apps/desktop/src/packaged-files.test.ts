import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const root = fileURLToPath(new URL("..", import.meta.url));

async function sourceFiles(directory: string): Promise<string[]> {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = await Promise.all(entries.map((entry) => {
    const full = path.join(directory, entry.name);
    return entry.isDirectory() ? sourceFiles(full) : Promise.resolve([full]);
  }));
  return files.flat().filter((file) => file.endsWith(".ts") && !file.endsWith(".test.ts"));
}

describe("arquivos do pacote", () => {
  it("todo módulo importado pelo processo principal está em build.files", async () => {
    const pkg = JSON.parse(await readFile(path.join(root, "package.json"), "utf8")) as { build: { files: string[] } };
    const packaged = new Set(pkg.build.files);
    const missing = new Set<string>();
    for (const file of await sourceFiles(path.join(root, "src"))) {
      const relative = path.relative(root, file).split(path.sep).join("/").replace(/^src\//, "dist/").replace(/\.ts$/, ".js");
      if (!packaged.has(relative)) continue;
      const source = await readFile(file, "utf8");
      for (const match of source.matchAll(/from\s+"(\.[^"]+\.js)"/g)) {
        const target = path.posix.normalize(path.posix.join(path.posix.dirname(relative), match[1]!));
        if (!packaged.has(target)) missing.add(target);
      }
    }
    expect([...missing]).toEqual([]);
  });
});
