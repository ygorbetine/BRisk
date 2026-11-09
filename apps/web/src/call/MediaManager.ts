import { WebScreenShareProvider, type CallTransport, type ScreenShareProvider } from "@briks/rtc";
import type { PeerState } from "@briks/protocol";
import { useCallStore } from "../store";
import { stabilizeMicrophoneGain } from "../services/audio/microphone";
import { loadVoiceVideoSettings, type VoiceVideoSettings } from "../services/audio/settings";
import {
  applyScreenCaptureQuality,
  SCREEN_QUALITY_PROFILES,
  screenVideoPublication,
  secondScreenPublication,
  type ScreenQualityProfile,
} from "../services/rtc/screen-quality";
import { openCamera } from "./media/camera";
import { createMicrophoneSession, stopMicrophoneSession, type MicrophoneSession } from "./media/microphone";
import {
  reportScreenAudioExclusion,
  startScreenCapture,
  stopDesktopScreenAudio,
  waitForPipeWireTrack,
  type DesktopScreenAudioPreparation,
} from "./media/screen-share";

export type MediaManagerDependencies = {
  getTransport(): CallTransport | undefined;
  currentLifecycle(): number;
  isActive(lifecycle: number): boolean;
  sendState(state: PeerState, failureMessage: string): void;
  reportError(error: unknown, fallback: string): void;
};

/**
 * Controla exclusivamente as capturas e publicações locais. O controller da
 * chamada não precisa conhecer tracks PipeWire, sessões RNNoise ou revisões de
 * qualidade da tela.
 */
export class MediaManager {
  private local = new MediaStream();
  private microphoneInputStream?: MediaStream;
  private microphone?: MediaStreamTrack;
  private rnnoiseMicrophone?: MicrophoneSession["rnnoise"];
  private microphoneSettings?: VoiceVideoSettings;
  private camera?: MediaStreamTrack;
  private screenStream?: MediaStream;
  private secondScreenStream?: MediaStream;
  private readonly secondScreen: ScreenShareProvider = new WebScreenShareProvider();
  private screenQualityProfile: ScreenQualityProfile = SCREEN_QUALITY_PROFILES["1080p30"];
  private screenQualityRevision = 0;
  private peerState: PeerState = { microphone: false, camera: false, screenShare: false };

  constructor(
    private readonly dependencies: MediaManagerDependencies,
    private readonly screen: ScreenShareProvider = new WebScreenShareProvider(),
  ) {}

  get localStream(): MediaStream { return this.local; }
  get microphoneTrack(): MediaStreamTrack | undefined { return this.microphone; }
  get state(): PeerState { return this.peerState; }

  reset(): void {
    this.local = new MediaStream();
    this.microphoneInputStream = undefined;
    this.microphone = undefined;
    this.rnnoiseMicrophone = undefined;
    this.microphoneSettings = undefined;
    this.camera = undefined;
    this.screenStream = undefined;
    this.secondScreenStream = undefined;
    this.peerState = { microphone: false, camera: false, screenShare: false };
  }

  async initializeMicrophone(
    settings: VoiceVideoSettings,
    transport: CallTransport,
    lifecycle: number,
  ): Promise<MediaStream> {
    const microphoneSession = await createMicrophoneSession(settings);
    if (!this.dependencies.isActive(lifecycle)) {
      await stopMicrophoneSession(microphoneSession);
      throw new DOMException("Entrada na chamada cancelada.", "AbortError");
    }

    this.microphoneInputStream = microphoneSession.inputStream;
    this.rnnoiseMicrophone = microphoneSession.rnnoise;
    this.microphone = microphoneSession.track;
    this.microphoneSettings = { ...settings };
    // O microfone começa mudo: evita vazar voz ao entrar (convive com a call do Discord).
    microphoneSession.track.enabled = false;
    microphoneSession.inputStream.getAudioTracks().forEach((input) => { input.enabled = false; });
    this.local.addTrack(microphoneSession.track);
    await transport.publishTrack(microphoneSession.track, this.local);
    if (!this.dependencies.isActive(lifecycle)) throw new DOMException("Entrada na chamada cancelada.", "AbortError");
    this.peerState.cameraStreamId = this.local.id;
    this.updateLocalPreview();
    return this.local;
  }

  async toggleMicrophone(): Promise<void> {
    try {
      const track = this.microphone ?? this.local.getAudioTracks()[0];
      if (!track) return;
      track.enabled = !track.enabled;
      this.microphoneInputStream?.getAudioTracks().forEach((input) => { input.enabled = track.enabled; });
      this.peerState.microphone = track.enabled;
      await this.dependencies.getTransport()?.setMicrophoneMuted?.(!track.enabled).catch(() => undefined);
      this.notifyState("Não foi possível atualizar o microfone.");
    } catch (error) {
      this.dependencies.reportError(error, "Não foi possível alterar o microfone.");
    }
  }

  async updateVoiceInput(settings: VoiceVideoSettings): Promise<void> {
    const lifecycle = this.dependencies.currentLifecycle();
    const previousTrack = this.microphone;
    const previousInputStream = this.microphoneInputStream;
    const previousRnnoise = this.rnnoiseMicrophone;
    const transport = this.dependencies.getTransport();
    if (!previousTrack || !previousInputStream || !transport || !this.dependencies.isActive(lifecycle)) {
      throw new Error("A chamada não está pronta para trocar o dispositivo de áudio.");
    }

    const replacement = await createMicrophoneSession(settings);
    if (!this.dependencies.isActive(lifecycle) || this.microphone !== previousTrack) {
      await stopMicrophoneSession(replacement);
      throw new DOMException("Troca de microfone cancelada porque a chamada mudou.", "AbortError");
    }

    const syncEnabledState = () => {
      const enabled = this.peerState.microphone;
      replacement.track.enabled = enabled;
      replacement.inputStream.getAudioTracks().forEach((track) => { track.enabled = enabled; });
    };
    syncEnabledState();

    try {
      await transport.replacePublishedTrack(previousTrack, replacement.track, this.local);
    } catch (error) {
      await stopMicrophoneSession(replacement);
      throw error;
    }
    if (!this.dependencies.isActive(lifecycle)) {
      await stopMicrophoneSession(replacement);
      return;
    }

    syncEnabledState();
    this.local.removeTrack(previousTrack);
    this.local.addTrack(replacement.track);
    this.microphoneInputStream = replacement.inputStream;
    this.microphone = replacement.track;
    this.rnnoiseMicrophone = replacement.rnnoise;
    this.microphoneSettings = { ...settings };
    this.updateLocalPreview();

    previousInputStream.getTracks().forEach((track) => track.stop());
    previousTrack.stop();
    await previousRnnoise?.stop().catch(() => undefined);
    console.info("Briks live microphone settings applied", {
      deviceId: replacement.inputStream.getAudioTracks()[0]?.getSettings().deviceId ?? "unknown",
      noiseSuppression: settings.noiseSuppression,
      echoCancellation: settings.echoCancellation,
      muted: !this.peerState.microphone,
    });
  }

  async toggleCamera(): Promise<void> {
    const lifecycle = this.dependencies.currentLifecycle();
    try {
      if (this.camera) {
        const track = this.camera;
        this.camera = undefined;
        this.peerState.camera = false;
        this.notifyState("Não foi possível atualizar a câmera.");
        await this.dependencies.getTransport()?.unpublishTrack(track);
        if (!this.dependencies.isActive(lifecycle)) return;
        track.stop();
        this.local.removeTrack(track);
        return;
      }

      const { stream: cameraStream, track: cameraTrack } = await openCamera();
      if (!this.dependencies.isActive(lifecycle)) {
        cameraStream.getTracks().forEach((track) => track.stop());
        return;
      }
      this.camera = cameraTrack;
      this.local.addTrack(cameraTrack);
      this.peerState.camera = true;
      this.notifyState("Não foi possível atualizar a câmera.");
      try {
        await this.dependencies.getTransport()?.publishTrack(cameraTrack, this.local, { source: "camera" });
      } catch (error) {
        if (this.camera === cameraTrack) this.camera = undefined;
        this.local.removeTrack(cameraTrack);
        cameraTrack.stop();
        this.peerState.camera = false;
        this.notifyState("Não foi possível atualizar a câmera.");
        throw error;
      }
      if (!this.dependencies.isActive(lifecycle)) cameraTrack.stop();
    } catch (error) {
      this.dependencies.reportError(error, "Não foi possível alterar a câmera.");
    }
  }

  async toggleScreen(
    sourceId: string | undefined,
    includeAudio: boolean,
    qualityProfile?: ScreenQualityProfile,
    audioApps?: string[],
  ): Promise<void> {
    if (this.screenStream) {
      await this.stopScreen();
      return;
    }
    if (qualityProfile) this.setScreenQualityProfile(qualityProfile);
    const lifecycle = this.dependencies.currentLifecycle();
    let desktopAudio: DesktopScreenAudioPreparation | null = null;
    let linuxAudioPreparation: Promise<DesktopScreenAudioPreparation | null> | undefined;
    let stream: MediaStream | undefined;
    try {
      const settings = loadVoiceVideoSettings();
      const capture = await startScreenCapture(
        this.screen,
        sourceId,
        includeAudio,
        settings.excludeBriksAudioFromScreenShare,
        audioApps,
      );
      stream = capture.stream;
      desktopAudio = capture.desktopAudio;
      linuxAudioPreparation = capture.linuxAudioPreparation;

      const videoTrack = stream.getVideoTracks()[0];
      if (!videoTrack) {
        stream.getTracks().forEach((track) => track.stop());
        throw new Error("A fonte selecionada não forneceu vídeo");
      }
      const publicationProfile = await this.applyLatestScreenCaptureQuality(videoTrack);

      if (desktopAudio?.mode === "unavailable") {
        const message = `PipeWire indisponível para áudio da tela: ${desktopAudio.reason ?? "ferramentas PipeWire não encontradas"}. A tela continuará sem áudio do sistema.`;
        console.warn(message);
        useCallStore.getState().setError(message);
      } else if (desktopAudio?.mode === "pipewire" && !stream.getAudioTracks().length) {
        const message = "A fonte virtual do PipeWire foi criada, mas o Chromium não a expôs como entrada de áudio. A tela continuará sem áudio do sistema.";
        console.warn(message);
        useCallStore.getState().setError(message);
      }

      const screenAudioTrack = stream.getAudioTracks()[0];
      if (screenAudioTrack && settings.excludeBriksAudioFromScreenShare) {
        reportScreenAudioExclusion(screenAudioTrack, desktopAudio, true);
      }
      if (!this.dependencies.isActive(lifecycle)) {
        stream.getTracks().forEach((track) => track.stop());
        await stopDesktopScreenAudio();
        return;
      }

      this.screenStream = stream;
      // Um evento tardio da captura anterior não pode encerrar uma nova
      // transmissão iniciada no mesmo MediaManager.
      videoTrack.addEventListener("ended", () => { void this.stopScreen(stream); }, { once: true });
      const transport = this.dependencies.getTransport();
      if (!transport) {
        stream.getTracks().forEach((track) => track.stop());
        await stopDesktopScreenAudio();
        this.screenStream = undefined;
        return;
      }

      this.peerState.screenShare = true;
      this.peerState.screenStreamId = stream.id;
      this.peerState.screenAudio = stream.getAudioTracks().length > 0;
      this.notifyState("Não foi possível atualizar o compartilhamento de tela.");
      await Promise.all(stream.getTracks().map((track) => transport.publishTrack(
        track,
        stream!,
        track.kind === "video" ? screenVideoPublication(publicationProfile) : undefined,
      )));
      if (!this.dependencies.isActive(lifecycle)) {
        stream.getTracks().forEach((track) => track.stop());
        await stopDesktopScreenAudio();
        return;
      }

      await this.ensureMicrophoneAfterScreenCapture(transport, lifecycle);
      if (linuxAudioPreparation) {
        void this.attachLinuxScreenAudio(stream, lifecycle, linuxAudioPreparation, settings.excludeBriksAudioFromScreenShare);
      }
    } catch (error) {
      if (linuxAudioPreparation) {
        void linuxAudioPreparation.then(() => stopDesktopScreenAudio()).catch(() => undefined);
      }
      if (stream && this.screenStream === stream) await this.stopScreen().catch(() => undefined);
      else if (desktopAudio?.mode === "pipewire") await stopDesktopScreenAudio();
      if (!(error instanceof DOMException && error.name === "NotAllowedError")) {
        this.dependencies.reportError(error, "Não foi possível compartilhar a tela.");
      }
    }
  }

  /** Liga ou desliga a segunda tela (outro monitor). Sem áudio e com bitrate menor para não dobrar o upload. */
  async toggleSecondScreen(sourceId: string | undefined, qualityProfile?: ScreenQualityProfile): Promise<void> {
    if (this.secondScreenStream) {
      await this.stopSecondScreen();
      return;
    }
    if (!this.screenStream) {
      this.dependencies.reportError(new Error("Inicie a primeira transmissão antes de adicionar a segunda."), "Inicie a primeira transmissão antes.");
      return;
    }
    if (qualityProfile) this.setScreenQualityProfile(qualityProfile);
    const lifecycle = this.dependencies.currentLifecycle();
    let stream: MediaStream | undefined;
    try {
      stream = await this.secondScreen.startScreenShare(sourceId, false);
      const videoTrack = stream.getVideoTracks()[0];
      if (!videoTrack) {
        stream.getTracks().forEach((track) => track.stop());
        throw new Error("A fonte selecionada não forneceu vídeo");
      }
      const profile = await this.applyLatestScreenCaptureQuality(videoTrack);
      const transport = this.dependencies.getTransport();
      if (!this.dependencies.isActive(lifecycle) || !transport) {
        stream.getTracks().forEach((track) => track.stop());
        return;
      }
      this.secondScreenStream = stream;
      const captured = stream;
      videoTrack.addEventListener("ended", () => { void this.stopSecondScreen(captured); }, { once: true });
      this.peerState.screenShare = true;
      this.peerState.secondScreenStreamId = stream.id;
      this.notifyState("Não foi possível atualizar a segunda tela.");
      await transport.publishTrack(videoTrack, stream, secondScreenPublication(profile));
    } catch (error) {
      if (stream && this.secondScreenStream === stream) await this.stopSecondScreen().catch(() => undefined);
      else stream?.getTracks().forEach((track) => track.stop());
      if (!(error instanceof DOMException && error.name === "NotAllowedError")) {
        this.dependencies.reportError(error, "Não foi possível compartilhar a segunda tela.");
      }
    }
  }

  private async stopSecondScreen(expected?: MediaStream): Promise<void> {
    const stream = this.secondScreenStream;
    if (!stream || (expected && stream !== expected)) return;
    this.secondScreenStream = undefined;
    this.peerState.secondScreenStreamId = undefined;
    this.peerState.screenShare = Boolean(this.screenStream);
    this.notifyState("Não foi possível atualizar a segunda tela.");
    const transport = this.dependencies.getTransport();
    if (transport) await Promise.all(stream.getTracks().map((track) => transport.unpublishTrack(track).catch(() => undefined)));
    stream.getTracks().forEach((track) => track.stop());
    await this.secondScreen.stopScreenShare().catch(() => undefined);
  }

  async updateScreenQuality(profile: ScreenQualityProfile): Promise<void> {
    this.setScreenQualityProfile(profile);
    const stream = this.screenStream;
    const track = stream?.getVideoTracks()[0];
    const transport = this.dependencies.getTransport();
    if (!stream || !track || !transport) return;

    const lifecycle = this.dependencies.currentLifecycle();
    const publicationProfile = await this.applyLatestScreenCaptureQuality(track);
    if (!this.dependencies.isActive(lifecycle) || this.screenStream !== stream || track.readyState !== "live") return;
    await transport.configurePublishedVideoTrack(track, screenVideoPublication(publicationProfile));
    this.updateLocalPreview();
  }

  async cleanup(): Promise<void> {
    const local = this.local;
    const inputStream = this.microphoneInputStream;
    const rnnoise = this.rnnoiseMicrophone;
    const screenStream = this.screenStream;
    const secondScreenStream = this.secondScreenStream;
    this.reset();
    secondScreenStream?.getTracks().forEach((track) => track.stop());
    void this.secondScreen.stopScreenShare().catch(() => undefined);

    local.getTracks().forEach((track) => track.stop());
    inputStream?.getTracks().forEach((track) => track.stop());
    screenStream?.getTracks().forEach((track) => track.stop());
    useCallStore.getState().setLocalMedia({ microphone: null, camera: null, screen: null, screen2: null }, this.peerState);

    const screenCleanup = this.screen.stopScreenShare().catch(() => undefined);
    const desktopAudioCleanup = stopDesktopScreenAudio();
    await rnnoise?.stop().catch(() => undefined);
    await screenCleanup;
    await desktopAudioCleanup;
  }

  private async attachLinuxScreenAudio(
    stream: MediaStream,
    lifecycle: number,
    preparationPromise: Promise<DesktopScreenAudioPreparation | null>,
    excludeBriks: boolean,
  ): Promise<void> {
    try {
      const preparation = await preparationPromise;
      if (!this.dependencies.isActive(lifecycle) || this.screenStream !== stream) {
        if (preparation?.mode === "pipewire") await stopDesktopScreenAudio();
        return;
      }
      if (!preparation) return;
      if (preparation.mode === "unavailable") {
        const message = `PipeWire indisponível para áudio da tela: ${preparation.reason ?? "ferramentas PipeWire não encontradas"}. A tela continuará sem áudio do sistema.`;
        console.warn(message);
        useCallStore.getState().setError(message);
        return;
      }
      if (preparation.mode !== "pipewire") return;

      const audioTrack = await waitForPipeWireTrack(preparation);
      if (!this.dependencies.isActive(lifecycle) || this.screenStream !== stream) {
        audioTrack?.stop();
        await stopDesktopScreenAudio();
        return;
      }
      if (!audioTrack) {
        const message = "A fonte virtual do PipeWire foi criada, mas o Chromium não a expôs como entrada de áudio. A tela continuará sem áudio do sistema.";
        console.warn(message);
        useCallStore.getState().setError(message);
        return;
      }

      if (excludeBriks && preparation.excludedBriks) {
        console.info("Briks screen audio exclusion active", {
          mode: "pipewire-node-exclusion",
          source: preparation.sourceName ?? preparation.sourceLabel ?? "unknown",
        });
      }

      stream.addTrack(audioTrack);
      this.peerState.screenAudio = true;
      this.notifyState("Não foi possível atualizar o áudio da tela.");
      const transport = this.dependencies.getTransport();
      if (!transport) {
        stream.removeTrack(audioTrack);
        audioTrack.stop();
        this.peerState.screenAudio = false;
        this.updateLocalPreview();
        return;
      }

      try {
        await transport.publishTrack(audioTrack, stream);
        if (this.microphone?.readyState === "live") await transport.publishTrack(this.microphone, this.local);
      } catch (error) {
        stream.removeTrack(audioTrack);
        audioTrack.stop();
        this.peerState.screenAudio = false;
        this.notifyState("Não foi possível atualizar o áudio da tela.");
        this.dependencies.reportError(error, "Não foi possível transmitir o áudio da tela.");
      }
    } catch (error) {
      console.warn("Falha ao anexar áudio PipeWire à transmissão.", error);
    }
  }

  private async stopScreen(expectedStream?: MediaStream): Promise<void> {
    const stream = this.screenStream;
    if (!stream || (expectedStream && stream !== expectedStream)) return;
    this.screenStream = undefined;
    this.peerState.screenShare = Boolean(this.secondScreenStream);
    this.peerState.screenStreamId = undefined;
    this.peerState.screenAudio = false;
    this.notifyState("Não foi possível atualizar o compartilhamento de tela.");
    const transport = this.dependencies.getTransport();
    if (transport) {
      await Promise.all(stream.getTracks().map((track) => transport.unpublishTrack(track).catch(() => undefined)));
    }
    stream.getTracks().forEach((track) => track.stop());
    await this.screen.stopScreenShare().catch(() => undefined);
    await stopDesktopScreenAudio();
    if (this.microphoneInputStream) await stabilizeMicrophoneGain(this.microphoneInputStream);
  }

  private async ensureMicrophoneAfterScreenCapture(transport: CallTransport, lifecycle: number): Promise<void> {
    const intendedEnabled = this.peerState.microphone;
    const currentMicrophone = this.microphone;
    const inputTrack = this.microphoneInputStream?.getAudioTracks()[0];
    if (!currentMicrophone || !inputTrack || !this.dependencies.isActive(lifecycle)) return;

    currentMicrophone.enabled = intendedEnabled;
    inputTrack.enabled = intendedEnabled;
    await stabilizeMicrophoneGain(this.microphoneInputStream!);

    // Alguns capturadores suspendem brevemente a fonte de entrada enquanto o
    // portal de tela é aberto. Damos tempo para o unmute nativo antes de criar
    // outra captura e renegociar o sender.
    if (microphoneTrackUnavailable(currentMicrophone) || microphoneTrackUnavailable(inputTrack)) {
      await new Promise((resolve) => window.setTimeout(resolve, 300));
    }
    if (!this.dependencies.isActive(lifecycle) || this.microphone !== currentMicrophone) return;

    if (!microphoneTrackUnavailable(currentMicrophone) && !microphoneTrackUnavailable(inputTrack)) {
      await transport.publishTrack(currentMicrophone, this.local);
      return;
    }

    const settings = this.microphoneSettings ?? loadVoiceVideoSettings();
    await this.updateVoiceInput(settings);
  }

  private setScreenQualityProfile(profile: ScreenQualityProfile): void {
    this.screenQualityProfile = profile;
    this.screenQualityRevision += 1;
  }

  private async applyLatestScreenCaptureQuality(track: MediaStreamTrack): Promise<ScreenQualityProfile> {
    for (;;) {
      const revision = this.screenQualityRevision;
      const profile = this.screenQualityProfile;
      await applyScreenCaptureQuality(track, profile);
      if (revision === this.screenQualityRevision) return profile;
    }
  }

  private notifyState(failureMessage: string): void {
    this.updateLocalPreview();
    this.dependencies.sendState(this.peerState, failureMessage);
  }

  private updateLocalPreview(): void {
    useCallStore.getState().setLocalMedia({
      microphone: this.microphone ? new MediaStream([this.microphone]) : null,
      camera: this.camera ? new MediaStream([this.camera]) : null,
      screen: this.screenStream ? new MediaStream(this.screenStream.getVideoTracks()) : null,
      screen2: this.secondScreenStream ? new MediaStream(this.secondScreenStream.getVideoTracks()) : null,
    }, this.peerState);
  }
}

function microphoneTrackUnavailable(track: MediaStreamTrack): boolean {
  return track.readyState !== "live" || track.muted;
}
