import type { CallController } from "../call";
import type { ChatController } from "../chat";
import type { BackgroundChatManager } from "../services/chat/background-manager";
import type { VoiceActivityDirectory } from "../services/supabase/voice-activity";
import type { InviteApplicationService } from "./InviteApplicationService";
import type { SessionService } from "./SessionService";
import type { BriksGateway } from "./contracts";

export type BriksApplicationDependencies = {
  gateway: BriksGateway;
  call: CallController;
  chat: ChatController;
  callChat: ChatController;
  backgroundChats: BackgroundChatManager;
  voiceActivities: VoiceActivityDirectory;
  session: SessionService;
  invites: InviteApplicationService;
  resetRuntimeAdapters(): void;
};

/** Única superfície entregue à camada React pelo composition root. */
export class BriksApplication {
  readonly gateway: BriksGateway;
  readonly call: CallController;
  readonly chat: ChatController;
  readonly callChat: ChatController;
  readonly backgroundChats: BackgroundChatManager;
  readonly voiceActivities: VoiceActivityDirectory;
  readonly session: SessionService;
  readonly invites: InviteApplicationService;
  private readonly resetAdapters: () => void;

  constructor(dependencies: BriksApplicationDependencies) {
    this.gateway = dependencies.gateway;
    this.call = dependencies.call;
    this.chat = dependencies.chat;
    this.callChat = dependencies.callChat;
    this.backgroundChats = dependencies.backgroundChats;
    this.voiceActivities = dependencies.voiceActivities;
    this.session = dependencies.session;
    this.invites = dependencies.invites;
    this.resetAdapters = dependencies.resetRuntimeAdapters;
  }

  backendChanged(): void {
    this.resetAdapters();
  }
}
