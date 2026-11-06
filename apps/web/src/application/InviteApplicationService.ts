import {
  FriendInviteService,
  GroupInviteService,
  type InviteDependencies,
  type InviteService,
} from "../services/invites/service";
import type { InviteType } from "../services/invites/code";
import { getOrCreateLocalIdentity, type PublicGroupMetadata } from "../services/offline/social-storage";
import { resolveStaticIceConfiguration } from "../services/rtc/ice";
import type { BriksGateway } from "./contracts";

export type CreateInviteServiceOptions = {
  type: InviteType;
  token: string;
  displayName: string;
  group?: PublicGroupMetadata;
};

/** Caso de uso que esconde da UI como identidade, ICE e transporte são obtidos. */
export class InviteApplicationService {
  constructor(
    private readonly gateway: Pick<BriksGateway, "turnCredentials">,
    private readonly desktopDependencies: InviteDependencies,
    private readonly isDesktop: () => boolean,
  ) {}

  async create(options: CreateInviteServiceOptions): Promise<InviteService> {
    const desktop = this.isDesktop();
    const [identity, iceServers] = await Promise.all([
      getOrCreateLocalIdentity(options.displayName),
      desktop
        ? Promise.resolve(resolveStaticIceConfiguration().iceServers)
        : this.gateway.turnCredentials(options.token).then((result) => result.iceServers),
    ]);
    const dependencies = desktop ? this.desktopDependencies : undefined;
    return options.type === "friend"
      ? new FriendInviteService(identity, iceServers, dependencies)
      : new GroupInviteService(identity, iceServers, dependencies);
  }
}
