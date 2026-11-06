import { BrowserWindow, ipcMain, shell } from "electron";
import { networkInterfaces } from "node:os";
import type { DesktopBackendManager } from "../backend.js";
import { collectNetworkInterfaces } from "../network-interfaces.js";

export function registerDesktopIpc(
  backend: DesktopBackendManager,
  isTrustedRendererUrl: (value: string) => boolean,
): void {
  const assertTrusted = (url: string) => {
    if (!isTrustedRendererUrl(url)) throw new Error("Origem do renderer não autorizada.");
  };

  ipcMain.handle("backend:config", async (event) => {
    assertTrusted(event.sender.getURL());
    const config = backend.getConfig();
    if (!config) throw new Error("Backend local ainda não está pronto.");
    return config;
  });

  ipcMain.handle("network:interfaces", async (event) => {
    assertTrusted(event.sender.getURL());
    return collectNetworkInterfaces(networkInterfaces());
  });

  ipcMain.handle("window:fullscreen", async (event, enabled: unknown) => {
    assertTrusted(event.sender.getURL());
    if (typeof enabled !== "boolean") throw new Error("Estado de tela cheia inválido.");
    const owner = BrowserWindow.fromWebContents(event.sender);
    if (!owner) throw new Error("Janela do Briks não encontrada.");
    owner.setFullScreen(enabled);
    if (enabled) owner.focus();
    return { fullscreen: enabled };
  });

  ipcMain.handle("shell:open-external", async (event, value: unknown) => {
    assertTrusted(event.sender.getURL());
    if (typeof value !== "string" || value.length > 2_048) throw new Error("Link externo inválido.");
    const url = new URL(value);
    if (url.protocol !== "https:" && url.protocol !== "http:") throw new Error("Protocolo de link não permitido.");
    await shell.openExternal(url.toString());
  });
}
