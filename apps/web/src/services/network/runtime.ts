import type { NetworkInterfaceDescriptor, RtcNetworkPreference } from "@briks/rtc";
import { loadNetworkSettings } from "./settings";

export type RtcNetworkContext = {
  preference: RtcNetworkPreference;
  networkInterfaces: NetworkInterfaceDescriptor[];
};

export async function loadRtcNetworkContext(): Promise<RtcNetworkContext> {
  const preference = loadNetworkSettings().preference;
  if (typeof window === "undefined" || !window.desktop?.getNetworkInterfaces) {
    return { preference, networkInterfaces: [] };
  }
  try {
    return { preference, networkInterfaces: await window.desktop.getNetworkInterfaces() };
  } catch (error) {
    console.warn("Não foi possível consultar as interfaces locais para a política WebRTC.", error);
    return { preference, networkInterfaces: [] };
  }
}
