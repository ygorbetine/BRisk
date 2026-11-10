import { readFile } from "node:fs/promises";

const tag = (process.env.GITHUB_REF_NAME ?? process.argv[2] ?? "").replace(/^v/, "");
const manifests = [
  "apps/web/package.json",
  "packages/protocol/package.json",
  "packages/rtc/package.json",
  "packages/shared/package.json",
  "packages/types/package.json",
];
const versions = await Promise.all(manifests.map(async (path) => {
  const manifest = JSON.parse(await readFile(new URL(`../${path}`, import.meta.url), "utf8"));
  return [path, manifest.version];
}));
const cargo = await readFile(new URL("../desktop-backend/Cargo.toml", import.meta.url), "utf8");
versions.push(["desktop-backend/Cargo.toml", cargo.match(/^version\s*=\s*"([^"]+)"/m)?.[1]]);
const mismatches = versions.filter(([, version]) => !tag || version !== tag);
if (mismatches.length) {
  console.error(`A tag (${tag || "ausente"}) precisa corresponder a todos os componentes ativos:`);
  mismatches.forEach(([path, version]) => console.error(`- ${path}: ${version ?? "versão ausente"}`));
  process.exit(1);
}
console.log(`Versão de release validada em ${versions.length} componentes: ${tag}.`);
