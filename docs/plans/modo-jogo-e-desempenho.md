# Plano: Modo Jogo e desempenho adaptativo no Briks

Origem da inspiração: a branch `feat/game-mode-remote-input` do repositório `xxsamboladiones/Risk-P2P` (AGPL-3.0), lida em 2026-11-08. Nada foi copiado. Este plano descreve o que absorver, o que reescrever para o Briks e em que ordem.

## 1. O que foi analisado

| Branch do repositório de origem | Situação |
|---|---|
| `feat/game-mode-remote-input` | 34 commits à frente da base comum, 159 arquivos. Contém tudo das outras branches ativas. |
| `feat/vpn-direct-webrtc` | 5 commits, já incluídos na anterior (temas, recuperação de microfone, ressincronização de grupos). |
| `hotfix/*`, `fix/*`, `feature/*`, `work/*`, `update-alpha-0.2.0`, `agent/*` | 0 commits à frente de `main` deles: já foram incorporadas lá, sem conteúdo novo. |

Base comum com o Briks: o commit `5b3611c`. Depois dela, o Briks tem 69 commits e a origem tem 34. Dos 159 arquivos da origem, cerca de 57 também mudaram aqui (conflito provável) e 100 são novos.

Conclusão: só vale olhar a branch do Modo Jogo. As demais já estão na `main` deles, e o que existe lá de estabilidade de chamada (reconexão de sinalização, recuperação de autenticação de peers) o Briks cobre com o próprio caminho SFU/LiveKit.

## 2. O que dá para absorver, por valor

1. **Modo Jogo (entrada remota).** Valor alto, risco alto.
2. **Política de desempenho adaptativa.** Valor alto para PCs fracos, risco médio.
3. **Pequenos itens independentes.** Valor médio, risco baixo:
   - som de notificação de mensagem;
   - copiar texto com fallback e colar imagem no chat;
   - perfil de aparência com temas escuros;
   - captura de jogos em tela cheia com DXGI (API de captura do Windows que pega superfícies de tela cheia exclusivas).

## 3. Modo Jogo: desenho para o Briks

### 3.1 Como funciona na origem

- O anfitrião compartilha a tela e liga o Modo Jogo. Participantes autenticados pedem para entrar e recebem uma concessão (`grantId`) por sessão.
- Um jogador usa teclado e mouse por vez. Até quatro usam controle.
- Controle de sessão (iniciar, parar, pedir, aceitar, rejeitar, revogar) passa por um canal confiável. A entrada passa por outro canal, sem ordenação e sem retransmissão, com pacotes de até 4 KiB.
- Cada pacote leva `sessionId`, `grantId` e sequência crescente. Pacotes repetidos, antigos ou de outra concessão são descartados.
- No anfitrião, um backend nativo injeta a entrada: SendInput no Windows, uinput no Linux, controle Xbox 360 virtual por ViGEmBus no Windows.
- Segurança: após 250 ms sem entrada, o watchdog solta teclas, botões e eixos. A sessão cai sem batimento do anfitrião em 5 s. Há atalho global de emergência e Esc pausa a captura.
- Qualidade: começa em 720p60 e sobe para 1080p60 quando RTT, perda, jitter e tempo de codificação permitem. O receptor pede `jitterBufferTarget = 0`.

### 3.2 O que muda no Briks

O Briks usa SFU (LiveKit) como transporte principal, além do mesh P2P. A origem foi escrita para mesh. Adaptações:

- **Canal de entrada.** No SFU não há canal de dados direto entre dois peers. Usar `publishData` do LiveKit com `reliable: false` e destino só no anfitrião (`destinationIdentities`). Manter o formato do pacote (versão, sessão, concessão, sequência) e a validação estrita.
- **Controle de sessão.** Mensagens confiáveis no mesmo mecanismo de dados já usado pelo app (`sendData` do transporte), com namespace próprio.
- **Identidade.** A concessão deve ser atrelada ao `peerId` autenticado do Briks (identidade do participante no LiveKit), nunca ao que o pacote declara.
- **Qualidade.** Reaproveitar o medidor de envio que já existe (`summarizeSendMeter`, estatísticas do publisher do LiveKit) em vez da política da origem, que depende de estatísticas por peer do mesh.
- **Anfitrião e Electron.** O backend de entrada fica no processo Rust do Briks (`desktop-backend`), com rotas protegidas pelo token local, como as demais.

### 3.3 Segurança (obrigatório)

Injetar teclado, mouse e controle no PC do anfitrião é a parte sensível do projeto. Regras:

- Desligado por padrão. O anfitrião liga explicitamente, vê quem entrou e pode revogar a qualquer momento. Revogado não volta na mesma sessão.
- Nenhum acesso persistente, execução de comandos, transferência de arquivos ou instalação silenciosa de driver.
- Só teclas de uma tabela fixa e validada (a origem usa `game-keys.json`). Esc reservado para sair.
- Watchdog de neutralização, expiração por falta de batimento e atalho global de emergência.
- Limite de deslocamento do mouse por atualização e primeiro frame após pausa sem movimento acumulado.
- Aviso claro no Windows: SendInput respeita o nível de integridade do processo. Jogos elevados ou que recusam entrada sintética podem não aceitar. Não contornar isso.
- Revisão independente do código nativo antes de liberar, e testes com backend falso que não enviam teclas ao sistema.

### 3.4 Fases

Cada fase vira uma versão separada, com testes, e só segue se a anterior estiver estável.

| Fase | Entrega | Critério de pronto |
|---|---|---|
| 1. Protocolo | Tipos, validação estrita, tabela de teclas e testes de pacotes inválidos, repetidos e de sessão antiga. Sem UI. | Testes passando, sem mudar comportamento do app. |
| 2. Sessão e permissão | Liga/desliga do anfitrião, pedido e aceite, revogação, estado para quem entra depois, painel do anfitrião. Sem entrada ainda. | Dois clientes reais conseguem pedir, aceitar e revogar. |
| 3. Teclado e mouse (Windows) | Captura com Pointer Lock e `KeyboardEvent.code`, canal de entrada, backend SendInput, watchdog, atalho de emergência. | Entrada chega, solta tudo ao pausar, cai sem batimento. |
| 4. Qualidade para jogo | Perfil 720p60 e 1080p60 e `jitterBufferTarget = 0` durante o jogo. Medição de latência real. | Números medidos em rede real, sem prometer latência de serviço dedicado. |
| 5. Controle virtual | Gamepad API no participante e Xbox 360 virtual por ViGEmBus. Driver instalado pelo usuário, nunca pelo app. | Funciona em jogo real. |
| 6. Linux (opcional) | uinput, se houver demanda. | Validado em máquina Linux real. |

### 3.5 Riscos e perguntas em aberto

- Latência total (captura, codificação, rede, injeção) só se mede em uso real. Não prometer comparação com serviços dedicados.
- Jogos em tela cheia exclusiva e proteção anti-trapaça podem recusar entrada sintética.
- ViGEmBus é um driver de terceiros; avaliar manutenção e licença antes da fase 5.
- Sobreposição do Modo Jogo com a sobreposição do Briks (Ctrl+Shift+O): definir quem tem prioridade de atalhos globais.
- Decidir se vale suportar o mesh também ou só SFU.

## 4. Desempenho adaptativo

### 4.1 Ideias da origem que valem

- Um coordenador central transforma sinais baratos em uma política de execução, e cada módulo consome só o que precisa. Estados: em primeiro plano, chamada visível, chamada em segundo plano e oculto/ocioso. A pressão de mídia (normal, aviso, sobrecarga) é um eixo separado.
- Piso de qualidade de 480p/30. Nunca abaixo disso.
- Medir antes de otimizar: benchmark reproduzível e relatório. Só mantém uma mudança se o perfil mostrar ganho.
- Suspender trabalho invisível quando a chamada está minimizada: diagnósticos, análise de voz, animações. Permitir o throttling do Chromium só quando seguro.

### 4.2 Aplicação no Briks

Pontos concretos a investigar primeiro (hipóteses, não provas):

- Diagnósticos ao vivo e análise de atividade de voz rodando com a chamada minimizada.
- `backgroundThrottling: false` na janela principal durante toda a vida do app.
- O tamanho do bundle principal e o carregamento sob demanda de telas pesadas.
- Amostragem de voz por stream e criação repetida de `MediaStream` nas prévias.

Fases:

1. **Medição.** Script de benchmark e linha de base em máquina fraca (por exemplo, Intel de terceira geração com vídeo integrado) com chamada e compartilhamento de tela. Nenhuma otimização antes disso.
2. **Ciclo de vida.** Coordenador mínimo com os quatro estados, ligado à visibilidade da janela e da chamada. Suspender o que está invisível.
3. **Caminhos quentes.** Só o que a fase 1 provar ser caro.
4. **Mídia adaptativa.** Reduzir resolução e FPS sob sobrecarga confirmada, nunca abaixo de 480p/30, voltando quando houver folga.

## 5. Pequenos itens (podem entrar a qualquer momento)

- Som de notificação de mensagem, com opção de desligar.
- Copiar texto com fallback e colar imagem no chat.
- Perfil de aparência com temas escuros (conferir com o sistema de temas atual do Briks antes).
- Captura DXGI para jogos em tela cheia: avaliar junto com a fase 3, já que atende o mesmo público.

## 6. Como integrar sem quebrar o Briks

- Trabalhar em branches curtas e específicas, uma por fase.
- Usar a branch da origem só como referência de leitura. Os cerca de 57 arquivos em comum mudaram nos dois lados, então um merge direto seria uma resolução manual grande e arriscada.
- Manter créditos e licença: o código é AGPL-3.0, igual ao Briks. Qualquer trecho copiado mantém o aviso de origem.
- Cada fase termina com testes, verificação do pacote (hash, interface renderizando) e só então uma versão publicada, com a sua autorização.

## 7. Próximo passo sugerido

Começar pela fase 1 do Modo Jogo (protocolo e validação, sem UI) em paralelo com a fase 1 do desempenho (medição). Nenhuma das duas muda o que o usuário vê.
