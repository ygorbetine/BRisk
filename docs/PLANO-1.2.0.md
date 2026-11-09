# Plano da versão 1.2.0

Escopo pedido: tudo numa única versão. Nada é publicado sem autorização explícita.

Achado do diagnóstico (leitura do repositório): o Supabase guarda só contas (`profiles`, `profile_keys`, funções `claim_nick`/`register_key`/`revoke_key`, RLS ligada nas duas tabelas) e faz a sinalização (Realtime). Há duas Edge Functions (`livekit-token`, `turn-credentials`). O chat é local/P2P. Só existe uma migration. Por isso o status e a presença podem ir pelo canal Realtime já usado, sem tabela nova.

Legenda de quem executa: **Eu** = código neste repositório. **Você** = ação em painel/conta que eu não faço (Supabase, LiveKit, VPS).

## Fase 1 — Perfil e presença
1. Status personalizado (emoji + texto até 60, limpar depois de 1h/4h/hoje/nunca), enviado na presença Realtime; exibido na lista de amigos, na barra de conta e no bloco da chamada. Botão de emoji abre o teclado do sistema (`app.showEmojiPanel`). — Eu
2. Rich presence: detecta o programa ou jogo em primeiro plano (inclui programas de trabalho, não só jogos) e mostra o nome com o ícone do app. Fica numa linha própria, separada do status personalizado: os dois aparecem juntos e um nunca substitui o outro (status personalizado = texto que a pessoa escreve; rich presence = o que o Briks detecta). Opt-in (desligado por padrão), com lista de programas ignorados e botão para ocultar. O ícone vai como PNG pequeno (24 px, limite de poucos KB, validado como data:image/png) na presença Realtime; sem ícone válido, mostra só o nome. — Eu (backend Rust + Electron `app:icon`)
3. Notificações de amigos: aviso quando um amigo fica online e quando começa a transmitir (bandeja/notificação do sistema). Uma opção para cada tipo, com silenciar por amigo e sem avisar durante a chamada ou ao abrir o app (evita enxurrada de avisos de quem já estava online). — Eu

## Fase 2 — Chamada e transmissão
4. Indicador de nível de áudio por app no seletor. — Eu (backend Rust)
5. Atalhos globais (mutar, ensurdecer, transmitir) e push-to-talk. — Eu (Electron)
6. Pedir transmissão a um amigo. — Eu
7. Qualidade adaptativa conforme a rede do espectador. — Eu
8. Teste de rede antes de entrar (ping, perda, se precisa de relay). — Eu
9. Gerenciador de dispositivos de áudio que lembra a escolha por jogo. — Eu
10. Histórico de chamadas (local). — Eu

## Fase 3 — Social
11. Salas com link de convite temporário. — Eu (pode exigir função no Supabase)
12. Mensagem de voz curta no chat. — Eu

## Fase 4 — Maior risco, por último
13. Gravar a transmissão localmente. — Eu
14. Controle remoto com permissão (injeção de entrada no SO). Só com confirmação a cada sessão, só mouse/teclado dentro da janela compartilhada, revogável. — Eu, com revisão de segurança antes de entrar

## Infraestrutura
15. Limite de requisições e validação nas Edge Functions `livekit-token` e `turn-credentials`; TURN com credencial de vida curta (confirmar). — Eu
16. Revisão de RLS e criação de migration versionada para qualquer mudança. — Eu escrevo; **Você** aplica no SQL Editor
17. Telemetria de falha de chamada sem dados pessoais (contadores P2P vs relay). — Eu
18. Remover ou arquivar `server/` (Rust legado). — Eu, só com sua confirmação
19. Backup e teste de restauração do Postgres, alertas e monitoramento do Supabase/LiveKit/coturn, região do TURN/SFU, autoescala do LiveKit. — **Você**; eu preparo um checklist

## Encerramento
- `tsc`, testes, captura de tela real das telas novas, notas de versão.
- Build e publicação só com sua autorização.

## Divisão em duas versões (decidido em 2026-11-09)
- **1.2.0:** itens 1 a 3 (status, rich presence, notificações), 5 (atalhos e push-to-talk), 6 (pedir transmissão), 8 (teste de rede), 10 (histórico de chamadas), 15 a 17 (infra no repositório).
- **1.3.0:** itens 4, 7, 9, 11, 12, 13, 14, 18 (nível de áudio por app, qualidade adaptativa, dispositivos por jogo, links temporários, mensagem de voz, gravação, controle remoto, remover `server/`).
- Item 19 (backup, monitoramento, região): ações do dono, em paralelo.

## Estado em 2026-11-09 (sem commit)
- Feito: status personalizado completo (editor, presença, lista, barra de conta, bloco da chamada), `foreground.rs` (rota `/presence/foreground`, teste ok), `rich-presence.ts` + teste, `voice-activity.ts` com `setLocalApp`/`onAppChange`.
- Rich presence concluído: `use-rich-presence.ts` (sondagem 10 s, estável em 2 leituras, ícone 24 px), `RichPresencePanel` em Perfil e conta (opt-in, ocultar o atual, lista de ignorados), linha "Usando X" na lista de amigos sob o status.
- Notificações concluídas: `friend-notifications.ts` + teste, `use-friend-notifications.ts`, campo `streaming` na presença (só com sala de voz), aba Configurações › Notificações, sino por amigo na lista. Sem aviso em chamada nem nos primeiros 20 s.
- `tsc` (web e desktop) e vitest do web passando. Falta: captura de tela real das telas novas (precisa do app desktop rodando), teste do Rust (`cargo test foreground`).
- Item 5 concluído: `apps/desktop/src/call-shortcuts.ts` (globalShortcut só durante a chamada, IPC `shortcuts:set`/`shortcuts:action`, incluído em `build.files`), `desktop-backend/src/p2p/input.rs` (rota `/input/key-wait`, long-poll com `GetAsyncKeyState` só da tecla escolhida, sem interceptar), `services/input/shortcuts.ts` + teste, `use-call-shortcuts.ts`, aba Configurações › Atalhos. Padrão: Ctrl+Shift+M (mutar), Ctrl+Shift+D (ensurdecer), transmitir sem atalho, PTT desligado.
- Falta testar no app real: atalhos com o Briks em segundo plano e PTT dentro de um jogo.
- Item 6 concluído: `call/stream-request.ts` + teste (mensagem `call.stream.request`, limite de 1 pedido a cada 30 s por pessoa nos dois lados), `CallController.requestStream`/`onStreamRequest` (só aceita de participante autenticado, ignora se já está transmitindo; funciona em P2P e SFU), botão "Pedir transmissão" no bloco de quem não transmite e convite "Transmitir / Agora não" para quem recebe (some em 30 s; notificação do sistema se o app estiver em segundo plano). Escopo: dentro da chamada; pedir a um amigo fora da chamada ficou de fora.
- Próximo: item 8 (teste de rede antes de entrar).

## Segurança e prevenção de vazamento (auditoria de 2026-11-09)

Achados da leitura do repositório (não inclui o painel do Supabase, que precisa ser conferido por quem tem acesso):

| # | Achado | Gravidade |
|---|---|---|
| S1 | `turn-credentials` publicada com `--no-verify-jwt`: qualquer pessoa na internet pede credenciais do relay e usa a banda do projeto. Sem limite de requisições. | Alta |
| S2 | `livekit-token` emite token de 6 h para qualquer sala e identidade informadas, sem checar quem pede nem se pertence à sala. A chave `anon` é pública (vai no app). Sem limite de requisições, CORS aberto. | Alta |
| S3 | Sem criptografia ponta a ponta: no SFU o servidor termina a cifra da mídia e vê o conteúdo; E2EE da LiveKit não está ligada. Mensagens ficam no banco local sem criptografia. | Média |
| S4 | Sem captcha, sem 2FA e sem limite de tentativas no código de conta (cadastro por e-mail e senha no Supabase). | Média |
| S5 | Declaração de idade é só uma data digitada, sem verificação. | Média |

Já está bom: RLS ligada em `profiles` e `profile_keys` com escrita só por funções; identidade ECDSA P-256 não extraível e mensagens assinadas; Electron com `contextIsolation`, `sandbox` e sem `nodeIntegration`; backend local com token aleatório; `.env` fora do git e sem segredos no histórico do `.env` (verificado em 2026-11-09 com busca por padrões comuns de chaves).

### Itens de ação (entram na 1.2.0, antes de qualquer divulgação em escala)
- **S1** Exigir sessão de usuário logado em `turn-credentials` (remover `--no-verify-jwt`), limitar requisições por usuário e por IP, TTL curto (1 h ou menos). — ✅ **Feito no código (2026-11-09), pendente de deploy.** Limitador em memória por instância; **Você** faz o deploy (ver SECURITY.md / resumo).
- **S2** Em `livekit-token`: validar o JWT do usuário, derivar a `identity` do usuário autenticado (nunca aceitar do corpo), conferir que ele pertence à sala, reduzir o TTL, restringir o CORS à origem do app, limitar requisições e validar o formato do nome da sala. — ✅ **Feito no código (2026-11-09), pendente de deploy.** Limitação: a pertença à sala não é verificável no servidor (sala vem de segredo do grupo só nos dispositivos); a checagem é sessão + `peerId` registrado em `profile_keys` + sala validada + TTL 1 h. Fechar de vez exigiria tabela de membros por sala. Cliente e funções precisam sair juntos.
- **S4a** Captcha no cadastro e no login com **Cloudflare Turnstile** (integração nativa do Supabase Auth). — Eu: widget no app; **Você**: criar o site no Turnstile e colocar a chave no painel do Supabase.
- **S4b** 2FA por aplicativo autenticador (TOTP, MFA do Supabase): tela de ativar, QR code, códigos de recuperação e pedido do código no login. Opcional para todos, **obrigatório para quem administra grupos**. — Eu: telas; **Você**: ligar MFA no painel.
- **S4c** Limites de tentativa de login e política de senha mínima no painel do Supabase. — **Você**; eu preparo o checklist.
- **S3a** Ligar a E2EE da LiveKit nas chamadas, com chave derivada do grupo (a chave nunca vai ao servidor). — Eu (pode virar 1.3.0 se o SFU próprio ainda não existir).
- **S3b** Cifrar o banco local de mensagens (chave guardada no cofre do Windows via Electron `safeStorage`). — Eu.
- **S5** Verificação de idade real (gov.br) conforme já anotado nas pendências. — Eu + decisão do dono sobre o provedor.

### Regras permanentes para evitar vazamento
1. Nenhum segredo no repositório nem no app: só `anon` (pública) e URLs. `service_role`, `LIVEKIT_API_SECRET`, `TURN_SECRET` ficam só nos segredos das funções. Rodar busca automática de segredos (por exemplo `gitleaks`) no CI e antes de cada release.
2. Todo endpoint novo nasce **com autenticação, validação de entrada e limite de requisições**. Nenhum `--no-verify-jwt` sem motivo escrito neste plano.
3. Princípio do menor privilégio: tabela nova só com RLS ligada e política mínima; sem leitura pública de dados pessoais além do `nick`.
4. Não guardar o que não precisa: o servidor já não persiste mensagens, SDP, ICE nem histórico de chamadas. Manter assim. Qualquer log precisa de prazo de retenção e sem conteúdo.
5. Conferir o painel do Supabase periodicamente: confirmação de e-mail ligada, MFA ligado, limites de login, sem tabela exposta sem RLS (o próprio Supabase tem um verificador de segurança).
6. Plano de resposta a incidente: quem avisa quem, como revogar chaves (`revoke_key` já existe), como girar `LIVEKIT_API_SECRET` e `TURN_SECRET`, e aviso aos usuários e à ANPD no prazo da LGPD (conferir prazo vigente).
7. Revisão de segurança antes de funções de risco (controle remoto, gravação) e auditoria externa quando houver escala.
8. Relatar vulnerabilidades: arquivo `SECURITY.md` no repositório com canal de contato.

Observação: o post do Reddit citado pelo dono (r/programacao, "vazamento de dados") não pôde ser lido nesta sessão (acesso bloqueado). Quando o resumo for colado, conferir se alguma causa daquele caso ainda não está coberta acima.
