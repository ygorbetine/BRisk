import { useEffect, useRef, useState } from "react";
import { loadShortcutSettings, watchShortcutSettings, type ShortcutAction, type ShortcutSettings } from "./shortcuts";

type Handlers = { mute(): void; deafen(): void; stream(): void; setMicrophone(enabled: boolean): void };

/** Espera a tecla do push-to-talk mudar de estado (long-poll no backend). `null` = backend indisponível. */
async function waitKey(vk: number, down: boolean, signal: AbortSignal): Promise<boolean | null> {
  const config = await window.desktop?.getBackendConfig?.();
  if (!config) return null;
  const response = await fetch(`${config.baseUrl.replace(/\/$/, "")}/input/key-wait?vk=${vk}&down=${down}`, { headers: { "x-briks-desktop-token": config.token }, signal });
  if (!response.ok) return null;
  const body = await response.json() as { down?: unknown };
  return body.down === true;
}

/**
 * Liga os atalhos globais e o push-to-talk enquanto a chamada está aberta.
 * Devolve as ações cujo atalho não pôde ser registrado (já em uso por outro programa) e se a tecla do PTT está apertada.
 */
export function useCallShortcuts(active: boolean, handlers: Handlers): { failed: ShortcutAction[]; talking: boolean } {
  const [settings, setSettings] = useState<ShortcutSettings>(loadShortcutSettings);
  const [failed, setFailed] = useState<ShortcutAction[]>([]);
  const [talking, setTalking] = useState(false);
  const handlersRef = useRef(handlers);
  handlersRef.current = handlers;

  useEffect(() => watchShortcutSettings(setSettings), []);

  useEffect(() => {
    if (!active || !window.desktop?.setCallShortcuts) return undefined;
    let alive = true;
    void window.desktop.setCallShortcuts(settings.accelerators)
      .then((result) => { if (alive) setFailed(result.failed.filter((item): item is ShortcutAction => item === "mute" || item === "deafen" || item === "stream")); })
      .catch(() => undefined);
    const off = window.desktop.onCallShortcut?.((action) => {
      if (action === "mute" && !settings.pushToTalk.enabled) handlersRef.current.mute();
      else if (action === "deafen") handlersRef.current.deafen();
      else if (action === "stream") handlersRef.current.stream();
    });
    return () => { alive = false; off?.(); void window.desktop?.setCallShortcuts?.({}).catch(() => undefined); };
  }, [active, settings.accelerators, settings.pushToTalk.enabled]);

  const ptt = settings.pushToTalk;
  useEffect(() => {
    if (!active || !ptt.enabled || !ptt.key || !window.desktop?.getBackendConfig) return undefined;
    const vk = ptt.key.vk;
    const controller = new AbortController();
    let releaseTimer: number | undefined;
    let down = false;
    let micOn = false;
    handlersRef.current.setMicrophone(false);
    const loop = async () => {
      while (!controller.signal.aborted) {
        const next = await waitKey(vk, down, controller.signal).catch(() => null);
        if (controller.signal.aborted) return;
        if (next === null) { await new Promise((resolve) => window.setTimeout(resolve, 1_000)); continue; }
        if (next === down) continue;
        down = next;
        setTalking(down);
        window.clearTimeout(releaseTimer);
        if (down) { micOn = true; handlersRef.current.setMicrophone(true); }
        // Pequeno atraso ao soltar para não cortar o fim da palavra.
        else releaseTimer = window.setTimeout(() => { micOn = false; handlersRef.current.setMicrophone(false); }, ptt.releaseDelayMs);
      }
    };
    void loop();
    return () => {
      controller.abort();
      window.clearTimeout(releaseTimer);
      setTalking(false);
      // Desligar o push-to-talk com a tecla apertada não pode deixar o microfone aberto.
      if (micOn) handlersRef.current.setMicrophone(false);
    };
  }, [active, ptt.enabled, ptt.key?.vk, ptt.releaseDelayMs]);

  return { failed, talking };
}
