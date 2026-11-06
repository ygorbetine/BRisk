# SFU do Briks (LiveKit)

O SFU recebe cada transmissão uma vez e a distribui, então o upload de quem transmite
não cresce com o número de espectadores e várias pessoas podem transmitir ao mesmo tempo.

1. Em um servidor com IP público: `docker compose -f compose.yml up -d` (ajuste `livekit.yaml`;
   abra 7880/tcp, 7881/tcp, 3478/udp, 5349/tcp e 50000-50200/udp; use TLS/proxy para `wss://`).
2. Serviço de tokens: `supabase functions deploy livekit-token` e
   `supabase secrets set LIVEKIT_API_KEY=... LIVEKIT_API_SECRET=... LIVEKIT_URL=wss://...`.
3. No `.env` do app: `VITE_LIVEKIT_URL` e `VITE_LIVEKIT_TOKEN_URL`.

Sem essas variáveis o Briks usa só a malha P2P. Com elas, chamadas com mais de 4 pessoas
(ou rede degradada) passam a usar o SFU automaticamente.
