import { afterAll, afterEach, beforeAll, describe, expect, it, vi } from "vitest";
import { CallTransportRegistry, type MeshCallTransport } from "@briks/rtc";
import type { PeerState } from "@briks/protocol";
import type { LocalGroup } from "./services/offline/social-storage";
import { InMemorySignalingHub, InMemorySignalingProvider } from "./services/signaling/in-memory";
import { useCallStore } from "./store";

const callRuntime = vi.hoisted(() => ({ groups: [] as LocalGroup[] }));

vi.mock("./services/offline/social-storage", async () => {
  const actual = await vi.importActual<typeof import("./services/offline/social-storage")>("./services/offline/social-storage");
  return { ...actual, loadLocalGroups: vi.fn(async () => callRuntime.groups) };
});

type ReconcileRemoteMediaState = typeof import("./call")["reconcileRemoteMediaState"];
let reconcileRemoteMediaState: ReconcileRemoteMediaState;
let parseCallProfileMessage: typeof import("./call")["parseCallProfileMessage"];
let sameCallPublicKey: typeof import("./call")["sameCallPublicKey"];
let callConnectionRecoveryMessage: typeof import("./call")["callConnectionRecoveryMessage"];
let CallController: typeof import("./call")["CallController"];

function fakeStream(id: string, video = true): MediaStream {
  return {
    id,
    getVideoTracks: () => video ? [{} as MediaStreamTrack] : [],
  } as unknown as MediaStream;
}

beforeAll(async () => {
  const values = new Map<string, string>();
  vi.stubGlobal("sessionStorage", {
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value); },
    removeItem: (key: string) => { values.delete(key); },
    clear: () => { values.clear(); },
  });
  vi.stubGlobal("MediaStream", class {
    getTracks(): MediaStreamTrack[] { return []; }
  });
  ({ CallController, reconcileRemoteMediaState, parseCallProfileMessage, sameCallPublicKey, callConnectionRecoveryMessage } = await import("./call"));
});

afterAll(() => vi.unstubAllGlobals());
afterEach(() => vi.useRealTimers());

describe("reconcileRemoteMediaState", () => {
  it("mapeia screen share quando o MediaStream.id remoto difere do id anunciado", () => {
    const screen = fakeStream("firefox-screen-stream");
    const state: PeerState = {
      microphone: true,
      camera: false,
      screenShare: true,
      screenAudio: true,
      screenStreamId: "chromium-screen-stream",
    };

    const result = reconcileRemoteMediaState({ [screen.id]: screen }, state);
    expect(result.screenStreamId).toBe(screen.id);
  });

  it("distingue câmera e tela quando ambos estão ativos", () => {
    const camera = fakeStream("remote-main-stream");
    const screen = fakeStream("remote-screen-stream");
    const state: PeerState = {
      microphone: true,
      camera: true,
      screenShare: true,
      cameraStreamId: "sender-main-stream",
      screenStreamId: "sender-screen-stream",
    };

    const result = reconcileRemoteMediaState(
      { [camera.id]: camera, [screen.id]: screen },
      state,
    );
    expect(result.cameraStreamId).toBe(camera.id);
    expect(result.screenStreamId).toBe(screen.id);
  });
});

describe("perfil da chamada via DataChannel", () => {
  it("reconhece a mesma chave pública independentemente da ordem dos campos", () => {
    const stored: JsonWebKey = { kty: "EC", crv: "P-256", x: "abc", y: "def", ext: true, key_ops: ["verify"] };
    const received: JsonWebKey = { key_ops: ["verify"], y: "def", x: "abc", crv: "P-256", kty: "EC", ext: true };
    expect(JSON.stringify(stored)).not.toBe(JSON.stringify(received));
    expect(sameCallPublicKey(stored, received)).toBe(true);
  });

  it("aceita nome e avatar de imagem com envelope válido", () => {
    const avatar = "data:image/png;base64,AA==";
    expect(parseCallProfileMessage(JSON.stringify({
      version: 1,
      type: "call.profile",
      payload: { displayName: "Maria", avatar },
    }))).toEqual({ version: 1, type: "call.profile", payload: { displayName: "Maria", avatar } });
  });

  it("recusa URLs externas, SVG e payload excessivo", () => {
    for (const avatar of ["https://example.com/avatar.png", "data:image/svg+xml;base64,PHN2Zz4="]) {
      expect(parseCallProfileMessage(JSON.stringify({ version: 1, type: "call.profile", payload: { displayName: "Maria", avatar } }))).toBeNull();
    }
    expect(parseCallProfileMessage(JSON.stringify({
      version: 1,
      type: "call.profile",
      payload: { displayName: "Maria", avatar: `data:image/png;base64,${"A".repeat(70_000)}` },
    }))).toBeNull();
  });
});

describe("recuperação da conexão da chamada", () => {
  it("não diagnostica falta de TURN quando existe uma interface ZeroTier", () => {
    const message = callConnectionRecoveryMessage(false, [{ provider: "zerotier" }]);
    expect(message).toContain("ZeroTier");
    expect(message).toContain("Tentando restabelecer automaticamente");
    expect(message).not.toContain("configure TURN");
  });

  it("mantém a recomendação de TURN somente para conexões sem VPN privada", () => {
    expect(callConnectionRecoveryMessage(false, [{ provider: "unknown" }])).toContain("configure TURN");
  });

  it("expõe no diagnóstico quando o grupo deveria usar SFU", () => {
    const controller = new CallController();
    const internals = controller as unknown as {
      expectedParticipantCount: number;
      transport: { kind: "mesh"; getDiagnostics(): [] };
    };
    internals.expectedParticipantCount = 9;
    internals.transport = { kind: "mesh", getDiagnostics: () => [] };

    expect(controller.getDiagnostics().transport).toMatchObject({
      active: "mesh",
      selected: "mesh",
      recommended: "sfu",
      reason: "sfu-unavailable",
    });
  });

  it("cleans up signaling and transport when call initialization fails", async () => {
    const signaling = new InMemorySignalingProvider(new InMemorySignalingHub());
    const disconnect = vi.spyOn(signaling, "disconnect");
    const leave = vi.fn(async () => undefined);
    const transport = {
      kind: "mesh",
      join: vi.fn(async () => { throw new Error("transport initialization failed"); }),
      leave,
      connectPeer: vi.fn(async () => undefined),
      disconnectPeer: vi.fn(async () => undefined),
      recoverPeer: vi.fn(async () => undefined),
      publishTrack: vi.fn(async () => undefined),
      configurePublishedVideoTrack: vi.fn(async () => undefined),
      unpublishTrack: vi.fn(async () => undefined),
      replaceTrack: vi.fn(async () => undefined),
      replacePublishedTrack: vi.fn(async () => undefined),
      sendData: vi.fn(() => 0),
      requireMediaAuthorization: vi.fn(),
      authorizePeerMedia: vi.fn(async () => undefined),
      revokePeerMedia: vi.fn(),
      getDiagnostics: vi.fn(() => []),
      collectDiagnostics: vi.fn(async () => []),
      acceptOffer: vi.fn(async () => undefined),
      acceptAnswer: vi.fn(async () => undefined),
      addIceCandidate: vi.fn(async () => undefined),
      restartIce: vi.fn(async () => undefined),
    } satisfies MeshCallTransport;
    const registry = new CallTransportRegistry({ mesh: () => transport });
    const controller = new CallController(
      () => signaling,
      { me: vi.fn(async () => { throw new Error("profile must not be requested"); }) },
      registry,
    );

    await expect(controller.join("token", "room_cleanup_12345678", [])).rejects.toThrow("transport initialization failed");

    expect(disconnect).toHaveBeenCalledOnce();
    expect(leave).toHaveBeenCalledOnce();
    expect(controller.getDiagnostics().signaling).toBeNull();
  });

  it("does not allow SDP from a ninth Mesh participant to bypass the hard limit", () => {
    const localPeerId = "peer_capacity_00000000";
    let presencePeers = Array.from({ length: 8 }, (_, index) => `peer_capacity_${String(index + 1).padStart(8, "0")}`);
    const disconnectPeer = vi.fn(async () => undefined);
    const controller = new CallController();
    const internals = controller as unknown as {
      peerId: string;
      signaling: { getDiagnostics(): { presencePeers: string[] } };
      transport: { kind: "mesh"; getDiagnostics(): Array<{ peerId: string }>; disconnectPeer: typeof disconnectPeer };
      enforceMeshCapacity(candidatePeerId?: string): boolean;
    };
    internals.peerId = localPeerId;
    internals.signaling = { getDiagnostics: () => ({ presencePeers }) };
    internals.transport = {
      kind: "mesh",
      getDiagnostics: () => presencePeers.map((peerId) => ({ peerId })),
      disconnectPeer,
    };

    expect(internals.enforceMeshCapacity(presencePeers.at(-1))).toBe(false);
    expect(useCallStore.getState().error).toContain("limite seguro de 8 participantes");

    presencePeers = presencePeers.slice(0, 7);
    expect(internals.enforceMeshCapacity()).toBe(true);
    expect(useCallStore.getState().error).toBeNull();
  });
});

describe("sincronização de membros durante a chamada", () => {
  it("conecta peers presentes que se tornaram confiáveis após atualizar o grupo", async () => {
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const peerB = "00000000-0000-4000-8000-000000000002";
    const peerC = "00000000-0000-4000-8000-000000000003";
    const groupId = "group_call_members_12345678";
    const roomId = "room_call_members_12345678";
    callRuntime.groups = [{
      groupId,
      name: "Grupo da chamada",
      channels: [],
      members: [localPeerId, peerB, peerC].map((peerId) => ({ peerId, displayName: peerId, publicKey: { kty: "EC" } })),
      ownerPeerId: localPeerId,
      membershipVersion: 3,
      manifestVersion: 3,
      administratorPeerIds: [],
      removedPeerIds: [],
      removedMembers: [],
      joinedAt: Date.now(),
    }];
    const connectPeer = vi.fn(async () => undefined);
    const reconnectSignaling = vi.fn(async () => undefined);
    const sendPeerState = vi.fn(async () => undefined);
    const signaling = {
      connect: reconnectSignaling,
      getDiagnostics: () => ({ presencePeers: [peerB, peerC] }),
      sendPeerState,
    };
    const controller = new CallController();
    const internals = controller as unknown as {
      lifecycleId: number;
      roomId: string;
      rendezvousId: string;
      groupId: string;
      peerId: string;
      signaling: typeof signaling;
      transport: { connectPeer: typeof connectPeer };
      transportReady: boolean;
      refreshGroupSecurity(): Promise<void>;
    };
    internals.lifecycleId = 1;
    internals.roomId = roomId;
    internals.rendezvousId = roomId;
    internals.groupId = groupId;
    internals.peerId = localPeerId;
    internals.signaling = signaling;
    internals.transport = { connectPeer };
    internals.transportReady = true;

    await internals.refreshGroupSecurity();

    expect(connectPeer).toHaveBeenCalledTimes(2);
    expect(connectPeer).toHaveBeenCalledWith(peerB, true);
    expect(connectPeer).toHaveBeenCalledWith(peerC, true);
    expect(sendPeerState).toHaveBeenCalledOnce();
  });

  it("recupera separadamente os três peers de uma chamada de quatro pessoas quando o DataChannel não abre", async () => {
    vi.useFakeTimers();
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const remotePeerIds = [
      "00000000-0000-4000-8000-000000000002",
      "00000000-0000-4000-8000-000000000003",
      "00000000-0000-4000-8000-000000000004",
    ];
    const recoverPeer = vi.fn(async () => undefined);
    const transport = {
      sendData: vi.fn(() => 0),
      recoverPeer,
      disconnectPeer: vi.fn(async () => undefined),
      connectPeer: vi.fn(async () => undefined),
    };
    const signaling = {
      getDiagnostics: () => ({ presencePeers: remotePeerIds }),
    };
    const controller = new CallController();
    const internals = controller as unknown as {
      lifecycleId: number;
      roomId: string;
      peerId: string;
      signaling: typeof signaling;
      transport: typeof transport;
      identity: { peerId: string };
      mediaAuthenticationRequired: boolean;
      trustedPeers: Map<string, { peerId: string }>;
      authTimers: Map<string, ReturnType<typeof setTimeout>>;
      schedulePeerAuthenticationCheck(peerId: string, delayMs: number): void;
    };
    internals.lifecycleId = 1;
    internals.roomId = "room-four-peers";
    internals.peerId = localPeerId;
    internals.signaling = signaling;
    internals.transport = transport;
    internals.identity = { peerId: localPeerId };
    internals.mediaAuthenticationRequired = true;
    remotePeerIds.forEach((peerId) => internals.trustedPeers.set(peerId, { peerId }));
    remotePeerIds.forEach((peerId) => internals.schedulePeerAuthenticationCheck(peerId, 20_000));

    await vi.advanceTimersByTimeAsync(20_000);

    expect(recoverPeer).toHaveBeenCalledTimes(3);
    remotePeerIds.forEach((peerId) => expect(recoverPeer).toHaveBeenCalledWith(peerId));
    internals.authTimers.forEach((timer) => clearTimeout(timer));
  });

  it("repete o desafio antes de recriar uma conexão autenticada incompleta", async () => {
    vi.useFakeTimers();
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const remotePeerId = "00000000-0000-4000-8000-000000000002";
    const recoverPeer = vi.fn(async () => undefined);
    const transport = {
      sendData: vi.fn(() => 1),
      recoverPeer,
      disconnectPeer: vi.fn(async () => undefined),
      connectPeer: vi.fn(async () => undefined),
    };
    const signaling = { getDiagnostics: () => ({ presencePeers: [remotePeerId] }) };
    const controller = new CallController();
    const internals = controller as unknown as {
      lifecycleId: number;
      roomId: string;
      peerId: string;
      signaling: typeof signaling;
      transport: typeof transport;
      identity: { peerId: string };
      mediaAuthenticationRequired: boolean;
      trustedPeers: Map<string, { peerId: string }>;
      authTimers: Map<string, ReturnType<typeof setTimeout>>;
      sendAuthChallenge(peerId: string): boolean;
    };
    internals.lifecycleId = 1;
    internals.roomId = "room-auth-retry";
    internals.peerId = localPeerId;
    internals.signaling = signaling;
    internals.transport = transport;
    internals.identity = { peerId: localPeerId };
    internals.mediaAuthenticationRequired = true;
    internals.trustedPeers.set(remotePeerId, { peerId: remotePeerId });

    expect(internals.sendAuthChallenge(remotePeerId)).toBe(true);
    await vi.advanceTimersByTimeAsync(24_000);

    expect(transport.sendData).toHaveBeenCalledTimes(3);
    expect(recoverPeer).toHaveBeenCalledOnce();
    expect(recoverPeer).toHaveBeenCalledWith(remotePeerId);
    internals.authTimers.forEach((timer) => clearTimeout(timer));
  });

  it("recria rapidamente o peer quando o DataChannel fecha depois da autenticação", async () => {
    vi.useFakeTimers();
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const remotePeerId = "00000000-0000-4000-8000-000000000002";
    const recoverPeer = vi.fn(async () => undefined);
    const transport = {
      sendData: vi.fn(() => 0),
      recoverPeer,
      disconnectPeer: vi.fn(async () => undefined),
      connectPeer: vi.fn(async () => undefined),
    };
    const signaling = {
      getDiagnostics: () => ({ presencePeers: [remotePeerId] }),
      sendPeerState: vi.fn(async () => undefined),
    };
    const controller = new CallController();
    const internals = controller as unknown as {
      lifecycleId: number;
      roomId: string;
      peerId: string;
      signaling: typeof signaling;
      transport: typeof transport;
      identity: { peerId: string };
      mediaAuthenticationRequired: boolean;
      trustedPeers: Map<string, { peerId: string }>;
      authenticatedPeers: Set<string>;
      authTimers: Map<string, ReturnType<typeof setTimeout>>;
      handleDataChannelState(peerId: string, state: RTCDataChannelState): void;
    };
    internals.lifecycleId = 1;
    internals.roomId = "room-authenticated-channel";
    internals.peerId = localPeerId;
    internals.signaling = signaling;
    internals.transport = transport;
    internals.identity = { peerId: localPeerId };
    internals.mediaAuthenticationRequired = true;
    internals.trustedPeers.set(remotePeerId, { peerId: remotePeerId });
    internals.authenticatedPeers.add(remotePeerId);

    internals.handleDataChannelState(remotePeerId, "closed");
    expect(internals.authenticatedPeers.has(remotePeerId)).toBe(false);
    expect(recoverPeer).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(500);

    expect(recoverPeer).toHaveBeenCalledOnce();
    expect(recoverPeer).toHaveBeenCalledWith(remotePeerId);
    internals.authTimers.forEach((timer) => clearTimeout(timer));
  });
});
