# Privacy policy / Política de privacidade

Briks is an open-source (AGPL-3.0) Windows app for screen sharing and voice calls. This page says what the app sends over the network. The source code is public, so every statement here can be checked.

## What the app does and does not collect

- **Account (optional login with Google or Discord).** Sign-in goes through our Supabase project. It stores the data the provider returns for the account: e-mail address, display name and avatar, plus the profile choices you make in Briks (nickname, friends, groups). Passwords of those providers never reach Briks.
- **Calls and screen sharing.** Audio, video and screen content travel over WebRTC, either peer-to-peer or through a LiveKit media server (SFU) that relays them in real time. They are **not recorded or stored** by Briks. A TURN relay (Cloudflare) may relay encrypted traffic when a direct connection is not possible; short-lived relay credentials are requested for that.
- **Coordination.** Supabase Realtime is used only for temporary signaling and presence (who is online in a room). Signaling data is not persisted.
- **Update check.** The app downloads `latest.yml` from the GitHub Releases page of this repository to learn whether a new version exists.
- **Optional anonymous counters.** The setting "Ajudar a melhorar as chamadas" sends only daily counters (whether a connection succeeded and whether it was direct or relayed). Never who, with whom, room, IP address or content. It can be turned off in Settings.
- **Local data.** Settings, keys and the local database stay in `%APPDATA%\Briks-CUI` on your PC.
- **No advertising, no tracking, no sale of data.**

## Your choices

You can use Briks without logging in to the online account features where the app allows it, turn off the anonymous counters, and delete your local data by removing `%APPDATA%\Briks-CUI`. To ask for deletion of your online account data, open a private report as described in [SECURITY.md](SECURITY.md).

## Changes

Changes to this policy are made through commits in this repository.

---

**PT-BR:** O Briks não grava chamadas nem tela. O login (Google/Discord) guarda e-mail, nome e avatar no Supabase do projeto. Chamadas passam por WebRTC (direto ou por servidor LiveKit/relay TURN, sem armazenar). O app consulta o GitHub para ver atualizações e, se você deixar ligado, envia contadores anônimos diários de conexão. Dados locais ficam em `%APPDATA%\Briks-CUI`. Não há anúncios, rastreamento nem venda de dados.
