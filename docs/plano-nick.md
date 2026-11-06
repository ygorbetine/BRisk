# Plano: identidade ligada ao @nick

> Status: **0.5.0 entregou contas, @nick e registro de chaves** (ver `ativar-nick.md`). Grupos por @nick vêm a seguir.

Objetivo: trocar a "id invisível" (chave gerada por instalação) por um **@nick único** por pessoa,
que sobrevive a reinstalação e troca de computador. Acaba com as cópias do mesmo nome, com a perda
de dono ao reinstalar e com amigos que "entram mas não se veem".

## Por que isso é necessário
- Hoje a identidade é uma chave ECDSA P-256 guardada no IndexedDB do app, **não exportável de propósito**.
- Cada origem do app (modo desenvolvimento, app instalado) tem seu próprio IndexedDB, então cada uma cria
  uma identidade nova. Os grupos ficam no SQLite do backend (compartilhado), por isso as cópias aparecem.
- Exportar/importar a chave não é possível sem enfraquecer a segurança. A solução é **religar a chave nova
  ao mesmo nick**, não mover a chave.

## Modelo
- Conta no Supabase Auth (e-mail + senha, ou provedor social).
- Tabela `profiles`: `user_id` (PK, ref. auth.users), `nick` (único, minúsculo, 3–20 caracteres `[a-z0-9_]`),
  `created_at`.
- Tabela `profile_keys`: `user_id`, `peer_id`, `public_key` (JWK), `created_at`, `revoked_at`.
  Uma pessoa pode ter várias chaves ativas (um por computador/instalação).
- Regras RLS: leitura pública de `nick` + chaves ativas; escrita só pelo próprio `user_id`.
- O nick é reservado por ordem de chegada; há troca de nick no máximo 1x a cada 30 dias.

## Fluxos
1. **Primeiro acesso:** criar conta, escolher @nick, o app registra a chave pública atual em `profile_keys`.
2. **Novo computador / reinstalação:** entrar com a mesma conta. O app gera chave nova e a registra para o
   mesmo nick. Nada de identidade "nova": o nick continua o mesmo.
3. **Grupos:** o grupo guarda `nick` junto de cada membro. Dono e administradores são definidos por **nick**,
   não por chave. Ao autenticar um peer na chamada, o app confere que a chave apresentada está ativa para
   aquele nick no Supabase.
4. **Convite:** "adicionar @nick" (busca por nick), além do código atual.
5. **Perda de dispositivo:** na conta, revogar a chave (`revoked_at`); a chave revogada deixa de ser aceita.

## Segurança (decisões a fechar antes de construir)
- O Supabase vira ponto de confiança do mapeamento nick → chave. Proteger a conta (senha forte, e-mail
  verificado, idealmente 2FA).
- Cache local do mapeamento para continuar funcionando offline por um tempo (ex.: 24 h).
- Alerta quando um nick ganha chave nova (para o grupo perceber troca suspeita).
- Anti-impersonação: nicks reservados/ofensivos bloqueados; nick comparado em minúsculas.

## Migração dos grupos atuais
- Cada membro legado (sem nick) aparece como "Sem @nick" até a pessoa entrar com a conta.
- Ao entrar, o app propõe: "esta identidade é @fulano" e o dono do grupo confirma a ligação uma vez.
- "Limpar duplicados" passa a agrupar por nick de verdade e some quando não houver mais cópias.

## Entregas sugeridas (versões)
1. **0.4.0 — Contas e nick:** telas de criar conta/entrar, escolha de @nick, tabelas e RLS, registro de chave.
2. **0.4.1 — Grupos por nick:** membros, dono e administradores por nick; lista de membros mostra @nick;
   autenticação de peer confere o nick.
3. **0.4.2 — Convite por @nick e migração:** busca por nick, ligação de membros legados, revogação de chave.
4. **0.4.3 — Limpeza:** remover o agrupamento por nome, documentar e testar de ponta a ponta.

## Testes
- Unidade: normalização do nick, regras de troca, escolha de chave ativa.
- Integração: criar conta, reinstalar (perfil limpo), confirmar que o nick continua dono do grupo.
- Duas máquinas: ver-se na chamada logo após reinstalar.

## Riscos e pontos em aberto
- Necessidade de login: o app hoje funciona sem conta; decidir se haverá modo "convidado".
- Recuperação de conta (e-mail) e política de privacidade.
- Custos do Supabase conforme o número de usuários (conferir os limites do plano atual).
