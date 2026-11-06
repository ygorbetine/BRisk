# Plano de ação: grupos por @nick

Objetivo: parar de depender de "chave invisível por instalação" para saber quem é quem. Com o @nick
(já entregue na 0.5.x), o mesmo humano deve ser reconhecido em qualquer instalação ou computador,
sem cópias soltas na lista e sem a pessoa "entrar mas ninguém se ver".

## O que já existe (0.5.x)
- Conta (e-mail + senha) e @nick único no Supabase; cada instalação registra sua chave (`profile_keys`).
- Leitura pública: qualquer instalação consegue perguntar "essa chave pertence a qual @nick?".
- Escrita só por funções que conferem o login. Nick e chave não são editáveis direto.

## Como o grupo funciona hoje (e por que falha)
- Um membro é uma identidade = `peerId` (UUID aleatório) + chave pública. Dono e admins também são `peerId`.
- Quem não está na lista local do grupo não é aceito na chamada nem no chat.
- A lista só muda por "snapshot" assinado pelo dono/admin. Se alguém ganha uma identidade nova
  (outro computador, dados limpos), ela não está na lista e ninguém a vê até o dono aprovar na mão.

## Regra de confiança escolhida (não muda a segurança atual)
Dono/admin continuam sendo quem **assina** a lista. O @nick só serve de **prova de que uma identidade nova
é da mesma pessoa de um membro já aceito**:
1. O banco diz: chave K / peerId P pertence ao @nick N (só o dono da conta N consegue registrar isso).
2. O grupo já tem um membro M cuja chave local é a mesma que o banco registrou para o @nick N.
3. Então P é outra instalação da mesma pessoa que M, e o dono/admin pode inscrevê-la sem aprovação manual.
Ninguém além de dono/admin altera a lista. Quem não registrou @nick continua como hoje.

## Fases

### Fase 1 (0.6.0) — ver o @nick e inscrever instalações novas
1. **Diretório de @nick**: serviço que pergunta ao Supabase `peerId → @nick` em lote (com cache curto) e só
   considera o vínculo **verificado** quando a chave do banco é idêntica à chave que o grupo guardou.
2. **Lista de membros**: mostra `@nick` ao lado do nome e agrupa instalações do mesmo @nick numa linha só
   (hoje agrupa só por nome, que falha se a pessoa mudar o nome).
3. **Inscrição automática (só dono/admin)**: ao ver na presença do grupo uma identidade desconhecida cujo
   @nick pertence a um membro verificado, adiciona essa identidade ao grupo e reenvia a lista a todos.
   Limite do grupo (48) e lista de removidos continuam respeitados: removido não volta sozinho.
4. **Visual**: título da tela de conta com a fonte do app e o campo do @nick com uma caixa só.
5. **Testes**: regra de decisão em função pura (quem entra, quem não entra), agrupamento por @nick.

### Fase 2 (0.6.1) — admin de reserva por @nick (ENTREGUE)
- O dono promove sozinho, com concessões de admin assinadas por ele, toda instalação **verificada** de um admin
  ou do próprio dono (mesma chave que o banco registrou para aquele @nick). Se o dono ganhar outro computador,
  esse computador vira admin e consegue gerir o grupo (convidar, editar canais, reenviar a lista).
- Não mexe na verificação criptográfica: a autoridade continua sendo a concessão assinada pelo dono.
- Limite conhecido: admin não troca de dono nem remove outros admins. Se o computador do dono se perder, o
  grupo continua gerido pelo admin de reserva, mas a posse continua na chave antiga.

### Fase 2b (0.6.2) — transferência de posse
- Recurso novo e assinado: o dono (ainda com a chave) transfere a posse para outro membro, ou para a nova
  instalação do mesmo @nick. Exige alterar `ownerPeerId` no manifesto, nos certificados de remoção e nas
  concessões de admin (cerca de 40 pontos do código), com testes de duas máquinas. Por isso não foi misturado
  com a Fase 2. Sem a transferência feita antes, o dono perdido não é recuperável.

### Fase 3 (0.7.1) — convite por @nick (ENTREGUE)
- "Adicionar @fulano": busca no Supabase e adiciona direto (sem trocar códigos), para quem já tem conta.
- Mantém o código atual para quem ainda não tem conta.

### Fase 4 — limpeza (ENTREGUE na Fase 1: agrupa por @nick) e endurecimento (PENDENTE: exige Edge Function no Supabase)
- "Limpar duplicados" passa a olhar o @nick (remove cópias antigas do mesmo @nick que o dono escolher).
- Endurecer `register_key`: hoje quem conhece o `peerId` de outra pessoa (por estar no mesmo grupo) poderia
  registrá-lo antes dela com a chave errada, o que só impediria o vínculo dela (negação de serviço, sem
  acesso indevido). Correção: o app assina `(conta, peerId)` com a chave e uma Edge Function confere a assinatura.
- Migração dos membros antigos sem @nick: aparecem como "Sem @nick" até a pessoa entrar com a conta.

## Riscos e decisões
- O Supabase vira ponto de confiança do vínculo @nick ↔ chave (já aceito no plano original).
- Confirmação de e-mail desligada: qualquer um cria conta com e-mail que não é dele. Aceitável entre amigos;
  ligar de novo se o app crescer.
- A inscrição automática exige que a instalação nova já tenha o grupo localmente (aparece na presença do grupo).
  Computador totalmente novo ainda entra pelo código de convite até a Fase 3.
- Sem teste com várias máquinas reais: cada fase termina com roteiro de teste para os amigos.

## Decisões revistas (0.7.4)
- **Transferência de posse (2b): NÃO implementada.** A posse está amarrada ao `peerId` do dono em ~100 pontos (manifesto, concessões de admin, certificados de remoção). Trocar isso sem teste em duas máquinas pode abrir brecha de segurança ou derrubar todos os admins. As chaves privadas são não exportáveis de propósito, então "mover a identidade" também não existe. Mitigação atual: o admin de reserva (Fase 2) mantém o grupo gerido se o dono ganhar outro computador.
- **Endurecer `register_key`: a correção planejada não resolve.** Assinar `(conta, peerId)` com a chave prova posse da chave, não do `peerId`; o `peerId` é um UUID aleatório, então um atacante assinaria com a própria chave e ainda registraria o `peerId` da vítima. A solução real é derivar o `peerId` da chave pública (mudança de protocolo, quebra identidades existentes). Risco atual: só negação de serviço do vínculo @nick.
