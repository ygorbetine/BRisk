import { useEffect, useState } from "react";
import { loadNotifySettings, saveNotifySettings, watchNotifySettings, type FriendNotifySettings } from "../services/profile/friend-notifications";

/** Liga/desliga os avisos de amigos. O silêncio por amigo fica no sino da lista de amigos. */
export function FriendNotifyPanel() {
  const [settings, setSettings] = useState<FriendNotifySettings>(loadNotifySettings);
  useEffect(() => watchNotifySettings(setSettings), []);
  const update = (next: FriendNotifySettings) => setSettings(saveNotifySettings(next));
  const blocked = typeof Notification !== "undefined" && Notification.permission === "denied";

  return <section className="sx-stack" aria-label="Notificações de amigos">
    <div className="sx-card">
      <span className="sx-title">Avisos de amigos</span>
      <label className="sx-row">
        <div><strong>Amigo ficou online</strong><small>Avisa quando alguém da sua lista abre o Briks.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.online} onChange={(event) => update({ ...settings, online: event.target.checked })}/><i/></span>
      </label>
      <div className="sx-divider"/>
      <label className="sx-row">
        <div><strong>Amigo começou a transmitir</strong><small>Avisa quando alguém começa a compartilhar a tela numa chamada.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.streaming} onChange={(event) => update({ ...settings, streaming: event.target.checked })}/><i/></span>
      </label>
      <p className="sx-note">Nada é avisado enquanto você está numa chamada nem logo depois de abrir o app. Para silenciar uma pessoa, use o sino ao lado do nome dela em Amigos{settings.muted.length ? ` (${settings.muted.length} silenciado${settings.muted.length > 1 ? "s" : ""})` : ""}.</p>
      {blocked && <p className="sx-note">As notificações do Briks estão bloqueadas nas configurações do sistema.</p>}
    </div>
  </section>;
}
