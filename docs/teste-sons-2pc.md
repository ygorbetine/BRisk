# Painel de sons: teste em 2 PCs (1.5)

Roteiro humano para o que os testes automáticos não alcançam: som saindo de verdade nas caixas, dois aparelhos, rede real.
Plano e decisões: `docs/plans/painel-de-sons.md`. Código: `briks-core/src/{sons,sonsnet}.rs`, `src/sons.c`.

## Preparação
- PC A e PC B com o mesmo build (core em nível 3: `CORE_PROTO` = 3; versão antiga avisa "motor de outra versão").
- Duas contas, amigas entre si, e um **grupo** com A como dono e B como membro (B não é administrador).
- Arquivos de teste: `curto.wav` (1 s), `longo.mp3` (7 s, volume alto), um áudio de 40 s (ou um vídeo `.mp4` com som) e um `.txt` renomeado para `.mp3`.
- Fones ou caixas nos dois PCs. Faça uma rodada com os PCs na mesma rede (P2P) e outra com um deles em outra rede/4G (SFU, se o servidor de chamadas estiver ativo).

## 1. Meus sons (pessoal)
1. A entra numa chamada com B (sala avulsa, fora de grupo).
2. A abre **Sons** > **Adicionar**, escolhe `curto.wav`, dá nome e emoji, destino **Meus sons**, marca "Tenho direito de usar" e confirma.
   - Esperado: o botão Adicionar só liga depois de marcar a caixa; **Ouvir** toca só em A; o som aparece em "Meus sons".
3. A clica no som. Esperado: **A e B ouvem** (B baixa o arquivo na hora; se o som atrasar mais de ~6 s, B não toca, para não sair fora de hora).
4. A clica de novo logo em seguida. Esperado: nada toca (intervalo de 3 s por pessoa) e aparece a nota do intervalo.
5. A clica passados 3 s: toca de novo nos dois.

## 2. Mini editor, limites e conversão
- O botão **Adicionar** aceita áudio (mp3, wav, ogg, m4a, flac…) **ou vídeo** (mp4, webm, mov…): o áudio é extraído. O editor mostra a onda, "Início" e "Fim" (o trecho nunca passa de **15 s**) e o volume.
- Áudio de 40 s: o trecho padrão já vem com 15 s; mover "Início" empurra o "Fim" (e vice-versa) para manter no máximo 15 s e no mínimo 0,2 s. **Testar** toca só o trecho, com o volume escolhido, só em você.
- Ao adicionar, o trecho é convertido para MP3 (44,1 kHz) e **comprimido**: 15 s ficam em ~240 KB. O limite final é 512 KB. Sem alterar nada (arquivo mp3/wav/ogg de até 15 s e 512 KB), vai o arquivo original.
- Um `.txt` renomeado para `.mp3`: o editor recusa ("Não consegui ler o áudio"). Nada entra no painel.
- `longo.mp3` com volume alto: toca, mas **sem estourar** (pico limitado a -3 dBFS).
- Volume do editor em 40%: o som sai mais baixo nos dois PCs e fica assim gravado no arquivo.

## 3. Sons de grupo (permissão)
1. A (dono) abre Sons > Adicionar, escolhe `curto.wav`, destino **Grupo · (nome)**. B, ao entrar na chamada do **canal de voz do grupo**, vê o som aparecer sozinho (catálogo no manifesto assinado) e o arquivo chega por P2P.
2. B abre o painel: o som do grupo aparece **sem** "Renomear/Apagar" nas opções (membro só dispara). B tentando adicionar: o destino "Grupo" não aparece na lista.
3. A promove B a administrador. B agora consegue adicionar e apagar sons do grupo; A vê a mudança.
4. Fora da chamada do grupo (outra sala), o som do grupo fica apagado e o clique avisa que só toca na chamada dele.
5. Um som **pessoal** de A dispara em qualquer chamada, inclusive de outro grupo; quem não tem o arquivo baixa de A, com conferência de SHA-256.

## 4. Silenciar, ensurdecer, volume
- B ensurdece: **nenhum** som toca em B (e o core de B nem repassa o evento). Ao voltar a ouvir, volta a tocar.
- B marca **Silenciar todos os sons**: nada toca em B, as vozes continuam normais.
- B abre "Silenciar pessoas…" e silencia A: os sons de A não tocam em B; os de outras pessoas, sim. Desfazer volta ao normal.
- Volume dos sons em B a 30%: só os sons ficam mais baixos, a voz não muda.
- Intervalo por pessoa em 10 s: disparos de A mais próximos que isso são ignorados em B.

## 5. Rede e robustez
- A dispara um som e B está com a rede lenta: B baixa, e só toca se chegar a tempo.
- B sai e entra na chamada no meio de um download: sem travar; o som falta e é pedido de novo quando um par autenticar.
- Fechar a chamada com um som tocando: o som para junto.
- Janela estreita (~800 px) e a janela bem baixa: o painel rola por dentro, sem cantos retos vazando nem cortes.

## 6. Ao encerrar o app
Depois dos testes acima, nos três jeitos de sair (menu Sair, fechar a janela, encerrar pelo Gerenciador de Tarefas), conferir que **não sobra `briks-core.exe`** (`powershell -File scripts/teste-encerramento.ps1` faz isso automaticamente).

## Limitações conhecidas (não são defeitos deste teste)
- **Versões misturadas:** quem está na 1.4.4 reserializa o snapshot do grupo sem o campo `sounds`; a assinatura de um catálogo com sons não confere lá e o app antigo **ignora a atualização**. Todo o grupo deve estar na 1.5 para usar sons de grupo. Grupos sem sons mantêm as assinaturas antigas intactas.
- **Catálogo grande:** o snapshot com sons é limitado a 14000 bytes para caber num pacote do canal de dados do SFU (15000). Isso dá folga para o limite de 50 sons por grupo com nomes curtos; nomes longos + emoji + muitos membros podem estourar e a edição é recusada com aviso.
- O som toca no dispositivo de saída escolhido nas configurações (waveOut), **não** é misturado ao microfone: quem tem fone e mic separados ouve normal; ninguém ouve o som "pela voz" de A, cada aparelho toca o seu.
- O limite de pico (-3 dBFS) vale para tudo que passa pelo editor (já gravado no arquivo convertido); um mp3/ogg importado sem alteração é limitado na reprodução.
- OGG precisa do decodificador do Windows ("Extensões de mídia da Web"); sem ele o editor avisa que não conseguiu ler o áudio. MP3, WAV, vídeos e a maioria dos formatos comuns funcionam sem nada extra.
- O editor lê até os primeiros 120 s do arquivo.
- Atalho de teclado para os sons: só dentro do app, e fica para depois da 1.5.
