import { useState } from "react";
import { LocalStoragePanel } from "../components/LocalStoragePanel";
import { NetworkSettingsPanel } from "../components/NetworkSettingsPanel";
import { OverlaySettingsPanel } from "../components/OverlaySettingsPanel";
import { AboutPanel } from "../components/AboutPanel";
import { UpdatesPanel } from "../components/UpdatesPanel";
import { useUpdateState } from "../components/useUpdateState";
import { ThemePanel } from "../components/ThemePanel";
import { VoiceVideoSettingsPanel } from "../components/VoiceVideoSettingsPanel";

export type SettingsTab = "voice" | "network" | "theme" | "storage" | "notices" | "overlay" | "updates" | "about";

export type SettingsNotice = { id: number; text: string; at: number };

export function SettingsView({ initialTab = "voice", notices = [], onClearNotices }: { initialTab?: SettingsTab; notices?: SettingsNotice[]; onClearNotices?: () => void } = {}) {
  const [tab, setTab] = useState<SettingsTab>(initialTab);
  const update = useUpdateState();
  return <>
    <nav className="settings-tabs" aria-label="Seções das configurações">
      <button className={tab === "voice" ? "active" : ""} onClick={() => setTab("voice")}>Voz e vídeo</button>
      <button className={tab === "network" ? "active" : ""} onClick={() => setTab("network")}>Rede</button>
      <button className={tab === "theme" ? "active" : ""} onClick={() => setTab("theme")}>Temas</button>
      <button className={tab === "storage" ? "active" : ""} onClick={() => setTab("storage")}>Armazenamento</button>
      <button className={tab === "overlay" ? "active" : ""} onClick={() => setTab("overlay")}>Sobreposição</button>
      <button className={tab === "notices" ? "active" : ""} onClick={() => setTab("notices")}>Avisos{notices.length > 0 ? ` (${notices.length})` : ""}</button>
      <button className={tab === "updates" ? "active" : ""} onClick={() => setTab("updates")}>Atualizações{update.phase === "ready" ? " •" : ""}</button>
      <button className={tab === "about" ? "active" : ""} onClick={() => setTab("about")}>Sobre</button>
    </nav>
    {tab === "voice" && <VoiceVideoSettingsPanel/>}
    {tab === "network" && <NetworkSettingsPanel/>}
    {tab === "theme" && <ThemePanel/>}
    {tab === "storage" && <LocalStoragePanel/>}
    {tab === "overlay" && <OverlaySettingsPanel/>}
    {tab === "notices" && <section className="sx-stack" aria-label="Avisos">
      {notices.length === 0 ? <div className="sx-card"><span className="sx-title">Nenhum aviso</span><p className="sx-sub">Erros e avisos do Briks aparecem aqui. Tudo certo por enquanto.</p></div>
        : <>{notices.map((item) => <div className="sx-card" key={item.id}><span className="sx-title">{item.text}</span><small className="sx-sub">{new Date(item.at).toLocaleTimeString()}</small></div>)}
          <button type="button" className="sx-btn" onClick={onClearNotices}>Limpar avisos</button></>}
    </section>}
    {tab === "updates" && <UpdatesPanel/>}
    {tab === "about" && <AboutPanel/>}
  </>;
}
