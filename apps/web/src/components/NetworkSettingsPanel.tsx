import { useEffect, useMemo, useState } from "react";
import { Network, RefreshCw, ShieldCheck, Wifi } from "lucide-react";
import {
  loadNetworkSettings,
  saveNetworkSettings,
  type NetworkPreference,
} from "../services/network/settings";
import "./network-settings.css";

const PREFERENCE_OPTIONS: Array<{
  value: NetworkPreference;
  title: string;
  description: string;
}> = [
  { value: "auto", title: "Automático", description: "Deixa o WebRTC escolher entre VPN, conexão direta e TURN." },
  { value: "internet-direct", title: "Internet direta", description: "Ignora candidatos dos adaptadores VPN e mantém STUN/TURN disponíveis." },
  { value: "private-vpn", title: "VPN privada quando disponível", description: "Prioriza ZeroTier, Tailscale ou WireGuard e usa a internet como fallback." },
];

function providerLabel(provider: BriksDesktopNetworkInterface["provider"]): string {
  if (provider === "zerotier") return "ZeroTier";
  if (provider === "tailscale") return "Tailscale";
  if (provider === "wireguard") return "WireGuard";
  return "VPN privada";
}

export function NetworkSettingsPanel() {
  const [preference, setPreference] = useState<NetworkPreference>(() => loadNetworkSettings().preference);
  const [interfaces, setInterfaces] = useState<BriksDesktopNetworkInterface[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string>();
  const vpnInterfaces = useMemo(() => interfaces.filter((item) => item.provider !== "unknown"), [interfaces]);

  async function refresh(): Promise<void> {
    if (!window.desktop?.getNetworkInterfaces) {
      setInterfaces([]);
      setError("A detecção de VPN está disponível no aplicativo desktop.");
      return;
    }
    setLoading(true);
    setError(undefined);
    try {
      setInterfaces(await window.desktop.getNetworkInterfaces());
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível consultar as interfaces de rede.");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => { void refresh(); }, []);

  function select(next: NetworkPreference): void {
    setPreference(next);
    saveNetworkSettings({ preference: next });
  }

  return <div className="network-settings">
    <section className="network-settings-section">
      <header><Network/><span><strong>Preferência de conexão</strong><small>A preferência vale para novas chamadas, chats, convites e transferências P2P.</small></span></header>
      <div className="network-preference-list">
        {PREFERENCE_OPTIONS.map((option) => <label key={option.value} className={preference === option.value ? "active" : ""}>
          <input type="radio" name="network-preference" checked={preference === option.value} onChange={() => select(option.value)}/>
          <span><strong>{option.title}</strong><small>{option.description}</small></span>
        </label>)}
      </div>
    </section>

    <section className="network-settings-section">
      <header><ShieldCheck/><span><strong>Redes privadas detectadas</strong><small>Os endereços permanecem neste dispositivo e não são publicados no diagnóstico.</small></span></header>
      <div className="vpn-interface-list">
        {vpnInterfaces.map((networkInterface) => <article key={`${networkInterface.name}:${networkInterface.address}`}>
          <span className="vpn-interface-icon"><Wifi/></span>
          <span><strong>{providerLabel(networkInterface.provider)}</strong><small>{networkInterface.name}</small></span>
          <code>{networkInterface.address}</code>
          <em>Detectado ✓</em>
        </article>)}
        {!vpnInterfaces.length && !loading && <div className="vpn-interface-empty">Nenhuma interface ZeroTier, Tailscale ou WireGuard foi detectada.</div>}
        {loading && <div className="vpn-interface-empty">Verificando interfaces de rede…</div>}
      </div>
      <button type="button" className="settings-secondary-button" disabled={loading} onClick={() => void refresh()}>
        <RefreshCw size={16}/>{loading ? "Atualizando…" : "Atualizar detecção"}
      </button>
      {error && <div className="settings-device-error">{error}</div>}
    </section>

    <div className="settings-note">A opção de VPN aumenta a prioridade do caminho privado quando ele funciona nos dois computadores. Se a VPN estiver indisponível, o Briks ainda pode usar conexão direta, STUN ou TURN.</div>
  </div>;
}
