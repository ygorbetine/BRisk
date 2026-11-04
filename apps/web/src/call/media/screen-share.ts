import type { ScreenShareProvider } from "@risk/rtc";
import {
  isWindowsDesktop,
  processAudioRequestForSource,
  startProcessAudioCapture,
  type ProcessAudioCapture,
} from "./process-audio";

export type DisplayAudioSettings = MediaTrackSettings & { restrictOwnAudio?: boolean };

export type DesktopScreenAudioPreparation = {
  mode: "display" | "pipewire" | "process" | "unavailable";
  sourceName?: string;
  sourceLabel?: string;
  excludedRisk: boolean;
  reason?: string;
};

export type ScreenCaptureStart = {
  stream: MediaStream;
  desktopAudio: DesktopScreenAudioPreparation | null;
  linuxAudioPreparation?: Promise<DesktopScreenAudioPreparation | null>;
};

export async function startScreenCapture(
  provider: ScreenShareProvider,
  sourceId: string | undefined,
  includeAudio: boolean,
  excludeRisk: boolean,
): Promise<ScreenCaptureStart> {
  const linuxDesktop = Boolean(window.desktop && /Linux/i.test(navigator.userAgent));
  if (linuxDesktop && includeAudio) {
    // O vídeo não espera o PipeWire; o áudio pode ser publicado depois.
    const linuxAudioPreparation = prepareDesktopScreenAudio(excludeRisk).catch((error) => {
      console.warn("Não foi possível preparar o áudio PipeWire da tela.", error);
      return null;
    });
    return {
      stream: await startDesktopVideoShare(sourceId),
      desktopAudio: null,
      linuxAudioPreparation,
    };
  }
  if (linuxDesktop) {
    return { stream: await startDesktopVideoShare(sourceId), desktopAudio: null };
  }
  if (!includeAudio) {
    return { stream: await provider.startScreenShare(sourceId, false), desktopAudio: null };
  }

  if (isWindowsDesktop()) {
    // Áudio por processo: janela = só aquele app; tela = tudo menos Discord e BRisk.
    const chosenId = sourceId ?? await window.desktop?.chooseScreenSource();
    if (!chosenId) throw new DOMException("Compartilhamento cancelado.", "NotAllowedError");
    const request = processAudioRequestForSource(chosenId);
    const processAudio = request ? await startProcessAudioCapture(request).catch(() => null) : null;
    if (processAudio) {
      activeProcessAudio = processAudio;
      try {
        const stream = await startDesktopVideoShare(chosenId);
        stream.addTrack(processAudio.track);
        return {
          stream,
          desktopAudio: { mode: "process", excludedRisk: true, sourceName: request?.mode },
        };
      } catch (error) {
        await stopActiveProcessAudio();
        throw error;
      }
    }
    // Sem suporte (Windows antigo etc.): mantém o loopback do sistema sem o Risk.
    sourceId = chosenId;
  }

  const desktopAudio = await prepareDesktopScreenAudio(excludeRisk).catch((error) => {
    console.warn("Não foi possível consultar o backend de áudio de tela; usando captura padrão.", error);
    return null;
  });
  const pipeWireDesktop = desktopAudio?.mode === "pipewire" || desktopAudio?.mode === "unavailable";
  return {
    stream: pipeWireDesktop
      ? await startPipeWireDesktopShare(desktopAudio, sourceId)
      : await provider.startScreenShare(sourceId, true),
    desktopAudio,
  };
}

export function reportScreenAudioExclusion(
  track: MediaStreamTrack,
  preparation: DesktopScreenAudioPreparation | null,
  excludeRisk: boolean,
): void {
  if (!excludeRisk) return;
  if (preparation?.mode === "process") {
    console.info("BRisk screen audio isolated by process", { scope: preparation.sourceName });
    return;
  }
  if (preparation?.mode === "pipewire" && preparation.excludedRisk) {
    console.info("Risk screen audio exclusion active", {
      mode: "pipewire-node-exclusion",
      source: preparation.sourceName ?? preparation.sourceLabel ?? "unknown",
    });
    return;
  }
  const settings = track.getSettings() as DisplayAudioSettings;
  const nativeRiskExclusion = settings.deviceId === "loopbackWithoutChrome";
  const browserRiskExclusion = settings.restrictOwnAudio === true;
  if (nativeRiskExclusion || browserRiskExclusion) {
    console.info("Risk screen audio exclusion active", {
      deviceId: settings.deviceId ?? "unknown",
      restrictOwnAudio: settings.restrictOwnAudio ?? false,
      mode: nativeRiskExclusion ? "native-process-loopback" : "restrictOwnAudio",
    });
  } else {
    console.warn("A captura de tela não confirmou a exclusão do áudio do Risk.", {
      deviceId: settings.deviceId ?? "unknown",
      restrictOwnAudio: settings.restrictOwnAudio ?? false,
    });
  }
}

export async function prepareDesktopScreenAudio(excludeRisk: boolean): Promise<DesktopScreenAudioPreparation | null> {
  if (!window.desktop?.getBackendConfig) return null;
  const config = await window.desktop.getBackendConfig();
  const response = await fetch(`${config.baseUrl.replace(/\/$/, "")}/screen-audio/prepare`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "x-risk-desktop-token": config.token,
    },
    body: JSON.stringify({ excludeRisk }),
  });
  if (!response.ok) throw new Error(`Falha ao preparar áudio de tela (HTTP ${response.status}).`);
  return response.json() as Promise<DesktopScreenAudioPreparation>;
}

let activeProcessAudio: ProcessAudioCapture | undefined;

async function stopActiveProcessAudio(): Promise<void> {
  const current = activeProcessAudio;
  activeProcessAudio = undefined;
  await current?.stop().catch(() => undefined);
}

export async function stopDesktopScreenAudio(): Promise<void> {
  await stopActiveProcessAudio();
  if (!window.desktop?.getBackendConfig) return;
  try {
    const config = await window.desktop.getBackendConfig();
    await fetch(`${config.baseUrl.replace(/\/$/, "")}/screen-audio/stop`, {
      method: "POST",
      headers: { "x-risk-desktop-token": config.token },
    });
  } catch {
    // O sidecar também encerra pw-loopback no shutdown; cleanup best-effort.
  }
}

export async function waitForPipeWireTrack(preparation: DesktopScreenAudioPreparation): Promise<MediaStreamTrack | undefined> {
  if (preparation.mode !== "pipewire") return undefined;
  const expectedLabel = preparation.sourceLabel?.trim().toLocaleLowerCase() ?? "";
  const expectedName = preparation.sourceName?.trim().toLocaleLowerCase() ?? "";
  for (let attempt = 0; attempt < 30; attempt += 1) {
    const devices = await navigator.mediaDevices.enumerateDevices();
    const source = devices.find((device) => {
      if (device.kind !== "audioinput") return false;
      const label = device.label.trim().toLocaleLowerCase();
      return Boolean(label) && (
        (expectedLabel && (label === expectedLabel || label.includes(expectedLabel)))
        || (expectedName && label.includes(expectedName))
      );
    });
    if (source) {
      const capture = await navigator.mediaDevices.getUserMedia({
        audio: {
          deviceId: { exact: source.deviceId },
          echoCancellation: false,
          noiseSuppression: false,
          autoGainControl: false,
          channelCount: 2,
        },
        video: false,
      });
      const track = capture.getAudioTracks()[0];
      if (!track) {
        capture.getTracks().forEach((item) => item.stop());
        return undefined;
      }
      try { track.contentHint = "music"; } catch { /* contentHint é opcional */ }
      return track;
    }
    await new Promise((resolve) => window.setTimeout(resolve, 100));
  }
  return undefined;
}

async function startDesktopVideoShare(selectedSourceId?: string): Promise<MediaStream> {
  if (!window.desktop) throw new Error("Bridge desktop indisponível para captura da tela.");
  const sourceId = selectedSourceId ?? await window.desktop.chooseScreenSource();
  if (!sourceId) throw new DOMException("Compartilhamento cancelado.", "NotAllowedError");
  await window.desktop.selectScreenSource(sourceId);
  return navigator.mediaDevices.getDisplayMedia({ video: true, audio: false });
}

async function startPipeWireDesktopShare(
  preparation: DesktopScreenAudioPreparation,
  selectedSourceId?: string,
): Promise<MediaStream> {
  const stream = await startDesktopVideoShare(selectedSourceId);
  if (preparation.mode === "pipewire") {
    const audioTrack = await waitForPipeWireTrack(preparation);
    if (audioTrack) stream.addTrack(audioTrack);
  }
  return stream;
}
