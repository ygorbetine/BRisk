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
