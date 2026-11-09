import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

describe("fronteiras do processo Electron", () => {
  it("mantém main.ts como composition root", async () => {
    const main = await readFile(fileURLToPath(new URL("./main.ts", import.meta.url)), "utf8");
    expect(main).not.toMatch(/desktopCapturer|protocol\.handle|new BrowserWindow|spawn\(/);
    expect(main).toContain("new DesktopBackendManager");
    expect(main).toContain("new ScreenCaptureController");
    expect(main).toContain("createMainWindow");
  });

  it("inclui todos os módulos compilados no pacote Electron", async () => {
    const manifest = JSON.parse(await readFile(fileURLToPath(new URL("../package.json", import.meta.url)), "utf8")) as {
      build?: { files?: string[] };
    };
    expect(manifest.build?.files).toEqual(expect.arrayContaining([
      "dist/backend.js",
      "dist/call-shortcuts.js",
      "dist/gpu.js",
      "dist/ipc/index.js",
      "dist/permissions.js",
      "dist/protocol.js",
      "dist/screen-capture.js",
      "dist/tray.js",
      "dist/window.js",
    ]));
  });
});
