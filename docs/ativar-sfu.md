# Ativar o SFU (guia passo a passo)

O Briks funciona sem isto (malha P2P, até 8 pessoas). O SFU melhora a qualidade com muita gente
ou várias transmissões ao mesmo tempo. Você precisa de **um servidor LiveKit** e do **serviço de tokens**.

## Opção A — LiveKit Cloud (mais fácil, tem plano grátis com limites; confira os atuais)
1. Crie uma conta em livekit.io e um projeto.
2. Copie: URL (`wss://SEU-PROJETO.livekit.cloud`), API Key e API Secret.
3. Instale o Supabase CLI e faça login (`supabase login`), depois no projeto:
   ```bash
   supabase secrets set LIVEKIT_API_KEY=SUA_KEY LIVEKIT_API_SECRET=SEU_SECRET LIVEKIT_URL=wss://SEU-PROJETO.livekit.cloud
   supabase functions deploy livekit-token
   ```
4. No `.env` da raiz:
   ```
   VITE_LIVEKIT_URL=wss://SEU-PROJETO.livekit.cloud
   VITE_LIVEKIT_TOKEN_URL=https://SEU-PROJETO.supabase.co/functions/v1/livekit-token
   ```
5. Gere o instalador de novo (`pnpm package:win`).

## Opção B — servidor próprio (VPS)
Siga `infrastructure/livekit/README.md` (Docker + `livekit.yaml`) e depois os passos 3 a 5 acima.

## Como saber se funcionou
Em uma sala com mais de 8 pessoas (ou escolhendo SFU), o diagnóstico da conexão mostra "SFU".
Sem as variáveis, o app continua só com a malha P2P.

## App nativo (CUI, versão 1.4.0): passo a passo curto

As seções acima (`.env`, `VITE_*`, `pnpm package:win`) são do app antigo (Electron) e não valem para o app nativo.

Nada aqui foi publicado ainda: é o que VOCÊ faz quando quiser ligar o servidor de chamadas. Nenhum segredo vai para o app.

1. **Servidor LiveKit** (escolha um):
   - **LiveKit Cloud**: crie a conta e um projeto em livekit.io. Anote a URL (`wss://SEU-PROJETO.livekit.cloud`), a API Key e o API Secret.
   - **VPS própria**: `infrastructure/livekit/` (Docker). Gere as chaves (`docker run --rm livekit/generate`), ponha no `livekit.yaml`,
     abra 7880/tcp, 7881/tcp, 3478/udp, 5349/tcp e 50000-50200/udp e coloque TLS na frente para ter `wss://`.
2. **Segredos no Supabase** (ficam só no servidor; rode no seu terminal, não cole em conversa nenhuma):
   ```bash
   supabase secrets set LIVEKIT_API_KEY=... LIVEKIT_API_SECRET=... LIVEKIT_URL=wss://SEU-SERVIDOR
   ```
3. **Publicar a função de tokens** (com o JWT do gateway ligado): `supabase functions deploy livekit-token`.
   Opcional, para a expulsão derrubar a pessoa de verdade no servidor: `supabase functions deploy livekit-kick`
   (o app nativo já a chama quando o dono da sala expulsa alguém numa chamada pelo SFU; se a função não estiver publicada, fica só o aviso assinado entre os apps).
4. **Dizer ao app onde fica o servidor** (são endereços públicos, não segredos). Crie `%APPDATA%\Briks-CUI\livekit.json`:
   ```json
   {"url":"wss://SEU-SERVIDOR","token_url":"https://SEU-PROJETO.supabase.co/functions/v1/livekit-token"}
   ```
   (ou as variáveis `BRIKS_LIVEKIT_URL` / `BRIKS_LIVEKIT_TOKEN_URL`). Só `wss://` e `https://` valem.
   Para todos os usuários receberem isso no instalador ainda falta embutir o arquivo: NÃO FEITO.
5. **Entrar na conta Briks** no app e abrir uma chamada. Em Configurações > Rede > diagnóstico deve aparecer
   "Modo da chamada: SFU · automático: servidor de chamadas (SFU) · servidor SEU-SERVIDOR".

O que muda para quem usa:
- Com o arquivo acima, **Automático e "Sempre SFU" usam o servidor de chamadas**. Se o token não sair (sem conta, Supabase ou LiveKit
  fora do ar), a chamada **não abre** e aparece o motivo; ela não cai em P2P sozinha, porque quem caísse ficaria falando com ninguém
  numa sala à parte. "Sempre P2P" continua disponível, desde que todos escolham o mesmo.
- Reserva P2P automática (só se todos combinarem): `"fallback":"p2p"` no `net-mode.json` da pasta de dados ou `BRIKS_NET_FALLBACK=p2p`.
- Queda de rede ou do servidor: o app reconecta sozinho (até 2 minutos tentando) e a lista de pessoas não pisca.
- Sem o arquivo, nada muda: tudo continua em P2P.

Detalhes técnicos, medições e o que falta: `docs/plans/sfu-livekit-nativo.md`.
