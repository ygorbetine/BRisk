import type { RtcNetworkPreference } from "@briks/rtc";

export type NetworkPreference = RtcNetworkPreference;

export type NetworkSettings = {
  preference: RtcNetworkPreference;
};

const STORAGE_KEY = "risk.network-settings.v1";
export const NETWORK_SETTINGS_EVENT = "briks:network-settings";

export const DEFAULT_NETWORK_SETTINGS: NetworkSettings = {
  preference: "auto",
};

export function loadNetworkSettings(): NetworkSettings {
  try {
    const value = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? "null") as Partial<NetworkSettings> | null;
    return { preference: normalizeNetworkPreference(value?.preference) };
  } catch {
    return { ...DEFAULT_NETWORK_SETTINGS };
  }
}

export function saveNetworkSettings(settings: NetworkSettings): NetworkSettings {
  const normalized = { preference: normalizeNetworkPreference(settings.preference) };
  localStorage.setItem(STORAGE_KEY, JSON.stringify(normalized));
  window.dispatchEvent(new CustomEvent<NetworkSettings>(NETWORK_SETTINGS_EVENT, { detail: normalized }));
  return normalized;
}

export function normalizeNetworkPreference(value: unknown): RtcNetworkPreference {
  return value === "internet-direct" || value === "private-vpn" ? value : "auto";
}

export function networkPreferenceLabel(preference: RtcNetworkPreference): string {
  if (preference === "internet-direct") return "Internet direta";
  if (preference === "private-vpn") return "VPN privada quando disponível";
  return "Automático";
}
