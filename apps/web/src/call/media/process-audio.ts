import workletUrl from "./process-pcm-worklet.js?url";

/**
 * Captura de áudio por processo (Windows). O sidecar entrega PCM f32le/48 kHz/estéreo
 * em streaming; aqui ele vira uma MediaStreamTrack para ser publicada com a tela.
 */

export type ProcessAudioRequest =
  | { mode: "window"; hwnd: number }
  | { mode: "screen"; excludeNames?: string[] };

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
