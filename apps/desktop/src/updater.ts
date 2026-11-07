import { app, dialog, type BrowserWindow } from "electron";
import electronUpdater from "electron-updater";

const CHECK_INTERVAL_MS = 4 * 60 * 60 * 1000;

/**
 * Atualização automática pelo GitHub Releases: baixa em segundo plano e, quando pronta,
 * pergunta se quer reiniciar agora. Só roda no app instalado (empacotado) e pode ser
 * desligada com BRIKS_DISABLE_UPDATE=1.
 */
export function startAutoUpdate(getWindow: () => BrowserWindow | undefined, beforeInstall: () => void): void {
  if (!app.isPackaged || process.env.BRIKS_DISABLE_UPDATE === "1") return;
  const { autoUpdater } = electronUpdater;
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.allowPrerelease = false;

  autoUpdater.on("error", (error) => console.warn("[updater] falha ao verificar atualização:", error?.message ?? error));
  autoUpdater.on("update-downloaded", (info) => {
    const window = getWindow();
    const options = {
      type: "info" as const,
      buttons: ["Reiniciar agora", "Depois"],
      defaultId: 0,
      cancelId: 1,
      title: "Atualização do Briks",
      message: `A versão ${info.version} está pronta.`,
      detail: "Reinicie para instalar. Se escolher depois, ela é instalada quando você fechar o Briks.",
    };
    void (window && !window.isDestroyed() ? dialog.showMessageBox(window, options) : dialog.showMessageBox(options)).then(({ response }) => {
      if (response === 0) {
        beforeInstall();
        autoUpdater.quitAndInstall(false, true);
      }
    });
  });

  const check = () => { void autoUpdater.checkForUpdates().catch(() => undefined); };
  setTimeout(check, 15_000);
  setInterval(check, CHECK_INTERVAL_MS).unref();
}

export type UpdateCheckOutcome = { status: "disabled" | "current" | "available" | "error"; version?: string; message?: string };

/** Verificação manual (botão em Sobre). A baixada continua automática e o aviso de reiniciar aparece ao terminar. */
export async function checkForUpdatesNow(): Promise<UpdateCheckOutcome> {
  if (!app.isPackaged || process.env.BRIKS_DISABLE_UPDATE === "1") {
    return { status: "disabled", message: "A atualização só funciona no app instalado." };
  }
  try {
    const result = await electronUpdater.autoUpdater.checkForUpdates();
    const latest = result?.updateInfo?.version;
    if (latest && latest !== app.getVersion()) return { status: "available", version: latest };
    return { status: "current", version: app.getVersion() };
  } catch (error) {
    return { status: "error", message: error instanceof Error ? error.message : "Falha ao verificar." };
  }
}
