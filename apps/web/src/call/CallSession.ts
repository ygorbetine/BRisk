import type { CallTransportPreference } from "@briks/rtc";
import type {
  GroupRevocationCertificate,
  LocalIdentity,
  PublicPeerIdentity,
} from "../services/offline/social-storage";

export type CallJoinOptions = {
  identity?: LocalIdentity;
  trustedPeers?: PublicPeerIdentity[];
  revokedPeers?: PublicPeerIdentity[];
  revocations?: GroupRevocationCertificate[];
  groupId?: string;
  /** Chamadas pertencentes a grupos nunca podem cair no modo anônimo. */
  requireIdentityAuthentication?: boolean;
  rendezvousId?: string;
  /** `auto` mantém Mesh até quatro participantes e prefere SFU acima disso. */
  transportPreference?: CallTransportPreference;
};

/** Identidade e geração da sessão, isoladas do gerenciamento de mídia. */
export class CallSession {
  lifecycleId = 0;
  roomId?: string;
  rendezvousId?: string;
  peerId?: string;

  begin(roomId: string, peerId: string, rendezvousId = roomId): number {
    this.lifecycleId += 1;
    this.roomId = roomId;
    this.rendezvousId = rendezvousId;
    this.peerId = peerId;
    return this.lifecycleId;
  }

  end(): void {
    this.lifecycleId += 1;
    this.roomId = undefined;
    this.rendezvousId = undefined;
    this.peerId = undefined;
  }

  isActive(lifecycleId: number, resourcesReady: boolean): boolean {
    return lifecycleId === this.lifecycleId && Boolean(this.roomId && resourcesReady);
  }
}
