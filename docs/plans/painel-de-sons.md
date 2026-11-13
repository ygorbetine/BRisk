# Plano: painel de sons (soundboard), versão 1.5

Pedido do usuário em 2026-11-12, com o soundboard do Discord como referência. Este plano é para a 1.5, depois da 1.4.4. Os esforços são estimativas de trabalho de uma sessão de desenvolvimento, não prazos garantidos. Antes de começar, confirmar nos arquivos citados o que já existe (marcado "reaproveitar").

## 1. Decisões já tomadas
- Os sons ficam **só entre os aparelhos** (sem servidor de arquivos na 1.5). Motivo: privacidade, custo, moderação e LGPD.
- Quem toca o som é **cada aparelho localmente**; o disparo é uma mensagem pequena no canal de dados da chamada. O microfone não é remixado.
- Só membros do grupo recebem e disparam. Dono e administradores decidem quem pode adicionar.

## 2. O que o usuário vê
- Botão "Sons" na barra da chamada, ao lado de "Áudio". Abre o painel (popover grande, responsivo, com rolagem interna).
- Busca no topo; seções: Favoritos, Usados com frequência, Sons do grupo (um bloco por grupo), Meus sons.
- Cada som é um botão com emoji e nome. Clicar toca para todos na chamada. Botão direito: favoritar, pré-ouvir só para mim, renomear, apagar.
- "Adicionar som": escolhe um arquivo, define nome e emoji (seletor completo de emojis), pré-ouve, salva em Meus sons ou em um grupo.
- Controles: volume do painel, "silenciar sons de [pessoa]", "silenciar todos os sons", intervalo mínimo entre sons.
- Sons de grupos onde a pessoa não pode usar aparecem com cadeado.

## 3. Limites e regras
- Formatos: mp3, wav, ogg. Duração máxima 8 s, tamanho máximo 512 KB, máximo de 50 sons por grupo (ajustável pelo dono).
- Intervalo mínimo de 3 s por pessoa entre disparos (configurável pelo dono do grupo); disparo em rajada é descartado.
- Quem adiciona fica registrado (id da conta) para o dono poder apagar e punir abuso.
- Cargo: dono e administradores adicionam e apagam; membros só disparam (configurável).

## 4. Arquitetura
### 4.1 Armazenamento local
- Pasta `sons\` na pasta de dados do app, com os arquivos nomeados pelo hash SHA-256 e um índice `sons.json` (hash, nome, emoji, grupo, autor, favorito, contagem de uso, última vez usado).
- Cache com limite (ex.: 200 MB) e limpeza dos menos usados.

### 4.2 Catálogo do grupo
- O catálogo (lista de hash, nome, emoji, autor, tamanho) vai junto do manifesto assinado do grupo (reaproveitar `grupos-p2p-core`: a assinatura do dono/administradores já impede adulteração). Adicionar ou apagar som = nova versão do manifesto.
- Quem não é administrador não consegue publicar entrada no catálogo (verificação de assinatura no cliente).

### 4.3 Transferência do arquivo
- Reaproveitar o caminho de anexos P2P (`attach.c` e o transporte em pedaços do core): o aparelho que vê um hash desconhecido pede o arquivo a quem estiver online e o tiver.
- Verificação: o SHA-256 do que chegou precisa bater com o do catálogo, e o tamanho e a duração respeitam os limites; senão descarta.
- Pedido em fila com prioridade baixa para não competir com voz e transmissão.

### 4.4 Disparo e reprodução
- Mensagem `sound.play {hash, nonce}` no canal de dados da chamada, assinada pelo remetente (reaproveitar a mesma autenticação das mensagens de chat).
- O receptor confere: remetente é membro com permissão, intervalo mínimo respeitado, não está silenciado pelo usuário. Se tem o arquivo, toca; se não, baixa e toca, ou pula.
- Reprodução: decodificar mp3/ogg/wav no core (decodificador já usado para o preview de áudio do chat, a conferir) e mandar para a saída de áudio da chamada, respeitando o volume do painel e o "ensurdecer". Ensurdecido não toca som.
- Reprodução em sala de SFU ou P2P funciona igual, porque o disparo vai pelo canal de dados e não pela mídia.

### 4.5 Atalhos de teclado
- Atalho global opcional por som favorito (reaproveitar o sistema de atalhos, se existir; caso contrário só dentro do app na 1.5).

## 5. Etapas e esforço (estimativa)
| Etapa | Conteúdo | Esforço |
|---|---|---|
| 1 | Armazenamento local, índice, importação com validação (formato, duração, tamanho) e pré-ouvir | 0,5 a 1 dia |
| 2 | Painel de UI: busca, seções, favoritos, adicionar/renomear/apagar, seletor de emoji, responsivo | 1 a 1,5 dia |
| 3 | Catálogo no manifesto do grupo e permissões por cargo | 1 dia |
| 4 | Transferência P2P com hash, fila e cache | 1 a 1,5 dia |
| 5 | Disparo, validações do receptor, reprodução e silenciar por pessoa | 1 dia |
| 6 | Testes (cargo, roteiros em loopback com 3 processos), paridade, smoke e instalador | 1 dia |
| Total | | 5,5 a 7 dias de sessão (algumas etapas em paralelo reduzem) |

## 6. Riscos
- **Abuso (sons ofensivos ou altos):** mitigado por cargos, limite de duração, normalização de volume na importação (limitar o pico) e silenciar por pessoa.
- **Direitos autorais:** o app só guarda o arquivo no aparelho e passa entre amigos do grupo; ainda assim, incluir aviso de responsabilidade do usuário ao adicionar e uma política de uso.
- **Sem servidor:** um som novo só chega a quem estiver online com alguém que o tenha. Aceito na 1.5; um catálogo em servidor fica para uma versão futura.
- **Banda em salas grandes:** muitos pedidos simultâneos do mesmo arquivo; mitigar pedindo a poucos pares e limitando concorrência.
- **Latência do primeiro uso:** quem não tem o arquivo ouve com atraso ou pula; mostrar indicador "baixando".

## 7. Critérios de pronto
- Três aparelhos em loopback ou rede: A adiciona um som ao grupo, B e C o recebem com hash verificado, A dispara e os três ouvem; B silencia A e não ouve mais.
- Disparo em rajada é descartado; arquivo corrompido ou com hash diferente é recusado; membro sem permissão não consegue adicionar nem apagar.
- Painel legível e utilizável em janela pequena, sem texto cortado e sem cantos vazando.
- Sem processo `briks-core` sobrando nas três saídas (normal, fechar janela, matar).

## 8. Perguntas para decidir antes de começar
1. Membros comuns podem adicionar sons ao grupo, ou só dono e administradores?
2. O limite de 8 s e 512 KB está bom?
3. Atalho de teclado global é necessário na 1.5, ou só dentro do app?
4. Os sons "Meus sons" (pessoais) podem ser usados em qualquer grupo, ou só são tocados para quem os tem?
