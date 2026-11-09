import workletUrl from "./process-pcm-worklet.js?url";

/**
 * Captura de áudio por processo (Windows). O sidecar entrega PCM f32le/48 kHz/estéreo
 * em streaming; aqui ele vira uma MediaStreamTrack para ser publicada com a tela.
 */

export type ProcessAudioRequest =
  | { mode: "window"; hwnd: number }
  | { mode: "screen"; excludeNames?: string[] }
  | { mode: "apps"; includeNames: string[] };

/** App com sessão de áudio aberta, devolvido pelo backend. `name` é o executável em minúsculas. */
export type AudioApp = { name: string; label: string; playing: boolean; path?: string };

/** Valida a resposta do backend: ignora itens malformados e limita o tamanho dos textos. */
export function parseAudioApps(value: unknown): AudioApp[] {
  if (!Array.isArray(value)) return [];
  const apps: AudioApp[] = [];
  for (const item of value.slice(0, 64)) {
    if (!item || typeof item !== "object") continue;
    const { name, label, playing, path } = item as Record<string, unknown>;
    if (typeof name !== "string" || !name.trim() || name.length > 128) continue;
    apps.push({
      name: name.trim().toLowerCase(),
      label: typeof label === "string" && label.trim() ? label.trim().slice(0, 60) : name,
      playing: playing === true,
      ...(typeof path === "string" && path.length > 0 && path.length <= 1_024 ? { path } : {}),
    });
  }
  return apps;
}

/** Lista os apps com sessão de áudio (Windows). Nunca lança: sem backend ou com erro, devolve lista vazia. */
export async function listAudioApps(): Promise<AudioApp[]> {
  if (!window.desktop?.getBackendConfig) return [];
  try {
    const config = await window.desktop.getBackendConfig();
    const response = await fetch(`${config.baseUrl.replace(/\/$/, "")}/screen-audio/apps`, {
      headers: { "x-briks-desktop-token": config.token },
    });
    if (!response.ok) return [];
    return parseAudioApps(await response.json());
  } catch {
    return [];
  }
}

/** Decide o que capturar: apps escolhidos têm prioridade; sem eles, o áudio segue a fonte de vídeo. */
export function resolveProcessAudioRequest(sourceId: string, audioApps?: string[]): ProcessAudioRequest | null {
  const names = (audioApps ?? []).map((name) => name.trim().toLowerCase()).filter(Boolean);
  if (names.length > 0) return { mode: "apps", includeNames: names };
  return processAudioRequestForSource(sourceId);
}

export type ProcessAudioCapture = {
  track: MediaStreamTrack;
  stop(): Promise<void>;
};

const BYTES_PER_FRAME = 8; // 2 canais × float32

/** Extrai o HWND de ids do Electron no formato "window:12345:0". */
export function hwndFromSourceId(sourceId: string): number | null {
  const match = /^window:(\d+):/.exec(sourceId);
  if (!match) return null;
  const value = Number(match[1]);
  return Number.isSafeInteger(value) && value > 0 ? value : null;
}

export function processAudioRequestForSource(sourceId: string): ProcessAudioRequest | null {
  if (sourceId.startsWith("screen:")) return { mode: "screen" };
  const hwnd = hwndFromSourceId(sourceId);
  return hwnd ? { mode: "window", hwnd } : null;
}

export function isWindowsDesktop(): boolean {
  return typeof window !== "undefined"
    && Boolean(window.desktop?.getBackendConfig)
    && /Windows/i.test(navigator.userAgent);
}

export async function startProcessAudioCapture(
  request: ProcessAudioRequest,
): Promise<ProcessAudioCapture | null> {
  if (!window.desktop?.getBackendConfig) return null;
  const config = await window.desktop.getBackendConfig();
  const controller = new AbortController();

  let response: Response;
  try {
    response = await fetch(`${config.baseUrl.replace(/\/$/, "")}/screen-audio/process-capture`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-briks-desktop-token": config.token,
      },
      body: JSON.stringify(request),
      signal: controller.signal,
    });
  } catch (error) {
    console.warn("Captura de áudio por processo indisponível.", error);
    return null;
  }
  if (!response.ok || !response.body) {
    const detail = await response.text().catch(() => "");
    console.warn("Captura de áudio por processo recusada.", response.status, detail);
    controller.abort();
    return null;
  }

  const context = new AudioContext({ sampleRate: 48_000, latencyHint: "interactive" });
  let node: AudioWorkletNode;
  let destination: MediaStreamAudioDestinationNode;
  try {
    await context.audioWorklet.addModule(workletUrl);
    node = new AudioWorkletNode(context, "process-pcm", { numberOfInputs: 0, outputChannelCount: [2] });
    destination = context.createMediaStreamDestination();
    node.connect(destination);
  } catch (error) {
    console.warn("Não foi possível preparar o áudio por processo.", error);
    controller.abort();
    await context.close().catch(() => undefined);
    return null;
  }

  const track = destination.stream.getAudioTracks()[0];
  if (!track) {
    controller.abort();
    await context.close().catch(() => undefined);
    return null;
  }
  try { track.contentHint = "music"; } catch { /* opcional */ }

  const reader = response.body.getReader();
  let stopped = false;
  void (async () => {
    let carry = new Uint8Array(0);
    try {
      while (!stopped) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value?.length) continue;
        const merged = new Uint8Array(carry.length + value.length);
        merged.set(carry, 0);
        merged.set(value, carry.length);
        const usable = merged.length - (merged.length % BYTES_PER_FRAME);
        carry = merged.slice(usable);
        if (usable === 0) continue;
        const samples = new Float32Array(merged.buffer.slice(0, usable));
        node.port.postMessage(samples, [samples.buffer]);
      }
    } catch {
      // Abort ao parar o compartilhamento.
    }
  })();

  return {
    track,
    async stop() {
      if (stopped) return;
      stopped = true;
      controller.abort();
      await reader.cancel().catch(() => undefined);
      track.stop();
      node.disconnect();
      await context.close().catch(() => undefined);
    },
  };
}
