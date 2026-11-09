# Checklist do servidor — versão 1.2.0

O que precisa ser feito no Supabase / LiveKit / coturn por quem tem acesso aos painéis. Nada disto é automático.
**O app 1.2.0 e as Edge Functions devem sair juntos** (a telemetria e o limite dependem da migration 0002).

## 1. Banco (SQL Editor)
1. Abra **SQL Editor › New query**, cole `supabase/migrations/0002_rate_limit_and_call_telemetry.sql` e rode.
   Pode rodar de novo sem problema.
2. Confira:
   ```sql
   select tablename, rowsecurity from pg_tables where schemaname = 'public';
   -- edge_rate_hits e call_outcomes_daily devem aparecer com rowsecurity = true
   ```
3. Em **Advisors › Security Advisor**, confirme que não há tabela sem RLS nem função exposta indevidamente.

## 2. Edge Functions
Publique **sem** `--no-verify-jwt`:
```
supabase functions deploy turn-credentials
supabase functions deploy livekit-token
```
- `SUPABASE_SERVICE_ROLE_KEY` já é injetada pelo Supabase; não precisa criar.
- Se o app web for hospedado num domínio, defina `ALLOWED_ORIGINS` (ex.: `https://app.exemplo.com`).
- Opcional: `TURN_TTL_SECONDS` e `LIVEKIT_TOKEN_TTL_SECONDS` (padrão e máximo: 3600).

Teste rápido (deve dar **401** sem login):
```
curl -i https://<projeto>.supabase.co/functions/v1/turn-credentials
```

## 3. Ler a telemetria
```sql
select day, transport, path, outcome, total from public.call_outcomes_daily order by day desc, transport, path, outcome;
-- taxa de falha por dia
select day, sum(total) filter (where outcome = 'failed')::float / nullif(sum(total), 0) as falha
from public.call_outcomes_daily group by day order by day desc;
```

## 4. Painel do Supabase (S4c e regras permanentes)
- Auth › Providers › Email: **confirmação de e-mail ligada**.
- Auth › Rate Limits: limite de login/cadastro por IP (ex.: 30 por 5 min).
- Auth › Password: tamanho mínimo 10 e exigir letras e números.
- Auth › MFA: TOTP ligado (para a 2FA quando as telas saírem).

## 5. Item 19 — operação
- **Backup:** confirmar o plano de backup diário do Supabase e **testar uma restauração** num projeto de teste (pelo menos uma vez).
- **Monitoramento:** alertas de uso (banda, Edge Function errors, conexões Realtime) no Supabase; no LiveKit, painel de salas/erros; no coturn, uso de banda e CPU da VPS.
- **Região:** TURN e SFU na mesma região da maioria dos usuários (Brasil → São Paulo).
- **LiveKit:** autoescala ou limite de salas conforme o plano contratado.
- **Segredos:** se houver suspeita de vazamento, gire `TURN_SECRET` (coturn + `supabase secrets set`) e `LIVEKIT_API_SECRET`, e republique as funções.
- `SECURITY.md`: preencher o contato de segurança.
