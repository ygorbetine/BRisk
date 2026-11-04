import type { CallTransportKind, CallTransportPreference } from "./call-transport";

export const MESH_PREFERRED_MAX_PARTICIPANTS = 8;
export const MESH_HARD_MAX_PARTICIPANTS = 8;

export type CallNetworkHealth = {
  roundTripTimeMs?: number;
  jitterMs?: number;
  packetLossPercent?: number;
};

export type CallTransportDecisionReason =
  | "small-call"
  | "participant-threshold"
  | "degraded-network"
  | "manual-mesh"
  | "manual-sfu"
  | "sfu-unavailable";

export type CallTransportDecision = {
  selected: CallTransportKind;
  recommended: CallTransportKind;
  reason: CallTransportDecisionReason;
  participantCount: number;
  sfuAvailable: boolean;
  networkDegraded: boolean;
  meshCapacityExceeded: boolean;
  label: string;
};

export type SelectCallTransportOptions = {
  participantCount: number;
  preference?: CallTransportPreference;
  sfuAvailable: boolean;
  network?: CallNetworkHealth;
};

export function selectCallTransport(options: SelectCallTransportOptions): CallTransportDecision {
  if (!Number.isInteger(options.participantCount) || options.participantCount < 1) {
    throw new Error("participantCount precisa ser um inteiro maior que zero.");
  }
  const preference = options.preference ?? "auto";
  const networkDegraded = isCallNetworkDegraded(options.network);
  const scaleRequiresSfu = options.participantCount > MESH_PREFERRED_MAX_PARTICIPANTS;
  const recommended: CallTransportKind = scaleRequiresSfu || networkDegraded ? "sfu" : "mesh";

  if (preference === "mesh") {
    return decision("mesh", recommended, "manual-mesh", options, networkDegraded);
  }
  if (preference === "sfu") {
    return options.sfuAvailable
      ? decision("sfu", "sfu", "manual-sfu", options, networkDegraded)
      : decision("mesh", "sfu", "sfu-unavailable", options, networkDegraded);
  }
  if (recommended === "sfu" && !options.sfuAvailable) {
    return decision("mesh", "sfu", "sfu-unavailable", options, networkDegraded);
  }
  return decision(
    recommended,
    recommended,
    scaleRequiresSfu ? "participant-threshold" : networkDegraded ? "degraded-network" : "small-call",
    options,
    networkDegraded,
  );
}

export function isCallNetworkDegraded(network?: CallNetworkHealth): boolean {
  return (network?.roundTripTimeMs ?? 0) > 350
    || (network?.jitterMs ?? 0) > 60
    || (network?.packetLossPercent ?? 0) > 8;
}

function decision(
  selected: CallTransportKind,
  recommended: CallTransportKind,
  reason: CallTransportDecisionReason,
  options: SelectCallTransportOptions,
  networkDegraded: boolean,
): CallTransportDecision {
  const meshCapacityExceeded = options.participantCount > MESH_HARD_MAX_PARTICIPANTS;
  const label = selected === "sfu"
    ? "SFU"
    : reason === "manual-mesh"
      ? "Mesh P2P · seleção manual"
      : meshCapacityExceeded
        ? "Mesh P2P · limite excedido e SFU não configurado"
        : recommended === "sfu" && !options.sfuAvailable
          ? "Mesh P2P · SFU recomendado, mas não configurado"
          : recommended === "sfu"
            ? "Mesh P2P · migração para SFU recomendada"
            : "Mesh P2P";
  return {
    selected,
    recommended,
    reason,
    participantCount: options.participantCount,
    sfuAvailable: options.sfuAvailable,
    networkDegraded,
    meshCapacityExceeded,
    label,
  };
}
