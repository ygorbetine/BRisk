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

## App nativo (CUI, 1.4.4): passo a passo curto para o dono

As seções acima (`.env`, `VITE_*`, `pnpm package:win`) são do app antigo (Electron) e não valem para o app nativo.
Nenhum segredo vai para o app nem para o git: a chave e o segredo do LiveKit ficam só no Supabase.

### 1. Escolha o servidor de chamadas
- **LiveKit Cloud** (mais fácil): crie conta e projeto em livekit.io; anote `wss://SEU-PROJETO.livekit.cloud`, API Key e API Secret.
  Confira no site o plano grátis atual (minutos de participante, banda e nº de conexões simultâneas) e o preço depois do limite: muda com o tempo.
- **VPS própria** (custo fixo, mais trabalho): `infrastructure/livekit/` (Docker). Gere as chaves (`docker run --rm livekit/generate`),
  ponha no `livekit.yaml`, abra 7880/tcp, 7881/tcp, 3478/udp, 5349/tcp e 50000-50200/udp e coloque TLS na frente para ter `wss://`.
  Confira banda mensal da VPS: vídeo em SFU gasta upload do servidor por espectador.

### 2. Rode o assistente (no SEU terminal, na raiz do repositório)
```powershell
supabase login        # uma vez
powershell -NoProfile -File scripts/ativar-sfu.ps1
```
Ele confere o Supabase CLI e o projeto, pergunta a URL, a API Key e o API Secret (os dois últimos sem aparecer na tela),
grava os segredos no Supabase, publica `livekit-token` e `livekit-kick` e valida tudo (ver passo 4). Não cole chave em conversa nenhuma.
Projeto diferente do padrão (`wusvizrteyoewggageeu`): `-ProjectRef SEU-REF`.

<details><summary>Fazendo à mão (o que o assistente automatiza)</summary>

```bash
supabase secrets set --env-file ./segredos.env      # LIVEKIT_API_KEY, LIVEKIT_API_SECRET, LIVEKIT_URL (apague o arquivo depois)
supabase functions deploy livekit-token
supabase functions deploy livekit-kick               # opcional: a expulsão derruba a pessoa de verdade no servidor
```
</details>

### 3. Entregar o endereço a todos os usuários (via instalador)
O instalador leva um `livekit.json` ao lado do `Briks.exe` com **só endereços públicos** (`url` e `token_url`; não é segredo).
O app usa o `livekit.json` da pasta de dados (`%APPDATA%\Briks-CUI`) se existir; senão o do instalador. Ao gerar o instalador:
```powershell
$env:BRIKS_LIVEKIT_URL = "wss://SEU-SERVIDOR"
$env:BRIKS_LIVEKIT_TOKEN_URL = "https://wusvizrteyoewggageeu.supabase.co/functions/v1/livekit-token"   # opcional: sem ele, deriva da URL do supabase.json
powershell -NoProfile -File scripts/build-installer.ps1 -Version 1.4.4
```
(ou `BRIKS_LIVEKIT_JSON_SRC=caminho\livekit.json`; o script recusa qualquer chave que não seja `url`, `token_url`, `kick_url`).
**Sem essas variáveis o instalador sai como antes, só P2P.** Quem já tem o app instalado e quer testar antes de publicar pode
só criar `%APPDATA%\Briks-CUI\livekit.json` (o assistente já faz isso na sua máquina).

### 4. Conferir que funcionou (sem segredos)
```powershell
powershell -NoProfile -File scripts/verificar-sfu.ps1 -Email conta-de-teste@exemplo.com
```
Diz OK/FALHA por etapa: servidor responde; função de tokens dá 401 sem login; login de teste; função emite token (200);
WebSocket aceita o token. Use uma conta Briks de teste (senha pedida mascarada ou em `BRIKS_TEST_PASSWORD`).
No app: Configurações > Rede > diagnóstico mostra "Modo da chamada: SFU · automático: servidor de chamadas (SFU) · servidor SEU-SERVIDOR".

### O que o app faz com e sem servidor
- **Sem `livekit.json`** (nem variáveis): tudo em P2P, como hoje. "Sempre SFU" mostra o aviso de que o servidor não está configurado.
- **Com servidor**: **Automático e "Sempre SFU" usam o servidor de chamadas**; "Sempre P2P" continua disponível se todos escolherem o mesmo.
  Se o token não sair (sem conta, Supabase ou LiveKit fora do ar), a chamada **não abre** e aparece o motivo; ela não cai em P2P sozinha,
  porque quem caísse ficaria numa sala à parte. Reserva P2P (só se todos combinarem): `"fallback":"p2p"` no `net-mode.json` ou `BRIKS_NET_FALLBACK=p2p`.
- Queda de rede ou do servidor: reconecta sozinho (até 2 minutos) e a lista de pessoas não pisca.
- A lógica de escolha de modo do cliente é de outra frente de trabalho; este guia só cobre servidor, funções e instalador.

Detalhes técnicos, medições e o que falta: `docs/plans/sfu-livekit-nativo.md`.
