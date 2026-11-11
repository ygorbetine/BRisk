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

## App nativo (CUI, versão 1.4.0): passo a passo

As seções 2 e 3 acima são do app antigo (Electron). No app nativo o TURN vem **da conta**: depois de entrar, o app pede as
credenciais temporárias à função `turn-credentials` com a sessão do usuário e as renova sozinho. Nada vai no instalador.

1. **Servidor Coturn numa VPS** (IP público, Docker): siga `infrastructure/coturn/README.md`. Gere o segredo no servidor, ponha no
   `.env.production` e suba com `docker compose -f compose.production.yml up -d`. Portas: 3478 UDP/TCP, 5349 TCP, 443 TCP e 49152–65535 UDP.
2. **Segredos no Supabase** (rode no seu terminal; não cole o segredo em conversa, no git ou no app):
   ```bash
   supabase secrets set TURN_HOST=turn.seudominio.com TURN_SECRET=<o mesmo segredo do Coturn>
   ```
   Opcionais: `TURN_PORT` (3478), `TURN_TLS_PORT` (5349), `TURN_TLS_ALT_PORT` (443), `TURN_TTL_SECONDS`.
3. **Publicar a função**: `supabase functions deploy turn-credentials`. Ela exige a sessão de um usuário Briks (responde 401 sem login)
   e limita pedidos por IP e por usuário (usa a função `edge_rate_limit` da migração 0002, que precisa estar aplicada).
4. **Conferir no app**: entre na conta, abra Configurações > Rede. O cartão do servidor TURN deve mostrar **"TURN da sua conta"**.
   Sem a função publicada, sem login ou com erro, mostra **"Somente STUN"** e a chamada continua tentando conexão direta.
5. **Teste real**: dois computadores em redes diferentes (um deles no 4G do celular) entram na mesma sala e se ouvem.

Prioridade no app: o que o usuário preencher em Configurações > Rede (ou `ice.json` / `BRIKS_ICE_SERVERS_JSON`) vale mais que o TURN da conta;
sem nenhum dos dois, só o STUN público. NÃO TESTADO nesta integração contra o projeto Supabase e um Coturn reais: só com servidor falso nos testes do core.
