import { beforeEach, describe, expect, it, vi } from "vitest";
import type { InviteDependencies } from "../services/invites/service";
import type { BriksGateway } from "./contracts";

const mocks = vi.hoisted(() => ({
  identity: { peerId: "peer-local", displayName: "Sam", publicKey: {}, privateKey: {} },
  staticIce: [{ urls: "stun:static.example" }] as RTCIceServer[],
  constructions: [] as Array<{ kind: "friend" | "group"; args: unknown[] }>,
}));

vi.mock("../services/offline/social-storage", () => ({
  getOrCreateLocalIdentity: vi.fn(async () => mocks.identity),
}));
vi.mock("../services/rtc/ice", () => ({
  resolveStaticIceConfiguration: () => ({ iceServers: mocks.staticIce }),
}));
vi.mock("../services/invites/service", () => ({
  FriendInviteService: class {
    constructor(...args: unknown[]) { mocks.constructions.push({ kind: "friend", args }); }
  },
  GroupInviteService: class {
    constructor(...args: unknown[]) { mocks.constructions.push({ kind: "group", args }); }
  },
}));

import { InviteApplicationService } from "./InviteApplicationService";

beforeEach(() => mocks.constructions.splice(0));

function gateway(turnCredentials: BriksGateway["turnCredentials"]): Pick<BriksGateway, "turnCredentials"> {
  return { turnCredentials };
}

describe("InviteApplicationService", () => {
  it("no desktop usa ICE estático e as dependências resilientes", async () => {
    const turnCredentials = vi.fn<BriksGateway["turnCredentials"]>();
    const dependencies = {} as InviteDependencies;
    const service = new InviteApplicationService(gateway(turnCredentials), dependencies, () => true);

    await service.create({ type: "friend", token: "local", displayName: "Sam" });
    expect(turnCredentials).not.toHaveBeenCalled();
    expect(mocks.constructions).toEqual([{
      kind: "friend",
      args: [mocks.identity, mocks.staticIce, dependencies],
    }]);
  });

  it("no navegador obtém TURN pelo gateway e deixa o serviço usar seu adapter padrão", async () => {
    const remoteIce = [{ urls: "turn:relay.example" }] as RTCIceServer[];
    const turnCredentials = vi.fn(async () => ({ iceServers: remoteIce }));
    const service = new InviteApplicationService(gateway(turnCredentials), {} as InviteDependencies, () => false);

    await service.create({ type: "group", token: "token", displayName: "Sam" });
    expect(turnCredentials).toHaveBeenCalledWith("token");
    expect(mocks.constructions).toEqual([{
      kind: "group",
      args: [mocks.identity, remoteIce, undefined],
    }]);
  });
});
