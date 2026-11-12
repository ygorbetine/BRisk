# VP8 / VP9 no Briks nativo: viabilidade

**Histórico:** na 1.4.2 a análise concluiu que não era viável no ambiente de então. Na 1.4.4 o VP8/VP9 entrou na malha P2P (libvpx compilada por `scripts/build-vpx.ps1`; seção final abaixo). A análise original segue abaixo como contexto. **SFU e câmera permanecem H.264**; não foi possível provar mídia LiveKit com VP8/VP9 nesta máquina.

## Situação atual
- Todo o vídeo (transmissão de tela, câmera, malha P2P e SFU) é **H264**: codificador do Media Foundation (hardware NVENC/Quick Sync/AMF
  ou o MFT de software) e o `openh264` só como reserva. O decodificador é o MFT H264 da Microsoft (`mfenc.rs`).
- No SFU (LiveKit) a oferta de envio registra só Opus e H264 (`sfu.rs`): com todos os codecs padrão a oferta listava VP8 primeiro e o LiveKit
  registrava a faixa como VP8 mesmo com mídia H264.

## O que VP8/VP9 exigiria
1. **Codificador**: não há codificador VP8/VP9 no Media Foundation do Windows (só decodificador VP9, e só com a extensão da Microsoft Store).
   A opção real é a **libvpx** (C). Não existe codificador VP8/VP9 em Rust puro maduro.
2. **Decodificador** nos receptores: libvpx também (o MFT VP9 depende de extensão opcional do sistema).
3. **Negociação** de codec em P2P e SFU (listar VP8/VP9 só quando os dois lados suportam, queda para H264), pacote RTP (payload VP8/VP9),
   pedido de quadro-chave (PLI/FIR) e adaptação de taxa por codec.
4. Re-testar toda a matriz de qualidade (resolução x fps x bitrate) com o novo codec.

## Por que não dá agora
- O ambiente de build não tem o necessário para compilar a libvpx: sem `make`, sem `nasm`/`yasm` e sem `cmake` no PATH do shell (só o
  CMake embutido no Visual Studio, que a build do app já usa). Os crates `vpx-sys`/`env-libvpx-sys` esperam uma libvpx pronta no sistema
  (pkg-config) ou a compilam com `configure`+`make`, o que no Windows pede MSYS ou ferramentas que não estão instaladas.
- Trazer uma libvpx pré-compilada exigiria auditar a origem do binário (cadeia de suprimento) e atualizar `THIRD_PARTY_NOTICES`/licenças (BSD-3).
- O ganho esperado é pequeno para o produto hoje: o H264 por hardware já cobre transmissão de tela em 1080p60 com custo baixo de CPU; VP9 só
  compensaria em bitrate baixo (rede ruim), e aí o ajuste de bitrate/resolução já adapta.

## Caminho quando houver ambiente
- Instalar (ou vendorizar após auditoria) a libvpx e compilar com `cc`/`vpx-sys` atrás de uma *feature* do Cargo (`vpx`), desligada por padrão.
- Começar por **VP9 só como decodificador opcional** (receber de quem enviar VP9), depois o codificador, com H264 sempre como reserva.
- Negociar por capacidade anunciada na presença do par, nunca por suposição.

## 1.4.4: implementado (atualização)
O ambiente passou a ser viável sem instalar nada no sistema: `scripts/build-vpx.ps1` baixa libvpx 1.14.1, nasm 2.16.03 e GNU make 4.4.1 para um cache
local (`%LOCALAPPDATA%\briks-vpx-build`), confere SHA-256 fixados, compila a `libvpx.lib` estática (MSVC, /MD, sem LTCG) e a coloca em
`apps/desktop-cui/briks-core/vendor/vpx/` (fora do git). nasm e make são só ferramentas de build; só a libvpx (BSD-3) vai no binário.
- P2P (malha): VP8 e VP9 na transmissão de tela, escolha por transmissão; capacidades trocadas no `peer.state` (`vcodecs`); par sem o campo = só H264;
  se algum espectador não decodifica, a transmissão cai sozinha para H264 (troca da faixa por `replace_track`, sem renegociar).
- SFU (LiveKit): **continua só H264**. A faixa é publicada uma vez com codec fixo, o LiveKit não troca codec no meio da chamada e o emissor não
  sabe o que cada espectador decodifica; precisaria de troca de capacidades por data packets e republicar a faixa. Fica para depois.
- Câmera: continua H264. Seletor na UI (Qualidade da transmissão): H.264 (padrão), VP9, VP8.
- Build: `build-cui.ps1`/`build-installer.ps1` compilam com `--features vpx` (`-NoVpx` = só H264).
