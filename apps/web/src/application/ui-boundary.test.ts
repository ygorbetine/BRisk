import { readdir, readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

describe("fronteira React → Application Services", () => {
  it("não deixa views importarem adapters de rede/RTC nem construírem controllers", async () => {
    const sourceRoot = fileURLToPath(new URL("../", import.meta.url));
    const componentDirectory = fileURLToPath(new URL("../components/", import.meta.url));
    const viewDirectory = fileURLToPath(new URL("../views/", import.meta.url));
    const components = (await readdir(componentDirectory))
      .filter((name) => name.endsWith(".tsx"))
      .map((name) => `${componentDirectory}/${name}`);
    const views = (await readdir(viewDirectory))
      .filter((name) => name.endsWith(".tsx"))
      .map((name) => `${viewDirectory}/${name}`);
    const sources = await Promise.all([
      `${sourceRoot}/main.tsx`,
      `${sourceRoot}/App.tsx`,
      ...components,
      ...views,
    ].map((file) => readFile(file, "utf8")));

    for (const source of sources) {
      expect(source).not.toMatch(/from\s+["'][^"']*(?:\/api|\/supabase\/|@briks\/rtc)["']/);
      expect(source).not.toMatch(/new\s+(?:CallController|ChatController|MeshWebRTCTransport|SupabaseSignalingProvider)\s*\(/);
    }
  });
});
