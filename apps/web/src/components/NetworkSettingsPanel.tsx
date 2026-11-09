import { useEffect, useMemo, useState } from "react";
import { Activity, RefreshCw, Wifi } from "lucide-react";
import { runConnectionTest, type ConnectionTestResult } from "../services/rtc/connection-test";
import {
  loadNetworkSettings,
  saveNetworkSettings,
  type NetworkPreference,
} from "../services/network/settings";
import { loadCallTransportMode, saveCallTransportMode } from "../services/call/transport-mode";
import { useBriksApplication } from "../application/ApplicationContext";
import { useCallStore } from "../store";
import "./network-settings.css";
import "./settings-ui.css";

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
  const [callMode, setCallMode] = useState(loadCallTransportMode);
  const [testing, setTesting] = useState(false);
  const [testResult, setTestResult] = useState<ConnectionTestResult>();
  const { gateway } = useBriksApplication();
  const token = useCallStore((state) => state.token);
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

  async function testConnection(): Promise<void> {
    setTesting(true);
    setTestResult(undefined);
    try {
      // Usa as credenciais TURN de verdade (temporárias, da conta); sem login, cai no STUN público.
      const iceServers = token ? await gateway.turnCredentials(token).then((result) => result.iceServers).catch(() => undefined) : undefined;
      setTestResult(await runConnectionTest({ iceServers }));
    }
    catch { setTestResult({ kinds: [], turnConfigured: false, level: "ruim", summary: "Não foi possível testar a conexão.", tips: [] }); }
    finally { setTesting(false); }
  }

  function select(next: NetworkPreference): void {
    setPreference(next);
    saveNetworkSettings({ preference: next });
  }

const CALL_MODES = [
  { value: "auto", title: "Automático", description: "Conexão direta; passa para o servidor em sala grande ou rede ruim." },
  { value: "mesh", title: "Sempre P2P", description: "Conexão direta entre todos, até 8 pessoas. Sem servidor no meio." },
  { value: "sfu", title: "Sempre SFU", description: "Passa por um servidor. Melhor para salas grandes e redes difíceis." },
] as const;

const PREFERENCE_SHORT: Record<NetworkPreference, string> = { auto: "Automática", "internet-direct": "Internet direta", "private-vpn": "VPN privada" };

  const [networksOpen, setNetworksOpen] = useState(false);
  const levelClass = testResult ? (testResult.level === "boa" ? "good" : testResult.level === "limitada" ? "warn" : "bad") : "";
  const levelText = testResult ? (testResult.level === "boa" ? "Conexão boa" : testResult.level === "limitada" ? "Conexão limitada" : "Conexão com problemas") : "Conexão não testada";
  const kindsText = testResult
    ? (testResult.kinds.length ? testResult.kinds.map((kind) => kind === "srflx" ? "direto (STUN)" : kind === "relay" ? "relay (TURN)" : "rede local").join(", ") : "nenhum")
    : "";

  return <div className="sx-stack network-settings">
    <section className={`sx-card ${testResult ? levelClass : "accent"}`}>
      <div className="sx-row">
        <div>
          <strong><span className={`sx-dot ${levelClass}`}/>{levelText}</strong>
          <small>{testResult ? testResult.summary : "Faça antes de entrar numa chamada: verifica se a rede permite conexão direta, se o relay (TURN) responde e mede atraso e perda. Leva uns 15 segundos."}</small>
        </div>
        <button type="button" className="sx-btn primary" disabled={testing} onClick={() => void testConnection()}>
          <Activity size={16}/>{testing ? "Testando…" : testResult ? "Testar de novo" : "Testar conexão"}
        </button>
      </div>
      {testResult && <>
        {testResult.tips.map((tip) => <p className="sx-sub" key={tip}>{tip}</p>)}
        <p className="sx-sub">Caminhos detectados: {kindsText}</p>
        {testResult.metrics && <div className="net-metrics">
          <span><small>Até o STUN</small><strong>{testResult.metrics.stunMs !== undefined ? `${testResult.metrics.stunMs} ms` : "sem resposta"}</strong></span>
          <span><small>Ida e volta pelo relay</small><strong>{testResult.metrics.relayRttMs !== undefined ? `${testResult.metrics.relayRttMs} ms` : "—"}</strong></span>
          <span><small>Variação</small><strong>{testResult.metrics.relayJitterMs !== undefined ? `${testResult.metrics.relayJitterMs} ms` : "—"}</strong></span>
          <span><small>Perda</small><strong>{testResult.metrics.relayLossPct !== undefined ? `${testResult.metrics.relayLossPct}%` : "—"}</strong></span>
          <span><small>Precisa de relay</small><strong>{testResult.needsRelay ? "Sim" : "Não"}</strong></span>
        </div>}
      </>}
    </section>

    <section className="sx-card">
      <span className="sx-title">Modo da chamada</span>
      <div className="sx-options cols-3" role="radiogroup" aria-label="Modo da chamada">
        {CALL_MODES.map((mode) => <button
          key={mode.value}
          type="button"
          role="radio"
          aria-checked={callMode === mode.value}
          className={`sx-option${callMode === mode.value ? " on" : ""}`}
          onClick={() => { setCallMode(mode.value); saveCallTransportMode(mode.value); }}
        ><strong>{mode.title}</strong><small>{mode.description}</small></button>)}
      </div>
      <p className="sx-sub">Todos da sala precisam usar o mesmo modo.</p>
    </section>

    <section className="sx-card">
      <span className="sx-title">Preferência de conexão</span>
      <div className="sx-seg" role="group" aria-label="Preferência de conexão">
        {PREFERENCE_OPTIONS.map((option) => <button key={option.value} type="button" aria-pressed={preference === option.value} className={preference === option.value ? "on" : ""} onClick={() => select(option.value)}>{PREFERENCE_SHORT[option.value]}</button>)}
      </div>
      <p className="sx-sub">{PREFERENCE_OPTIONS.find((option) => option.value === preference)?.description} Vale para novas chamadas, chats, convites e transferências.</p>
    </section>

    <section className="sx-card sx-fold">
      <button type="button" className="sx-fold-head" aria-expanded={networksOpen} onClick={() => setNetworksOpen((open) => !open)}>
        <span>Redes privadas detectadas <small>{loading ? "verificando…" : vpnInterfaces.length ? `${vpnInterfaces.length} encontrada${vpnInterfaces.length > 1 ? "s" : ""}` : "nenhuma"}</small></span>
        <span aria-hidden="true">{networksOpen ? "⌃" : "⌄"}</span>
      </button>
      {networksOpen && <>
        <p className="sx-sub">Os endereços ficam neste dispositivo e não entram no diagnóstico.</p>
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
        <button type="button" className="sx-btn small" disabled={loading} onClick={() => void refresh()}><RefreshCw size={14}/>{loading ? "Atualizando…" : "Atualizar detecção"}</button>
        {error && <div className="sx-error">{error}</div>}
      </>}
    </section>
  </div>;
}
