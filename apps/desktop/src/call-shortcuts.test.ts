import { describe, expect, it, vi } from "vitest";

vi.mock("electron", () => ({ globalShortcut: {}, ipcMain: {} }));
const { parseShortcutRequest } = await import("./call-shortcuts.js");

describe("atalhos globais da chamada", () => {
  it("aceita as três ações com aceleradores válidos", () => {
    expect(parseShortcutRequest({ mute: "Control+Shift+M", deafen: null, stream: "F9" })).toEqual({ mute: "Control+Shift+M", stream: "F9" });
  });
  it("recusa ação desconhecida, formato estranho e letra sem modificador", () => {
    expect(parseShortcutRequest({ quit: "Control+Q" })).toBeNull();
    expect(parseShortcutRequest({ mute: "Control+M\nrm" })).toBeNull();
    expect(parseShortcutRequest({ mute: "M" })).toBeNull();
    expect(parseShortcutRequest({ mute: "Shift+M" })).toBeNull();
    expect(parseShortcutRequest(["mute"])).toBeNull();
    expect(parseShortcutRequest(null)).toBeNull();
  });
});
