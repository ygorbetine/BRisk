import { MeshWebRTCTransport } from "@briks/rtc";
import { api, resetApiRuntimeConfig } from "../infrastructure/briks-gateway";
import { CallController } from "../call";
import { ChatController } from "../chat";
import { BackgroundChatManager } from "../services/chat/background-manager";
import { resetChatStorageRuntime } from "../services/offline/chat-storage";
import { resetSocialStorageRuntime } from "../services/offline/social-storage";
import type { InviteDependencies } from "../services/invites/service";
import { SupabaseSignalingProvider } from "../services/supabase/signaling";
import { VoiceActivityDirectory } from "../services/supabase/voice-activity";
import { loadRtcNetworkContext } from "../services/network/runtime";
import { InviteApplicationService } from "./InviteApplicationService";
import { BriksApplication } from "./BriksApplication";
import { SessionService } from "./SessionService";

const createSignaling = () => new SupabaseSignalingProvider();
const createChat = () => new ChatController(createSignaling);

const resilientDesktopInviteDependencies: InviteDependencies = {
  createSignaling,
  createTransport: async (peerId, iceServers, events) => {
    const network = await loadRtcNetworkContext();
    return new MeshWebRTCTransport(peerId, iceServers, {
      ...events,
      // O transporte tenta ICE restart antes de o caso de uso interpretar a
      // conexão como encerrada definitivamente.
      onConnectionState: (remotePeerId, state) => {
        if (state === "failed") return;
        events.onConnectionState(remotePeerId, state);
      },
    }, {
      networkInterfaces: network.networkInterfaces,
      networkPreference: network.preference,
    });
  },
  now: () => Date.now(),
  setTimer: (callback, delay) => setTimeout(callback, delay),
  clearTimer: (timer) => clearTimeout(timer),
};

export function createBriksApplication(): BriksApplication {
  return new BriksApplication({
    gateway: api,
    call: new CallController(createSignaling, api),
    chat: createChat(),
    callChat: createChat(),
    backgroundChats: new BackgroundChatManager(createChat),
    voiceActivities: new VoiceActivityDirectory(),
    session: new SessionService(api),
    invites: new InviteApplicationService(
      api,
      resilientDesktopInviteDependencies,
      () => Boolean(window.desktop?.getBackendConfig),
    ),
    resetRuntimeAdapters: () => {
      resetApiRuntimeConfig();
      resetChatStorageRuntime();
      resetSocialStorageRuntime();
    },
  });
}

export const briksApplication = createBriksApplication();
