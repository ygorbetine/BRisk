import React, { useEffect, useMemo, useState } from "react";
import { useBriksApplication } from "../application/ApplicationContext";
import {
  ensureLocalGroup,
  getOrCreateLocalIdentity,
  loadLocalGroups,
  publicIdentity,
  type GroupInviteMetadata,
  type LocalGroup,
  type LocalGroupChannel,
  type PublicGroupMetadata,
} from "../services/offline/social-storage";
import { P2PInvitePanel } from "./P2PInvitePanel";

export function GroupInvitePanel({
  token,
  displayName,
  initialMode = "join",
  preferredGroupId,
  preferredGroupName,
  preferredGroupChannels,
  onComplete,
}: {
  token: string;
  displayName: string;
  initialMode?: "create" | "join";
  preferredGroupId?: string;
  preferredGroupName?: string;
  preferredGroupChannels?: LocalGroupChannel[];
  onComplete?(): void;
}) {
  const { gateway: api } = useBriksApplication();
  const [groups, setGroups] = useState<LocalGroup[]>([]);
  const preferredMetadata = useMemo<PublicGroupMetadata | undefined>(() => groups.find((group) => group.groupId === preferredGroupId), [groups, preferredGroupId]);
  const [selectedId, setSelectedId] = useState(preferredGroupId ?? "");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");

  useEffect(() => {
    let alive = true;

    void (async () => {
      setLoading(true);
      setError("");
      const currentIdentity = await getOrCreateLocalIdentity(displayName);
      let localGroups = await loadLocalGroups();

      if (preferredGroupId) {
        const existing = localGroups.find((group) => group.groupId === preferredGroupId);
        if (!existing) {
          let metadata: PublicGroupMetadata | undefined = existing;
          if (!metadata) {
            const communities = await api.communities(token);
            const community = communities.find((group) => group.id === preferredGroupId);
            if (!community) throw new Error("Grupo selecionado não foi encontrado.");
            const identity = await getOrCreateLocalIdentity(displayName);
            metadata = { groupId: preferredGroupId, name: preferredGroupName ?? community.name, channels: preferredGroupChannels ?? await api.channels(token, preferredGroupId).catch(() => []), ownerPeerId: identity.peerId, membershipVersion: 1, manifestVersion: 1, manifestActorPeerId: identity.peerId, manifestOperationId: crypto.randomUUID(), administratorEpoch: 1, administratorPeerIds: [], administratorGrants: [], removedPeerIds: [], removedMembers: [], revocations: [], rendezvousVersion: 1, rendezvousSecret: crypto.randomUUID() };
          }

          try {
            const identity = await getOrCreateLocalIdentity(displayName);
            await ensureLocalGroup(
              metadata.groupId,
              metadata.name,
              publicIdentity(identity),
              metadata.channels,
              metadata.ownerPeerId,
              metadata.membershipVersion,
              metadata.manifestVersion,
              metadata.removedPeerIds,
              metadata.administratorPeerIds,
              metadata.removedMembers,
              metadata.manifestActorPeerId,
              metadata.manifestOperationId,
              metadata.administratorEpoch,
              metadata.revocations,
              metadata.administratorGrants,
              metadata.rendezvousVersion,
              metadata.rendezvousSecret,
            );
            localGroups = await loadLocalGroups();
            window.dispatchEvent(new Event("briks:social-updated"));
          } catch (cause) {
            console.warn("Não foi possível preparar o grupo no armazenamento P2P antes do convite", cause);
          }
        }
      }

      if (!alive) return;
      const ownedGroups = initialMode === "create" ? localGroups.filter((group) => group.ownerPeerId === currentIdentity.peerId || (
        (group.administratorPeerIds ?? []).includes(currentIdentity.peerId)
        && (group.administratorGrants ?? []).some((grant) => grant.administratorPeerId === currentIdentity.peerId
          && grant.administratorEpoch === (group.administratorEpoch ?? 1))
      )) : localGroups;
      const available = preferredGroupId
        ? ownedGroups.filter((group) => group.groupId === preferredGroupId)
        : ownedGroups;
      setGroups(available);
      setSelectedId((current) => {
        if (preferredGroupId) return preferredGroupId;
        if (current && available.some((group) => group.groupId === current)) return current;
        return available[0]?.groupId ?? "";
      });
    })()
      .catch((cause) => {
        if (alive) setError(cause instanceof Error ? cause.message : "Falha ao carregar grupo para o convite");
      })
      .finally(() => {
        if (alive) setLoading(false);
      });

    return () => {
      alive = false;
    };
  }, [displayName, initialMode, preferredGroupChannels, preferredGroupId, preferredGroupName, token]);

  const selected = groups.find((group) => group.groupId === selectedId);
  const metadata: GroupInviteMetadata | undefined = selected
    ? {
        groupId: selected.groupId,
        name: selected.name,
        avatar: selected.avatar,
        channels: selected.channels,
        ownerPeerId: selected.ownerPeerId,
        membershipVersion: selected.membershipVersion,
        manifestVersion: selected.manifestVersion,
        manifestActorPeerId: selected.manifestActorPeerId,
        manifestOperationId: selected.manifestOperationId,
        administratorEpoch: selected.administratorEpoch,
        administratorPeerIds: selected.administratorPeerIds ?? [],
        administratorGrants: selected.administratorGrants ?? [],
        removedPeerIds: selected.removedPeerIds ?? [],
        removedMembers: (selected.removedMembers ?? []).map(({ avatar: _avatar, ...member }) => member),
        revocations: selected.revocations ?? [],
        rendezvousVersion: selected.rendezvousVersion ?? 1,
        rendezvousSecret: selected.rendezvousSecret ?? selected.groupId,
        ownerIdentity: selected.members.find((member) => member.peerId === selected.ownerPeerId) ?? selected.ownerIdentity,
        members: selected.members,
      }
    : preferredMetadata && selectedId === preferredMetadata.groupId
      ? preferredMetadata
      : undefined;

  return <div className="group-invite-panel">
    <label>Grupo para criar convite</label>
    <select
      value={selectedId}
      onChange={(event) => setSelectedId(event.target.value)}
      disabled={loading || Boolean(preferredGroupId) || (!preferredMetadata && groups.length === 0)}
    >
      <option value="">{loading ? "Carregando seus grupos…" : "Selecione um grupo"}</option>
      {preferredMetadata && !groups.some((group) => group.groupId === preferredMetadata.groupId) && (
        <option value={preferredMetadata.groupId}>{preferredMetadata.name}</option>
      )}
      {groups.map((group) => <option key={group.groupId} value={group.groupId}>{group.name}</option>)}
    </select>

    {!loading && !metadata && <p className="invite-notice">
      {error
        ? "Não foi possível preparar seus grupos para convites P2P."
        : "Você ainda não possui um grupo neste dispositivo. Crie ou entre em um grupo antes de gerar um convite."}
    </p>}
    {error && metadata && <div className="invite-notice">O armazenamento local será sincronizado quando o convite for concluído.</div>}
    {error && !metadata && <div className="invite-notice error">{error}</div>}

    <P2PInvitePanel
      type="group"
      token={token}
      displayName={displayName}
      group={metadata}
      initialMode={initialMode}
      onComplete={onComplete}
    />
  </div>;
}
