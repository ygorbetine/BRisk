#!/usr/bin/env bash
# 1.4.3 (seletor de canal de audio): prova com o core REAL em loopback (mock-realtime local, sem rede, sem tocar em tela/janela de ninguem).
# O app entra numa sala vazia, transmite o padrao de teste (BRIKS_DEV_SHARE=test) e o roteiro abre o popover "Canal/Apps" da barra,
# escolhe "Escolher apps" e confere que o estado muda ao vivo. Uso: bash scripts/prova-canal-audio.sh [roteiro.txt] [pasta-de-saida]
set -u
cd "$(dirname "$0")/.."
OUT="${2:-$PWD/shots-canal-audio}"; mkdir -p "$OUT"
BUILD="$(cygpath -w "$PWD/build")"; TMP="$(mktemp -d)"; mkdir -p "$TMP/A"; W() { cygpath -w "$1"; }
cleanup() { powershell -NoProfile -Command "Get-Process briks-core,briks_cui -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$BUILD*' } | Stop-Process -Force"; }
trap cleanup EXIT; cleanup
[ briks-core/target/release/briks-core.exe -nt build/briks-core.exe ] && cp briks-core/target/release/briks-core.exe build/briks-core.exe
./build/briks-core.exe mock-realtime 0 > "$TMP/mock.txt" 2>&1 &
sleep 1
PORT=$(grep -o '"port":[0-9]*' "$TMP/mock.txt" | grep -o '[0-9]*')
export BRIKS_SHARE_NO_PRIMARY=1 BRIKS_REALTIME_URL="ws://127.0.0.1:$PORT" BRIKS_REALTIME_KEY=teste BRIKS_CORE_FAKE_AUDIO=1 BRIKS_WINDOW=1280x800 BRIKS_DEV_ROOM=canal-audio BRIKS_SCRIPT_SHOTS=1
BRIKS_DEMO=call BRIKS_HIDDEN=1 BRIKS_DEV_SHARE=test BRIKS_CORE_DATA="$(W "$TMP/A")" BRIKS_SCRIPT="$(W "$PWD/${1:-scripts/canal-audio.txt}")" BRIKS_SCRIPT_OUT="$(W "$OUT")" timeout 90 ./build/briks_cui.exe
echo "saida: $?"; ls "$OUT" | tr '\n' ' '
