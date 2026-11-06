# Ativar o TURN (para quem não consegue entrar na chamada)

O TURN repassa a chamada quando dois amigos não conseguem se conectar direto (operadoras, redes com bloqueio).
Sem ele, parte dos amigos fica sem áudio/vídeo. Você precisa de **um servidor Linux com IP público** (VPS barata serve).

## 1. Servidor (uma vez)
1. VPS Linux com Docker e IP fixo; aponte um domínio (ex.: `turn.seudominio.com`) para o IP.
2. Gere certificado TLS para esse domínio e siga `infrastructure/coturn/README.md` (secret, `.env.production`, `docker compose up -d`).
3. Libere as portas: 3478 UDP/TCP, 5349 TCP, 443 TCP, 49152–65535 UDP.
4. Valide a configuração do repositório: `pnpm verify:turn-config`.

## 2. Emissor de credenciais (usa o Supabase que você já tem)
O app pede credenciais temporárias a uma URL pública. A função pronta está em `supabase/functions/turn-credentials`.
1. No painel do Supabase: Edge Functions → criar `turn-credentials` com o conteúdo do arquivo; **desligue "Verify JWT"**.
2. Em Secrets: `TURN_HOST=turn.seudominio.com` e `TURN_SECRET=<o mesmo segredo do Coturn>`.
3. Teste no navegador: `https://<projeto>.supabase.co/functions/v1/turn-credentials` deve devolver JSON com `iceServers`.

## 3. Ligar no app
No `.env` do build: `VITE_TURN_CREDENTIALS_URL=https://<projeto>.supabase.co/functions/v1/turn-credentials` e gerar o instalador (`pnpm package:win`).

## 4. Conferir
Em chamada, o diagnóstico do Briks deve mostrar caminho `relay` quando dois amigos não conseguem ligação direta.

## Cuidados
- Sem limite de requisições na função: qualquer um que descubra a URL gera credenciais e gasta a banda do seu servidor. Use limite de banda no Coturn (`TURN_USER_QUOTA`) e monitore.
- Nunca coloque o `TURN_SECRET` no app, no git ou em chat.
