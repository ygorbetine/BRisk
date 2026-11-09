import { app, BrowserWindow, ipcMain, shell } from "electron";
import { isAbsolute } from "node:path";
import { networkInterfaces } from "node:os";
import type { DesktopBackendManager } from "../backend.js";
import { collectNetworkInterfaces } from "../network-interfaces.js";
import { checkForUpdatesNow, getUpdateState, installUpdateNow } from "../updater.js";

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

  // Ícone do executável de um app com áudio (seletor de áudio da transmissão). Só aceita .exe com caminho absoluto.
  ipcMain.handle("app:icon", async (event, value: unknown) => {
    assertTrusted(event.sender.getURL());
    if (typeof value !== "string" || value.length > 1_024 || !isAbsolute(value) || !/\.exe$/i.test(value)) return null;
    try {
      const image = await app.getFileIcon(value, { size: "normal" });
      return image.isEmpty() ? null : image.toDataURL();
    } catch {
      return null;
    }
  });

  // Abre o teclado de emojis do sistema (Windows 10+) com o campo de texto atual em foco.
  ipcMain.handle("app:emoji-panel", async (event) => {
    assertTrusted(event.sender.getURL());
    if (typeof app.showEmojiPanel !== "function") return false;
    app.showEmojiPanel();
    return true;
  });

  ipcMain.handle("updater:check", async (event) => {
    assertTrusted(event.sender.getURL());
    return checkForUpdatesNow();
  });

  ipcMain.handle("updater:state", async (event) => {
    assertTrusted(event.sender.getURL());
    return getUpdateState();
  });

  ipcMain.handle("updater:install", async (event) => {
    assertTrusted(event.sender.getURL());
    return installUpdateNow();
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
