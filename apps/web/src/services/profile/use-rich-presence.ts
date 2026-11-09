import { useEffect, useRef, useState } from "react";
import { isIgnored, readForeground, shrinkIcon, type RichApp, type RichPresenceSettings } from "./rich-presence";

const POLL_MS = 10_000;
const iconCache = new Map<string, Promise<string | undefined>>();

async function iconFor(path: string | undefined): Promise<string | undefined> {
  if (!path || !window.desktop?.getAppIcon) return undefined;
  let pending = iconCache.get(path);
  if (!pending) {
    pending = window.desktop.getAppIcon(path).then((raw) => (raw ? shrinkIcon(raw) : undefined)).catch(() => undefined);
    iconCache.set(path, pending);
  }
  return pending;
}

/**
 * Sonda o programa em primeiro plano a cada 10 s e só publica depois de duas leituras iguais
 * (trocar de janela rapidinho não muda o que os amigos veem). Desligado, publica `null`.
 * Devolve o último programa detectado (mesmo ignorado), para a tela de configurações oferecer "ignorar".
 */
export function useRichPresence(settings: RichPresenceSettings, publish: (app: RichApp | null) => void): { detected: string | null } {
  const [detected, setDetected] = useState<string | null>(null);
  const publishRef = useRef(publish);
  publishRef.current = publish;

  useEffect(() => {
    if (!settings.enabled) {
      setDetected(null);
      publishRef.current(null);
      return undefined;
    }
    let alive = true;
    let candidate: string | null = null;
    let published: string | null | undefined;
    const tick = async () => {
      const current = await readForeground();
      if (!alive) return;
      const name = current?.name ?? null;
      setDetected(name);
      const stable = name === candidate;
      candidate = name;
      if (!stable) return;
      const visible = current && !isIgnored(settings, current.name) ? current : null;
      const key = visible?.name ?? null;
      if (key === published) return;
      published = key;
      if (!visible) { publishRef.current(null); return; }
      const icon = await iconFor(visible.path);
      if (!alive || published !== key) return;
      publishRef.current({ name: visible.name, label: visible.label, ...(icon ? { icon } : {}) });
    };
    void tick();
    const timer = window.setInterval(() => void tick(), POLL_MS);
    return () => { alive = false; window.clearInterval(timer); };
  }, [settings]);

  return { detected };
}
