import { Menu, nativeImage, Tray } from "electron";

export type TrayActions = {
  show: () => void;
  restart: () => void;
  quit: () => void;
};

export function createApplicationTray(iconPath: string, actions: TrayActions): Tray | undefined {
  try {
    const source = nativeImage.createFromPath(iconPath);
    if (source.isEmpty()) throw new Error(`Ícone da bandeja não encontrado em ${iconPath}`);
    const size = process.platform === "darwin" ? 18 : 22;
    const tray = new Tray(source.resize({ width: size, height: size, quality: "best" }));
    tray.setToolTip("Briks");
    tray.setContextMenu(Menu.buildFromTemplate([
      { label: "Abrir app", click: actions.show },
      { label: "Reiniciar app", click: actions.restart },
      { type: "separator" },
      { label: "Fechar o app", click: actions.quit },
    ]));
    tray.on("click", actions.show);
    return tray;
  } catch (error) {
    console.error("Não foi possível criar o ícone da bandeja; fechar a janela encerrará o Briks.", error);
    return undefined;
  }
}

export function isTrayAvailable(tray: Tray | undefined): boolean {
  return Boolean(tray && !tray.isDestroyed());
}
