# Política de segurança

## Como relatar uma vulnerabilidade

Não abra uma issue pública para falhas de segurança. Envie um relato privado para:

**CONTATO-DE-SEGURANCA: <PREENCHER — e-mail ou link de "Report a vulnerability" do GitHub>**

Inclua: o que acontece, passos para reproduzir, versão do app e impacto estimado. Responderemos assim que possível; pedimos que você aguarde a correção antes de divulgar publicamente.

## Escopo

App desktop/web do Briks, Edge Functions em `supabase/functions/` e a configuração do servidor (Supabase, LiveKit, coturn).

## Notas para quem mantém o servidor

- As Edge Functions `turn-credentials` e `livekit-token` exigem usuário logado. Publique **sem** `--no-verify-jwt`.
- Segredos (`TURN_SECRET`, `LIVEKIT_API_SECRET`) ficam só em `supabase secrets`; nunca no repositório nem no app. A chave `anon` do Supabase é pública por design.
- Defina `ALLOWED_ORIGINS` se o app web for hospedado num domínio.
