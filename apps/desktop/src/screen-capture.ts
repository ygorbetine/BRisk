import { BrowserWindow, desktopCapturer, dialog, ipcMain, session } from "electron";

type CapturableSource = Awaited<ReturnType<typeof desktopCapturer.getSources>>[number];
type PendingDisplaySelection = {
  id: string;
  name?: string;
  displayId?: string;
  source?: CapturableSource;
};

const WINDOWS_LOOPBACK_WITHOUT_BRIKS = "loopbackWithoutChrome";

export class ScreenCaptureController {
  private pendingSelection: PendingDisplaySelection | undefined;
  private readonly knownSources = new Map<string, PendingDisplaySelection>();

  constructor(private readonly isTrustedRendererUrl: (value: string) => boolean) {}

  register(): void {
    ipcMain.handle("screen:list", async (event) => {
      this.assertTrusted(event.sender.getURL());
      const sources = await desktopCapturer.getSources({
        types: ["screen", "window"],
        thumbnailSize: { width: 320, height: 180 },
        fetchWindowIcons: true,
      });
      this.knownSources.clear();
      sources.forEach((source) => this.rememberSource(source));
      return sources.map((source) => ({
        id: source.id,
        name: source.name,
        displayId: source.display_id,
        thumbnail: source.thumbnail.toDataURL(),
      }));
    });

    ipcMain.handle("screen:choose", async (event) => {
      this.assertTrusted(event.sender.getURL());
      const allSources = await desktopCapturer.getSources({
        types: ["screen", "window"],
        thumbnailSize: { width: 0, height: 0 },
        fetchWindowIcons: false,
      });
      if (!allSources.length) return null;

      // Em PipeWire o portal já realizou a escolha e retorna apenas aquela fonte.
      if (process.platform === "linux" && allSources.length === 1) {
        const selected = allSources[0]!;
        this.rememberSource(selected);
        return selected.id;
      }

      const sources = allSources.slice(0, 20);
      const buttons = [...sources.map((source) => source.name.slice(0, 80)), "Cancelar"];
      const owner = BrowserWindow.fromWebContents(event.sender);
      const options = {
        type: "question" as const,
        title: "Compartilhar tela",
        message: "Escolha uma tela ou janela para compartilhar",
        detail: allSources.length > sources.length
          ? `Mostrando as primeiras ${sources.length} fontes disponíveis.`
          : "O áudio do sistema será incluído quando o sistema operacional permitir.",
        buttons,
        defaultId: 0,
        cancelId: sources.length,
        noLink: true,
      };
      const result = owner
        ? await dialog.showMessageBox(owner, options)
        : await dialog.showMessageBox(options);
      if (result.response < 0 || result.response >= sources.length) return null;
      const selected = sources[result.response];
      if (!selected) return null;
      this.rememberSource(selected);
      return selected.id;
    });

    ipcMain.handle("screen:select", async (event, sourceId: unknown) => {
      this.assertTrusted(event.sender.getURL());
      if (typeof sourceId !== "string" || sourceId.length === 0 || sourceId.length > 512) {
        throw new Error("Fonte de compartilhamento inválida.");
      }
      const known = this.knownSources.get(sourceId);
      if (process.platform === "linux" && !known?.source) {
        throw new Error("A fonte PipeWire selecionada não está mais disponível. Abra o seletor novamente.");
      }
      this.pendingSelection = known ?? { id: sourceId };
    });

    session.defaultSession.setDisplayMediaRequestHandler(async (request, callback) => {
      const selection = this.pendingSelection;
      this.pendingSelection = undefined;
      if (!selection || !this.isTrustedRendererUrl(request.securityOrigin)) {
        callback({});
        return;
      }
      try {
        const selected = await this.resolveCurrentSource(selection);
        if (!selected) {
          console.warn("Fonte de compartilhamento desapareceu antes do getDisplayMedia", {
            id: selection.id,
            name: selection.name,
            displayId: selection.displayId,
          });
          callback({});
          return;
        }

        const audioDevice = process.platform === "win32" ? WINDOWS_LOOPBACK_WITHOUT_BRIKS : "loopback";
        if (request.audioRequested) {
          console.info(`[briks-screen-audio] Electron ${process.versions.electron}; device=${audioDevice}; source=${selected.name}`);
        }
        callback({
          video: selected,
          ...(request.audioRequested ? { audio: audioDevice as unknown as "loopback" } : {}),
        });
      } catch (error) {
        console.error("Falha ao autorizar compartilhamento de tela", error);
        callback({});
      }
    });
  }

  clear(): void {
    this.pendingSelection = undefined;
    this.knownSources.clear();
  }

  private assertTrusted(url: string): void {
    if (!this.isTrustedRendererUrl(url)) throw new Error("Origem do renderer não autorizada.");
  }

  private rememberSource(source: CapturableSource): void {
    this.knownSources.set(source.id, {
      id: source.id,
      name: source.name,
      displayId: source.display_id,
      source,
    });
  }

  private async resolveCurrentSource(selection: PendingDisplaySelection): Promise<CapturableSource | undefined> {
    // Reconsultar o portal PipeWire pode invalidar a sessão já selecionada.
    if (selection.source) return selection.source;
    if (process.platform === "linux") return undefined;

    for (let attempt = 0; attempt < 2; attempt += 1) {
      const sources = await desktopCapturer.getSources({
        types: ["screen", "window"],
        thumbnailSize: { width: 0, height: 0 },
        fetchWindowIcons: false,
      });
      const selected = sources.find((source) => source.id === selection.id)
        ?? (selection.displayId ? sources.find((source) => source.display_id === selection.displayId) : undefined);
      if (selected) return selected;
      if (attempt === 0) await new Promise((resolve) => setTimeout(resolve, 120));
    }
    return undefined;
  }
}
