# Ativar contas e @nick (Briks 0.5.0)

O app já traz a tela de conta. Falta preparar o banco no Supabase (uma vez só):

1. Abra o painel do Supabase do projeto do Briks > **SQL Editor** > **New query**.
2. Cole o conteúdo de `supabase/migrations/0001_profiles_nick.sql` e clique em **Run**.
3. Em **Authentication > Providers > Email**, deixe o login por e-mail ligado.
   - Para testar rápido, desligue **Confirm email** (a conta entra na hora).
   - Com **Confirm email** ligado, quem cria a conta precisa abrir o link do e-mail antes de entrar.
4. Em **Authentication > Rate Limits**, confira o limite de e-mails por hora (o plano grátis envia poucos).

Pronto: no app, clique no ícone **@** ao lado do seu nome (barra inferior esquerda), crie a conta, reserve o @nick.
Cada computador que entrar na mesma conta é ligado automaticamente ao mesmo @nick.

## O que esta versão faz (e o que ainda não)
Faz: conta (e-mail + senha), @nick único (3–20 caracteres `a-z 0-9 _`, troca a cada 30 dias),
registro da chave de cada instalação, lista e revogação de outros computadores.

Ainda não faz (próximas versões, ver `plano-nick.md`): dono/admin e membros de grupo por @nick,
convite por @nick, ligação dos membros antigos, e checar o @nick na autenticação da chamada.
Hoje o @nick aparece na sua conta; os grupos continuam funcionando como antes.
