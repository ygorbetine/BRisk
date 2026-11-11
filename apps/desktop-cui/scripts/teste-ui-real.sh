#!/usr/bin/env bash
# Teste da interface com CLIQUES DE VERDADE (mouse injetado pelo modo roteiro do app, não ganchos): abrir o seletor, aba Janelas,
# escolher a janela de teste, ajustar a qualidade, transmitir, trocar a qualidade ao vivo, e reagir a redimensionar / minimizar /
# fechar a janela transmitida. Tudo na 2ª tela, sem foco; a única coisa capturada é a janela de teste deste roteiro.
# EMIT_ENV="BRIKS_DEV_SHARE=test" faz o emissor transmitir uma fonte SINTÉTICA (sem tocar em nenhuma tela ou janela real).
# Uso: bash scripts/teste-ui-real.sh <roteiro-do-emissor.txt> [pasta-de-saida] [roteiro-do-espectador.txt]   (roteiros em scripts/ui-real-*.txt)
set -u
cd "$(dirname "$0")/.."
SCRIPT_A="$(cygpath -w "$PWD/$1")"
OUT="${2:-$PWD/shots-ui-real}"; mkdir -p "$OUT"; rm -f "$OUT"/*.flag
BUILD="$(cygpath -w "$PWD/build")"
TMP="$(mktemp -d)"; mkdir -p "$TMP/A" "$TMP/B"
W() { cygpath -w "$1"; }
cleanup() {
  powershell -NoProfile -Command "Get-Process briks-core,briks_cui -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$BUILD*' } | Stop-Process -Force; Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { \$_.CommandLine -like '*janela-teste.ps1*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }"
}
trap cleanup EXIT
cleanup
# o CMake só copia o core quando religa a UI: garante que o app use o core recém-compilado
[ briks-core/target/release/briks-core.exe -nt build/briks-core.exe ] && cp briks-core/target/release/briks-core.exe build/briks-core.exe
./build/briks-core.exe mock-realtime 0 > "$TMP/mock.txt" 2>&1 &
sleep 1
PORT=$(grep -o '"port":[0-9]*' "$TMP/mock.txt" | grep -o '[0-9]*')
powershell -NoProfile -WindowStyle Hidden -File scripts/janela-teste.ps1 -X 3480 -Y 420 -Flags "$(W "$OUT")" &
sleep 2
export BRIKS_SHARE_NO_PRIMARY=1   # nunca transmite o monitor principal do usuário
export BRIKS_REALTIME_URL="ws://127.0.0.1:$PORT" BRIKS_REALTIME_KEY=teste BRIKS_CORE_FAKE_AUDIO=1 BRIKS_WINDOW=1280x800 BRIKS_DEV_ROOM=ui-real BRIKS_SCRIPT_SHOTS=1
# espectador: no modo Transmissão, tira uma foto a cada sinal do emissor
BRIKS_DEMO=call-broadcast BRIKS_CORE_DATA="$(W "$TMP/B")" BRIKS_TEST_WINDOW=3900,200 BRIKS_SCRIPT="$(W "$PWD/${3:-scripts/ui-real-espectador.txt}")" BRIKS_SCRIPT_OUT="$(W "$OUT")" timeout 120 ./build/briks_cui.exe &
PB=$!
sleep 1
env ${EMIT_ENV:-BRIKS_NOP=1} BRIKS_DEMO=call BRIKS_CORE_DATA="$(W "$TMP/A")" BRIKS_TEST_WINDOW=3440,0 BRIKS_SCRIPT="$SCRIPT_A" BRIKS_SCRIPT_OUT="$(W "$OUT")" timeout 120 ./build/briks_cui.exe
wait $PB 2>/dev/null
ls "$OUT" | tr '\n' ' '
