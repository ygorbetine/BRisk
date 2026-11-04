import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { MeshWebRTCTransport, WebScreenShareProvider } from "./index";

class FakeRtpSender {
  parameters = {
    transactionId: "fake",
    encodings: [{}],
    codecs: [],
    headerExtensions: [],
    rtcp: {},
  } as unknown as RTCRtpSendParameters;

  constructor(public track: MediaStreamTrack | null) {}
  getParameters(): RTCRtpSendParameters { return this.parameters; }
  async setParameters(parameters: RTCRtpSendParameters): Promise<void> { this.parameters = parameters; }
  async replaceTrack(track: MediaStreamTrack | null): Promise<void> { this.track = track; }
}

class FakePeerConnection {
  static instances: FakePeerConnection[] = [];
  static addedIce: RTCIceCandidateInit[] = [];
  static dataChannels: FakeDataChannel[] = [];
  static addedTracks: MediaStreamTrack[] = [];
  static failMLineOrderOnce = false;
  static configurations: RTCConfiguration[] = [];
  static stats: Array<Record<string, unknown>> = [];
  static restartIceCalls = 0;
  static offerOptions: RTCOfferOptions[] = [];
  connectionState: RTCPeerConnectionState = "new";
  iceConnectionState: RTCIceConnectionState = "new";
  signalingState: RTCSignalingState = "stable";
  localDescription: RTCSessionDescription | null = null;
  remoteDescription: RTCSessionDescription | null = null;
  onicecandidate: ((event: RTCPeerConnectionIceEvent) => void) | null = null;
  ontrack: ((event: RTCTrackEvent) => void) | null = null;
  onnegotiationneeded: (() => void) | null = null;
  onconnectionstatechange: (() => void) | null = null;
  ondatachannel: ((event: RTCDataChannelEvent) => void) | null = null;
  readonly senders: FakeRtpSender[] = [];

  constructor(configuration: RTCConfiguration) {
    FakePeerConnection.instances.push(this);
    FakePeerConnection.configurations.push(configuration);
  }
  async createOffer(options: RTCOfferOptions = {}): Promise<RTCSessionDescriptionInit> {
    FakePeerConnection.offerOptions.push(options);
    return { type: "offer", sdp: "offer" };
  }
  async createAnswer(): Promise<RTCSessionDescriptionInit> { return { type: "answer", sdp: "answer" }; }
  async setLocalDescription(description: RTCSessionDescriptionInit): Promise<void> {
    this.localDescription = descriptionWithJson(description);
    this.signalingState = description.type === "offer" ? "have-local-offer" : "stable";
  }
  async setRemoteDescription(description: RTCSessionDescriptionInit): Promise<void> {
    if (description.type === "offer" && FakePeerConnection.failMLineOrderOnce) {
      FakePeerConnection.failMLineOrderOnce = false;
      throw new DOMException("Failed to set remote offer sdp: The order of m-lines in subsequent offer doesn't match order from previous offer/answer.", "InvalidAccessError");
    }
    this.remoteDescription = descriptionWithJson(description);
    this.signalingState = description.type === "offer" ? "have-remote-offer" : "stable";
  }
  async addIceCandidate(candidate: RTCIceCandidateInit): Promise<void> { FakePeerConnection.addedIce.push(candidate); }
  addTrack(track: MediaStreamTrack): RTCRtpSender {
    FakePeerConnection.addedTracks.push(track);
    const sender = new FakeRtpSender(track);
    this.senders.push(sender);
    return sender as unknown as RTCRtpSender;
  }
  removeTrack(sender: RTCRtpSender): void {
    const index = this.senders.indexOf(sender as unknown as FakeRtpSender);
    if (index >= 0) this.senders.splice(index, 1);
  }
  getSenders(): RTCRtpSender[] { return this.senders as unknown as RTCRtpSender[]; }
  getTransceivers(): RTCRtpTransceiver[] { return []; }
  createDataChannel(label: string): RTCDataChannel { const channel = new FakeDataChannel(label); FakePeerConnection.dataChannels.push(channel); return channel as unknown as RTCDataChannel; }
  restartIce(): void { FakePeerConnection.restartIceCalls += 1; }
  async getStats(): Promise<RTCStatsReport> {
    return new Map(FakePeerConnection.stats.map((stat) => [String(stat.id), stat])) as unknown as RTCStatsReport;
  }
  close(): void { this.connectionState = "closed"; }
}

class FakeDataChannel {
  readyState: RTCDataChannelState = "connecting";
  bufferedAmount = 0;
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((event: MessageEvent<string>) => void) | null = null;
  constructor(readonly label: string) {}
  open(): void { this.readyState = "open"; this.onopen?.(); }
  send(data: string): void { this.sent.push(data); }
  receive(data: string): void { this.onmessage?.(new MessageEvent("message", { data })); }
  close(): void { this.readyState = "closed"; this.onclose?.(); }
}

function descriptionWithJson(value: RTCSessionDescriptionInit): RTCSessionDescription {
  return { type: value.type!, sdp: value.sdp ?? "", toJSON: () => value } as RTCSessionDescription;
}

describe("WebScreenShareProvider", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("seleciona a única fonte Electron antes de chamar getDisplayMedia", async () => {
    const selectScreenSource = vi.fn(async () => undefined);
    const getDisplayMedia = vi.fn(async () => ({ getTracks: () => [] }) as unknown as MediaStream);
    vi.stubGlobal("desktop", {
      listScreenSources: vi.fn(async () => [{ id: "screen:1:0", name: "Tela 1" }]),
      selectScreenSource,
    });
    vi.stubGlobal("navigator", { mediaDevices: { getDisplayMedia } });

    const provider = new WebScreenShareProvider();
    const stream = await provider.startScreenShare();

    expect(selectScreenSource).toHaveBeenCalledWith("screen:1:0");
    expect(getDisplayMedia).toHaveBeenCalledWith({ video: true, audio: true });
    expect(stream).toBeDefined();
  });

  it("mantém o picker nativo do navegador quando não há bridge Electron", async () => {
    const getDisplayMedia = vi.fn(async () => ({ getTracks: () => [] }) as unknown as MediaStream);
    vi.stubGlobal("navigator", { mediaDevices: { getDisplayMedia } });

    const provider = new WebScreenShareProvider();
    await provider.startScreenShare();

    expect(getDisplayMedia).toHaveBeenCalledOnce();
  });

  it("não solicita captura de áudio quando o usuário escolhe transmitir somente vídeo", async () => {
    const getDisplayMedia = vi.fn(async () => ({ getTracks: () => [] }) as unknown as MediaStream);
    vi.stubGlobal("navigator", { mediaDevices: { getDisplayMedia } });

    const provider = new WebScreenShareProvider();
    await provider.startScreenShare(undefined, false);

    expect(getDisplayMedia).toHaveBeenCalledWith({ video: true, audio: false });
  });
});

describe("MeshWebRTCTransport", () => {
  beforeEach(() => {
    FakePeerConnection.instances = [];
    FakePeerConnection.addedIce = [];
    FakePeerConnection.dataChannels = [];
    FakePeerConnection.addedTracks = [];
    FakePeerConnection.failMLineOrderOnce = false;
    FakePeerConnection.configurations = [];
    FakePeerConnection.stats = [];
    FakePeerConnection.restartIceCalls = 0;
    FakePeerConnection.offerOptions = [];
    vi.stubGlobal("RTCPeerConnection", FakePeerConnection);
  });
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it("mantém no máximo uma RTCPeerConnection por peer", async () => {
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], events());
    await transport.connect("00000000-0000-4000-8000-000000000002", false);
    await transport.connect("00000000-0000-4000-8000-000000000002", false);
    expect(FakePeerConnection.instances).toHaveLength(1);
    await transport.disconnect();
  });

  it("implementa o ciclo de sessão comum a Mesh e SFU", async () => {
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const transport = new MeshWebRTCTransport(localPeerId, [], events());
    await transport.join({ roomId: "room-1", localPeerId });
    await transport.connectPeer("00000000-0000-4000-8000-000000000002", false);
    expect(transport.kind).toBe("mesh");
    expect(transport.getDiagnostics()).toHaveLength(1);
    await transport.leave();
    expect(transport.getDiagnostics()).toHaveLength(0);
    await expect(transport.join({ roomId: "room-2", localPeerId: "outro-peer" })).rejects.toThrow("outro peer local");
  });

  it("preserva ICE normal com host, STUN e TURN habilitados", async () => {
    const iceServers = [{ urls: "stun:stun.example.test" }, { urls: "turn:turn.example.test" }];
    const transport = new MeshWebRTCTransport("local", iceServers, events());
    await transport.connect("remote", false);
    expect(FakePeerConnection.configurations[0]).toEqual({ iceServers, iceTransportPolicy: "all", iceCandidatePoolSize: 4 });
  });

  it("prioriza o candidate ZeroTier antes de enviá-lo ao peer", async () => {
    const callbacks = events();
    const transport = new MeshWebRTCTransport("local", [], callbacks, {
      networkPreference: "private-vpn",
      networkInterfaces: [{ name: "ztabcd1234", address: "10.147.20.5", family: "IPv4", provider: "zerotier" }],
    });
    await transport.connect("remote", false);
    FakePeerConnection.instances[0]!.onicecandidate?.({
      candidate: { toJSON: () => ({ candidate: "candidate:1 1 udp 1800000000 10.147.20.5 50000 typ host" }) },
    } as unknown as RTCPeerConnectionIceEvent);
    expect(callbacks.sendIce).toHaveBeenCalledWith("remote", expect.objectContaining({
      candidate: expect.stringContaining("udp 2130706431 10.147.20.5"),
    }));
  });

  it("não sinaliza o candidate VPN quando internet direta foi escolhida", async () => {
    const callbacks = events();
    const transport = new MeshWebRTCTransport("local", [], callbacks, {
      networkPreference: "internet-direct",
      networkInterfaces: [{ name: "tailscale0", address: "100.64.0.8", family: "IPv4", provider: "tailscale" }],
    });
    await transport.connect("remote", false);
    FakePeerConnection.instances[0]!.onicecandidate?.({
      candidate: { toJSON: () => ({ candidate: "candidate:1 1 udp 2122260223 100.64.0.8 50000 typ host" }) },
    } as unknown as RTCPeerConnectionIceEvent);
    expect(callbacks.sendIce).not.toHaveBeenCalled();
  });

  it("reavalia a rota selecionada a cada getStats sem expor o endereço", async () => {
    const transport = new MeshWebRTCTransport("local", [], events(), [{
      name: "ztabcd1234",
      address: "10.147.20.5",
      family: "IPv4",
      provider: "zerotier",
    }]);
    await transport.connect("remote", false);
    FakePeerConnection.stats = [
      { id: "transport", type: "transport", selectedCandidatePairId: "pair" },
      { id: "pair", type: "candidate-pair", state: "succeeded", localCandidateId: "local-candidate", remoteCandidateId: "remote-candidate", currentRoundTripTime: 0.021 },
      { id: "local-candidate", type: "local-candidate", candidateType: "host", address: "10.147.20.5", protocol: "udp" },
      { id: "remote-candidate", type: "remote-candidate", candidateType: "host", address: "10.147.20.8", protocol: "udp" },
    ];
    const vpn = (await transport.collectDiagnostics())[0]!;
    expect(vpn.selectedConnectionPath).toMatchObject({ kind: "vpn-direct", provider: "zerotier" });
    expect(vpn.roundTripTimeMs).toBe(21);
    expect(JSON.stringify(vpn)).not.toContain("10.147.20.5");

    FakePeerConnection.stats = [
      { id: "transport", type: "transport", selectedCandidatePairId: "relay-pair" },
      { id: "relay-pair", type: "candidate-pair", state: "succeeded", localCandidateId: "relay", remoteCandidateId: "remote-candidate" },
      { id: "relay", type: "local-candidate", candidateType: "relay", protocol: "udp" },
      { id: "remote-candidate", type: "remote-candidate", candidateType: "host" },
    ];
    expect((await transport.collectDiagnostics())[0]?.selectedConnectionPath.kind).toBe("turn-relay");
  });

  it("mantém cinco conexões remotas para uma chamada Mesh de seis participantes", async () => {
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], events());
    for (let index = 2; index <= 6; index += 1) {
      await transport.connect(`00000000-0000-4000-8000-${String(index).padStart(12, "0")}`, false);
    }
    expect(FakePeerConnection.instances).toHaveLength(5);
    expect(transport.getDiagnostics()).toHaveLength(5);
    await expect(transport.connect("00000000-0000-4000-8000-000000000007", false)).rejects.toThrow("Sala cheia");
    expect(transport.getDiagnostics()).toHaveLength(5);
    await transport.disconnect();
  });

  it("enfileira ICE até remoteDescription e depois processa a fila", async () => {
    const callbacks = events();
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000002", [], callbacks);
    const candidate = { candidate: "candidate:1", sdpMid: "0", sdpMLineIndex: 0 };
    await transport.addIceCandidate("00000000-0000-4000-8000-000000000001", candidate);
    expect(FakePeerConnection.addedIce).toHaveLength(0);
    expect(transport.getDiagnostics()[0]?.pendingIceCandidates).toBe(1);
    await transport.acceptOffer("00000000-0000-4000-8000-000000000001", { type: "offer", sdp: "offer" });
    expect(FakePeerConnection.addedIce).toEqual([candidate]);
    expect(callbacks.sendAnswer).toHaveBeenCalledOnce();
    expect(transport.getDiagnostics()[0]?.pendingIceCandidates).toBe(0);
  });

  it("renegocia imediatamente quando uma track é publicada após a conexão", async () => {
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(1);
    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer-1" });

    const track = { id: "camera-track", kind: "video" } as MediaStreamTrack;
    const stream = { id: "camera-stream" } as MediaStream;
    await transport.publishTrack(track, stream);

    expect(callbacks.sendOffer).toHaveBeenCalledTimes(2);
  });

  it("aplica no sender a política de alta movimentação e permite atualizá-la ao vivo", async () => {
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer-1" });

    const track = {
      id: "screen-track",
      kind: "video",
      getSettings: () => ({ width: 3840, height: 2160 }),
    } as unknown as MediaStreamTrack;
    await transport.publishTrack(track, { id: "screen-stream" } as MediaStream, {
      source: "screen",
      maxBitrate: 9_000_000,
      maxFramerate: 60,
      targetWidth: 1920,
      targetHeight: 1080,
      degradationPreference: "balanced",
    });

    const sender = FakePeerConnection.instances[0]!.senders[0]!;
    expect(sender.parameters.encodings[0]).toMatchObject({
      maxBitrate: 9_000_000,
      maxFramerate: 60,
      scaleResolutionDownBy: 2,
      priority: "high",
      networkPriority: "high",
    });
    expect(sender.parameters.degradationPreference).toBe("balanced");

    await transport.configurePublishedVideoTrack(track, {
      source: "screen",
      maxBitrate: 3_500_000,
      maxFramerate: 30,
      targetWidth: 1280,
      targetHeight: 720,
      degradationPreference: "maintain-resolution",
    });
    expect(sender.parameters.encodings[0]).toMatchObject({
      maxBitrate: 3_500_000,
      maxFramerate: 30,
      scaleResolutionDownBy: 3,
    });
    expect(sender.parameters.degradationPreference).toBe("maintain-resolution");
  });

  it("não publica mídia antes da autorização criptográfica do peer", async () => {
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], events());
    transport.requireMediaAuthorization();
    const track = { id: "microphone", kind: "audio" } as MediaStreamTrack;
    await transport.publishTrack(track, { id: "local" } as MediaStream);
    await transport.connect(peerId, false);
    expect(FakePeerConnection.addedTracks).toHaveLength(0);
    await transport.authorizePeerMedia(peerId);
    expect(FakePeerConnection.addedTracks).toEqual([track]);
  });

  it("mantém mídia remota fora da UI até a autorização criptográfica", async () => {
    const peerId = "00000000-0000-4000-8000-000000000002";
    const callbacks = events();
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    transport.requireMediaAuthorization();
    await transport.connect(peerId, false);
    const track = { id: "remote-camera", kind: "video", enabled: true } as MediaStreamTrack;
    const stream = { id: "remote-stream", getTracks: () => [track] } as unknown as MediaStream;

    FakePeerConnection.instances[0]!.ontrack?.({ streams: [stream] } as unknown as RTCTrackEvent);

    expect(track.enabled).toBe(false);
    expect(callbacks.onRemoteStream).not.toHaveBeenCalled();
    await transport.authorizePeerMedia(peerId);
    expect(track.enabled).toBe(true);
    expect(callbacks.onRemoteStream).toHaveBeenCalledWith(peerId, stream);
    transport.revokePeerMedia(peerId);
    expect(track.enabled).toBe(false);
  });

  it("preserva renegociação pendente se a câmera/tela ligar enquanto uma offer está em voo", async () => {
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(1);

    const track = { id: "screen-track", kind: "video" } as MediaStreamTrack;
    const stream = { id: "screen-stream" } as MediaStream;
    await transport.publishTrack(track, stream);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(1);

    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer-1" });
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(2);
  });

  it("reinicia o ICE quando a conexão inicial nunca chega a connected", async () => {
    vi.useFakeTimers();
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer" });
    const before = FakePeerConnection.restartIceCalls;

    await vi.advanceTimersByTimeAsync(14_000);
    expect(FakePeerConnection.restartIceCalls).toBe(before);
    await vi.advanceTimersByTimeAsync(2_000);
    expect(FakePeerConnection.restartIceCalls).toBe(before + 1);

    await transport.disconnect();
    vi.useRealTimers();
  });

  it("não reinicia o ICE pelo vigia se a conexão já ficou connected", async () => {
    vi.useFakeTimers();
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], events());
    const peerId = "00000000-0000-4000-8000-000000000002";
    await transport.connect(peerId, true);
    const connection = FakePeerConnection.instances[0]!;
    const before = FakePeerConnection.restartIceCalls;
    connection.connectionState = "connected";
    connection.onconnectionstatechange?.();

    await vi.advanceTimersByTimeAsync(60_000);
    expect(FakePeerConnection.restartIceCalls).toBe(before);

    await transport.disconnect();
    vi.useRealTimers();
  });

  it("reinicia o ICE de todos os peers quando a rede local muda", async () => {
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], events());
    await transport.connect("00000000-0000-4000-8000-000000000002", true);
    await transport.connect("00000000-0000-4000-8000-000000000003", true);
    const before = FakePeerConnection.restartIceCalls;

    await transport.notifyNetworkChange();
    expect(FakePeerConnection.restartIceCalls).toBe(before + 2);
    await transport.disconnect();
  });

  it("o lado de id maior assume a recuperação depois de uma folga", async () => {
    vi.useFakeTimers();
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000009", [], events());
    const peerId = "00000000-0000-4000-8000-000000000001";
    await transport.connect(peerId, false);
    const connection = FakePeerConnection.instances[0]!;
    const before = FakePeerConnection.restartIceCalls;
    connection.connectionState = "failed";
    connection.onconnectionstatechange?.();

    await vi.advanceTimersByTimeAsync(5_000);
    expect(FakePeerConnection.restartIceCalls).toBe(before);
    await vi.advanceTimersByTimeAsync(2_000);
    expect(FakePeerConnection.restartIceCalls).toBe(before + 1);

    await transport.disconnect();
    vi.useRealTimers();
  });

  it("preserva o ICE restart quando a queda ocorre com uma offer em andamento", async () => {
    vi.useFakeTimers();
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    const connection = FakePeerConnection.instances[0]!;
    connection.connectionState = "failed";
    connection.onconnectionstatechange?.();

    await vi.advanceTimersByTimeAsync(0);
    expect(FakePeerConnection.restartIceCalls).toBe(1);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(1);

    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer-inicial-atrasada" });
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(2);
    expect(FakePeerConnection.offerOptions.at(-1)).toMatchObject({ iceRestart: true });

    connection.connectionState = "connected";
    connection.onconnectionstatechange?.();
    await transport.disconnect();
    vi.useRealTimers();
  });

  it("repete a recuperação e recria um peer que continua travado", async () => {
    vi.useFakeTimers();
    const callbacks = { ...events(), onDataMessage: vi.fn(), onPeerReset: vi.fn() };
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);
    await transport.acceptAnswer(peerId, { type: "answer", sdp: "answer" });
    const stalled = FakePeerConnection.instances[0]!;
    stalled.connectionState = "failed";
    stalled.onconnectionstatechange?.();

    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(4_000);
    await vi.advanceTimersByTimeAsync(8_000);

    expect(FakePeerConnection.restartIceCalls).toBe(3);
    expect(FakePeerConnection.instances).toHaveLength(2);
    expect(callbacks.onPeerReset).toHaveBeenCalledWith(peerId);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(3);

    const replacement = FakePeerConnection.instances[1]!;
    replacement.connectionState = "connected";
    replacement.onconnectionstatechange?.();
    await transport.disconnect();
    vi.useRealTimers();
  });

  it("permite recriar explicitamente um peer cujo DataChannel ficou travado", async () => {
    const callbacks = { ...events(), onDataMessage: vi.fn(), onDataState: vi.fn(), onPeerReset: vi.fn() };
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, true);

    expect(FakePeerConnection.instances).toHaveLength(1);
    expect(FakePeerConnection.dataChannels).toHaveLength(1);
    await transport.recoverPeer(peerId);

    expect(FakePeerConnection.instances).toHaveLength(2);
    expect(FakePeerConnection.dataChannels).toHaveLength(2);
    expect(callbacks.onPeerReset).toHaveBeenCalledWith(peerId);
    expect(callbacks.sendOffer).toHaveBeenCalledTimes(2);
    expect(callbacks.onDataState).not.toHaveBeenCalledWith(peerId, "closed");
  });

  it("recria somente o peer quando uma offer antiga viola a ordem de m-lines", async () => {
    const callbacks = events();
    const peerId = "00000000-0000-4000-8000-000000000002";
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect(peerId, false);
    FakePeerConnection.failMLineOrderOnce = true;

    await transport.acceptOffer(peerId, { type: "offer", sdp: "stale-generation-offer" });

    expect(FakePeerConnection.instances).toHaveLength(2);
    expect(callbacks.sendAnswer).toHaveBeenCalledOnce();
    expect(transport.getDiagnostics()).toHaveLength(1);
  });

  it("abre um DataChannel por peer e entrega mensagens sem servidor", async () => {
    const callbacks = { ...events(), onDataMessage: vi.fn(), onDataState: vi.fn() };
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect("00000000-0000-4000-8000-000000000002", true);
    const channel = FakePeerConnection.dataChannels[0]!; channel.open();
    expect(transport.sendData('{"type":"chat.message"}')).toBe(1);
    expect(channel.sent).toEqual(['{"type":"chat.message"}']);
    channel.receive("mensagem recebida");
    expect(callbacks.onDataMessage).toHaveBeenCalledWith("00000000-0000-4000-8000-000000000002", "mensagem recebida");
    await transport.disconnect();
    expect(channel.readyState).toBe("closed");
  });

  it("não envia em DataChannel congestionado", async () => {
    const callbacks = { ...events(), onDataMessage: vi.fn(), onDataState: vi.fn() };
    const transport = new MeshWebRTCTransport("00000000-0000-4000-8000-000000000001", [], callbacks);
    await transport.connect("00000000-0000-4000-8000-000000000002", true);
    const channel = FakePeerConnection.dataChannels[0]!;
    channel.open();
    channel.bufferedAmount = 600 * 1024;
    expect(transport.sendData("mensagem")).toBe(0);
    expect(channel.sent).toHaveLength(0);
  });
});

function events() {
  return {
    sendOffer: vi.fn(), sendAnswer: vi.fn(), sendIce: vi.fn(),
    onRemoteStream: vi.fn(), onConnectionState: vi.fn(),
  };
}
