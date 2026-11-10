# Contas online no app nativo (CUI)

Estado: **fase 1 implementada**, testada sem rede e conferida contra o projeto Supabase de desenvolvimento até o ponto
de abrir o navegador. **Falta**: o login em si (é você quem faz), os redirects do loopback e aplicar as migrações (seção 4).

## 1. O que muda para quem usa

- A tela de entrada mostra primeiro **Continuar com Google** e **Continuar com Discord**. Abaixo, discreto,
  **Entrar com e-mail** e **Criar conta com e-mail** (e-mail + senha, com confirmação do e-mail e recuperação de senha).
- O login social abre o navegador padrão. Ao terminar, a aba mostra "Tudo certo!" e o app continua sozinho.
- A sessão fica guardada: ao reabrir o app a pessoa já entra. **Sair** fica em Configurações > Conta.
- Cada conta ganha um **@nick** (provisório no primeiro acesso, trocável no Perfil). Amigos são adicionados pelo @nick.
- Em Configurações aparece **Importar amigos do Briks antigo** quando há dados do Briks 1.x neste computador.

## 2. Arquitetura

```
UI (C)  src/ui_auth.c, src/account.c
   │  IPC local (linhas JSON, token por processo): account.*, friends.*, import.*, local.credentials
briks-core (Rust)  src/accounts/
   ├─ provider.rs   contrato AuthProvider + tipos (o resto do app só conhece isto)
   ├─ supabase.rs   ÚNICO arquivo que conhece as rotas do Supabase (Auth + RPC) e traduz os erros para português
   ├─ mod.rs        Accounts: sessão, renovação, fluxos (OAuth, e-mail, recuperação), perfil e amigos
   ├─ pkce.rs       PKCE S256 (RFC 7636)
   ├─ loopback.rs   servidor temporário em 127.0.0.1 que recebe o retorno do navegador
   ├─ store.rs      arquivo cifrado com a DPAPI do Windows
   ├─ config.rs     onde achar o servidor; recusa chaves secretas
   ├─ service.rs    comandos/eventos do IPC
   ├─ legacy.rs     importação seletiva do Briks antigo
   └─ fake.rs       servidor de contas falso (testes e modo de desenvolvimento)
```

Trocar de provedor um dia = escrever outra implementação de `AuthProvider`. IPC, UI e testes de fluxo não mudam.

### Login social (PKCE + loopback)

1. O core abre uma porta em `127.0.0.1` (a primeira livre entre **53682, 53683, 53684**) e gera o par PKCE.
2. Abre no navegador `…/auth/v1/authorize?provider=google&redirect_to=http://127.0.0.1:53682/callback&code_challenge=…`.
3. O Supabase devolve `?code=…` para o loopback; o core troca o código pela sessão enviando o **verificador**, que nunca
   saiu do processo. Um código interceptado não serve sem ele (testado), e cada código vale uma vez.
4. Prazo de 5 minutos; **Cancelar** fecha a porta na hora.

Por que loopback e não `briks://`: não exige registrar protocolo no Windows (funciona sem instalador e sem admin) e
outro programa não consegue sequestrar o retorno registrando o mesmo esquema. As portas são fixas para caberem na lista
de redirects permitidos.

### O que fica guardado, e onde

| Dado | Onde | Proteção |
|---|---|---|
| refresh token + nome/e-mail | `%APPDATA%\Briks-CUI\account-session.bin` | DPAPI (só abre na mesma conta do Windows, neste PC) |
| access token | memória do briks-core | renovado sozinho 60 s antes de vencer; nunca vai para a UI nem para o disco |
| senha da conta | em lugar nenhum | vai direto ao Supabase por HTTPS e é apagada da memória da UI |
| credencial do banco local | `local-bridge.bin` | DPAPI; aleatória, não é a senha da conta (ver "Ponte local") |
| chave privada da identidade P2P | `core-identity.json` (só neste PC) | **nunca vai ao servidor**; ver pendência na seção 7 |

Se a DPAPI falhar, nada é gravado em texto aberto: a sessão vale só enquanto o app estiver aberto.
**Sair** invalida a sessão no servidor e apaga `account-session.bin`.

### Conta ↔ identidade P2P

Ao entrar, o core chama `account_sync_profile` com a **chave pública** (JWK com `kty/crv/x/y`) e a impressão digital.
O banco recusa qualquer chave com a parte privada (`d`) e confere a impressão digital (SHA-256 de `x.y`).
Amigos aceitos recebem a chave pública e a impressão digital um do outro em `friends_list`; é o que permite reconhecer
o amigo dentro de uma sala P2P. Pedidos pendentes mostram só nome, avatar e @nick.

Limite desta fase: a identidade é por aparelho. Entrar em outro PC publica a chave daquele PC (a última vence).
A fase 2 (seção 6) resolve isso.

### Foto e @nick para a UI

O módulo entrega à UI dois campos de `app_state_t`: `user_photo` (textura; 0 = só iniciais) e `user_nick` (sem o @).

- **Foto**: no login social o core baixa a foto da conta (só https, sem seguir redirects, até 2 MB, PNG/JPEG conferidos
  pelo conteúdo) para `%APPDATA%\Briks-CUIvatar-cache\<id da conta>.png|jpg` e avisa a UI (`account.avatar`). Nos
  logins seguintes a foto em cache aparece na hora. **Sair** apaga o arquivo.
- **@nick**: o definitivo vem do servidor (`account.profile`). Antes disso (ou enquanto as migrações não existem) a UI
  mostra um provisório derivado do **nome** da conta. Nada é derivado do e-mail: sem nome no provedor, o nome é "Usuário".
- Os botões de entrada usam as logos do Google e do Discord (`apps/desktop-cui/assets/login`, copiadas para
  `build/assets/login`). São marcas dos respectivos donos, usadas só nos botões de login.

### Ponte local (transição)

Mensagens e grupos ainda são gravados pelo backend local (`briks-desktop-backend`), que exige um usuário local. Ao
entrar, a UI pede ao core uma credencial local da conta (`<id da conta>@conta.briks.local` + senha aleatória de 48
caracteres, cifrada com DPAPI) e entra/cadastra esse usuário no banco local sem a pessoa ver. O canal das conversas
diretas passa a usar os ids das contas (`dm-<conta A>-<conta B>`), igual nos dois lados.

### Importar amigos do Briks antigo

`legacy.rs` lê `%APPDATA%\@briks\desktop\risk.sqlite3` **por cópia** (a pasta do Electron nunca é escrita) e traz
contatos, grupos e mensagens P2P para o usuário local da conta. Não importa: a identidade/peerId antigos, as contas
locais antigas (nem os hashes de senha), anexos e histórico de chamadas. Não sobrescreve nada e roda uma vez
(`import-social.json`). Os contatos importados aparecem como "Do Briks antigo": não têm conta; servem de lembrete para
pedir o @nick. O `import.rs` do branch `cui-instalador` (cópia integral, usada pelo instalador) veio junto, sem mudanças.

## 3. Servidor: `supabase/migrations/0003_accounts_profiles_friends.sql`

Roda depois da 0001 e da 0002. **Nada foi executado em banco nenhum**: é você quem cola no SQL Editor.

- `profiles` (já existia na 0001, com `user_id`, `nick`, `created_at`) ganha `display_name`, `avatar_url`, `public_key`,
  `fingerprint`, `status`, `nick_auto`, `updated_at`, com `check` de formato em cada uma.
- `friendships` (pedido/aceito, um vínculo por par).
- RLS ligado em tudo. **Nenhuma escrita direta**: só as funções `account_sync_profile`, `account_set_nick`,
  `account_set_status`, `friends_list`, `friend_request`, `friend_respond`, `friend_remove`, que conferem `auth.uid()`.
- Leitura direta de `profiles`: só `user_id`, `nick`, `nick_changed_at`, `created_at`, e só para quem está logado
  (privilégio por coluna). Nome, avatar, chave e status saem apenas pelas funções: para o dono e para amigos aceitos.
- `friendships`: cada pessoa lê só os vínculos de que faz parte.
- Papel `anon` não lê tabela nem chama função.

**Mudança que afeta o site (apps/web)**: antes qualquer visitante, mesmo sem login, lia `profiles` e `profile_keys`.
Agora só quem está logado. O site consulta essas tabelas com a pessoa logada, então deve seguir funcionando, mas isso
não foi testado. Se preferir manter o acesso anônimo, diga antes de aplicar.

**Grupos**: não criei tabelas. Os grupos do Briks são manifestos P2P assinados que vivem em cada aparelho; guardá-los no
servidor é outra decisão de produto (quem é a fonte da verdade). Fica para depois, se você quiser.

Validação feita: o arquivo passa no parser real do Postgres (SQL e PL/pgSQL, via `pglast`). **Não foi executado**:
comportamento das políticas e das funções só fica provado no projeto de desenvolvimento.

## 4. Passos manuais (você)

Nunca me envie nem grave no repositório: senha do banco, `service_role`/`sb_secret_…`, client secrets do Google/Discord.
Os secrets são colados **só no painel do Supabase**. O app recusa uma chave secreta se ela for configurada por engano.

1. **SQL**: no SQL Editor do projeto de desenvolvimento, rode `0001`, `0002` e `0003`, nessa ordem.
2. **Redirects**: Authentication > URL Configuration > Redirect URLs, adicione as três:
   `http://127.0.0.1:53682/callback`, `http://127.0.0.1:53683/callback`, `http://127.0.0.1:53684/callback`.
3. **Google**: Google Cloud Console > APIs & Services > tela de consentimento OAuth (tipo Externo; enquanto estiver em
   "Testing", adicione seu e-mail como usuário de teste) > Credentials > Create credentials > OAuth client ID > tipo
   **Web application** > Authorized redirect URI: `https://<ref-do-projeto>.supabase.co/auth/v1/callback`.
   Cole o Client ID e o Client Secret em Supabase > Authentication > Providers > Google e ative.
4. **Discord**: Discord Developer Portal > New Application > OAuth2 > Redirects:
   `https://<ref-do-projeto>.supabase.co/auth/v1/callback`. Cole Client ID e Client Secret em Supabase >
   Authentication > Providers > Discord e ative.
5. **E-mail**: Authentication > Providers > Email: ative "Confirm email" e senha mínima de 8. Opcional e recomendado:
   em Email Templates ("Confirm signup" e "Reset password") inclua `{{ .Token }}` no texto, para a pessoa poder digitar
   o código de 6 dígitos no app em vez de depender do link. O SMTP embutido do Supabase envia poucos e-mails por hora;
   para uso real será preciso um SMTP próprio.
6. **Configurar o app** (só dados públicos), de um dos jeitos:
   - arquivo `%APPDATA%\Briks-CUI\supabase.json`: `{"url":"https://<ref>.supabase.co","anon_key":"<chave anon/publishable>"}`
   - ou variáveis `BRIKS_SUPABASE_URL` e `BRIKS_SUPABASE_ANON_KEY`.
7. Abra o app e faça **você** o login com Google e com Discord no navegador. Me diga o que aconteceu.

### Situação do projeto de desenvolvimento (10/11/2026)

Conferido pela rede, só com a chave pública e sem criar conta nem fazer login:

- Google, Discord e e-mail estão ativos; confirmação de e-mail ligada.
- O endereço de login social leva ao Google e ao Discord (o app chega até abrir o navegador).
- Senha errada, refresh token inválido e código PKCE inválido voltam como os nossos erros em português. O formato real
  do refresh inválido (`validation_failed: Refresh token is not valid`) era diferente do previsto e foi corrigido.
- **As migrações ainda não foram aplicadas** (a tabela `profiles` e as funções não existem). Sem elas o login entra,
  mas o perfil não é publicado e a lista de amigos avisa "o servidor ainda não foi preparado".
- `%APPDATA%\Briks-CUI\supabase.json` já foi criado neste computador (fora do git), com a URL e a chave publishable.

O que falta você fazer para o login completo:

1. Authentication > URL Configuration > **Redirect URLs** > Add URL, uma por vez, exatamente assim:
   `http://127.0.0.1:53682/callback`, `http://127.0.0.1:53683/callback`, `http://127.0.0.1:53684/callback`.
   Não dá para saber de fora se já estão lá. Sintoma de que faltam: depois do login o navegador vai para outra página
   (a "Site URL") em vez de mostrar "Tudo certo!", e o app fica em "Aguardando o navegador…".
2. Rodar `0001`, `0002` e `0003` no SQL Editor, nessa ordem (passo 1 da lista acima).
3. No Google, como o app está em modo Teste, entrar com um e-mail cadastrado como usuário de teste.

Teste repetível: `cargo test --release -- --ignored real_supabase` com `BRIKS_SUPABASE_URL` e
`BRIKS_SUPABASE_ANON_KEY` no ambiente.

## 5. Testes

`cargo test --release` em `apps/desktop-cui/briks-core` (51 testes, 22 do módulo de contas; nenhum usa a internet: o
cliente Supabase de verdade fala com `fake.rs` em 127.0.0.1):

- PKCE: vetor da RFC 7636; verificador aleatório; a URL leva o desafio e nunca o verificador.
- Loopback: código, cancelamento, link vencido, caminhos estranhos, prazo.
- DPAPI: o arquivo não contém token nem e-mail em texto aberto; blob adulterado não abre.
- Configuração: `service_role` e `sb_secret_` recusadas; http recusado; ordem ambiente > arquivo.
- Fluxos: login social, cancelar, código roubado inútil, cadastro com confirmação (link e código), e-mail não
  confirmado, senha errada, e-mail já usado, recuperação (link e código), renovação automática e simultânea, servidor
  fora do ar (não apaga a sessão), sessão revogada (apaga), sair.
- Perfil/amigos: só a chave pública é publicada; pedido, aceite (só quem recebeu), dados sensíveis só após o aceite.
- Importação: reatribui ao usuário novo, não importa contas antigas, não sobrescreve, origem intacta.

Modo de desenvolvimento (sem Supabase): `BRIKS_ACCOUNTS_MOCK=1` (a tela avisa que as contas são de mentira).
Extras: `BRIKS_ACCOUNTS_MOCK_CONFIRM=1` exige confirmar e-mail (código `123456`); `BRIKS_ACCOUNTS_MOCK_DENY=1` simula
cancelamento. Pasta de dados isolada: `BRIKS_CUI_DATA` + `BRIKS_CORE_DATA`. Capturas das telas: `BRIKS_DEMO=login` com
`BRIKS_DEV_ACCT=login|signup|recover|browser|email|newpass|error|setup`.

Rodado no app real em modo falso, em pasta isolada: entrar com "Google", ponte com o banco local, lista de amigos,
Configurações com Sair, e importação de um Briks antigo fabricado (3 amigos, 1 grupo, 1 mensagem; origem com o mesmo
hash antes e depois).

**Não testado**: Supabase real (login Google/Discord, e-mails, RLS e funções SQL em execução, formato exato dos erros
reais), o site com a 0003 aplicada, dois aparelhos na mesma conta, e os cliques da tela de e-mail no app (os fluxos de
e-mail estão cobertos no core, não na UI).

## 6. Fase 2 (desenhada, NÃO implementada): cópia cifrada da identidade, recuperável por frase

Objetivo: usar a mesma identidade P2P em outro PC, ou recuperá-la depois de formatar, sem o servidor jamais conseguir
ler a chave privada.

- No aparelho: frase de recuperação → Argon2id (64 MiB, 3 passadas, sal aleatório de 16 bytes) → chave de 32 bytes →
  XChaCha20-Poly1305 sobre a chave privada (32 bytes), com `user_id` + versão como dado autenticado.
- No servidor: tabela `identity_backups(user_id, versão, parâmetros do KDF, sal, nonce, texto cifrado, impressão
  digital, atualizado_em)`, RLS só do dono, escrita por função. O servidor guarda só o que não consegue abrir.
- Outro aparelho: entra na conta, baixa o pacote, pede a frase, decifra, confere a impressão digital com a do perfil.
- Sem a frase não há recuperação (nem por nós). Esse é o preço de o servidor não poder ler.

Decisões que são suas, antes de eu implementar:
1. Frase **gerada pelo app** (12 palavras, 128 bits; resiste a força bruta mesmo se o banco vazar) ou **escolhida pela
   pessoa** (mais cômoda, bem mais fraca)? Recomendo a gerada.
2. Oferecer no primeiro login ou só em Configurações?
3. Ao entrar num segundo aparelho sem a frase: criar identidade nova (os amigos veem a chave mudar) ou bloquear?

## 7. Pendências e riscos conhecidos

- `core-identity.json` guarda a chave privada P2P em texto aberto no disco (já era assim antes desta tarefa). Convém
  cifrá-la com a mesma DPAPI; é uma mudança pequena, fora do escopo pedido.
- Troca de chave pública de um amigo (outro aparelho) é aceita sem aviso. Um aviso "a chave de Fulano mudou" faz
  sentido junto com a fase 2.
- Recuperação de senha e confirmação pelo **link** só funcionam com o app aberto neste computador (o link aponta para
  o loopback). Fora disso, vale o código de 6 dígitos (passo 5 da seção 4) ou entrar com e-mail e senha após confirmar.
- Sem bloqueio de pessoas e sem limite por tempo nos pedidos de amizade (há teto de 50 pendentes).
- O marcador da importação é por computador, não por conta: uma segunda conta no mesmo PC não reimporta.
- Os botões Google/Discord são clicáveis só com o mouse (sem foco por teclado).
- Integração do branch `cui-instalador`: `import.rs`, a linha do `rusqlite` e o subcomando `import-electron` foram
  trazidos idênticos para o merge sair limpo; `migrate.c` (assistente de migração da UI) não foi trazido.
