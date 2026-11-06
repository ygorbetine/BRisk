export type ScreenSource = { id: string; name: string; thumbnail?: string; displayId?: string };

export interface ScreenShareProvider {
  getSources(): Promise<ScreenSource[]>;
  startScreenShare(sourceId?: string, includeAudio?: boolean): Promise<MediaStream>;
  stopScreenShare(): Promise<void>;
}

type DesktopScreenBridge = {
  listScreenSources(): Promise<ScreenSource[]>;
  chooseScreenSource?(): Promise<string | null>;
  selectScreenSource(sourceId: string): Promise<void>;
};

type DisplayAudioConstraints = MediaTrackConstraints & { restrictOwnAudio?: boolean };
type DisplayAudioSettings = MediaTrackSettings & { restrictOwnAudio?: boolean };
type BriksMediaCaptureOptions = { restrictOwnAudio?: boolean };

function desktopScreenBridge(): DesktopScreenBridge | undefined {
  return (globalThis as typeof globalThis & { desktop?: DesktopScreenBridge }).desktop;
}

function briksMediaCaptureOptions(): BriksMediaCaptureOptions | undefined {
  return (globalThis as typeof globalThis & { __briksMediaCaptureOptions?: BriksMediaCaptureOptions }).__briksMediaCaptureOptions;
}

export class WebScreenShareProvider implements ScreenShareProvider {
  private stream?: MediaStream;

  async getSources(): Promise<ScreenSource[]> {
    const desktop = desktopScreenBridge();
    return desktop ? desktop.listScreenSources() : [];
  }

  async startScreenShare(sourceId?: string, includeAudio = true): Promise<MediaStream> {
    const desktop = desktopScreenBridge();
    if (desktop) {
      let selectedSourceId = sourceId;
      if (!selectedSourceId) {
        if (desktop.chooseScreenSource) {
          selectedSourceId = await desktop.chooseScreenSource() ?? undefined;
        } else {
          const sources = await desktop.listScreenSources();
          if (sources.length === 1) selectedSourceId = sources[0]?.id;
          else if (sources.length > 1) throw new Error("O seletor de tela do Electron não está disponível nesta versão do desktop.");
        }
      }
      if (!selectedSourceId) throw new DOMException("Compartilhamento cancelado.", "NotAllowedError");
      await desktop.selectScreenSource(selectedSourceId);
    }

    const restrictOwnAudio = briksMediaCaptureOptions()?.restrictOwnAudio === true;
    const audio: boolean | DisplayAudioConstraints = includeAudio
      ? restrictOwnAudio ? { restrictOwnAudio: true } : true
      : false;
    this.stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio });

    if (includeAudio && restrictOwnAudio) {
      const audioTrack = this.stream.getAudioTracks()[0];
      if (audioTrack) {
        const settings = audioTrack.getSettings() as DisplayAudioSettings;
        console.info("Briks screen audio capture", {
          deviceId: settings.deviceId ?? "unknown",
          restrictOwnAudio: settings.restrictOwnAudio ?? "unknown",
        });
        if (desktop && settings.deviceId === "loopback") {
          console.warn("A captura ainda está usando loopback completo; o áudio do Briks pode retornar na transmissão.");
        }
      }
    }

    return this.stream;
  }

  async stopScreenShare(): Promise<void> {
    this.stream?.getTracks().forEach((track) => track.stop());
    this.stream = undefined;
  }
}
