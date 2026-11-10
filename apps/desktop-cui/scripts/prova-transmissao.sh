#!/usr/bin/env bash
# Prova da transmissão de tela com o app de verdade, sem Supabase: sobe o Realtime simulado do briks-core e duas instâncias
# do Briks (cada uma com seu core e sua pasta de dados) na mesma sala. A transmite a Tela 1; B assiste no modo Transmissão.
# Salva capturas 1280x800 em $1 (padrão: ./shots-transmissao). As capturas mostram a sua tela real: não as publique sem olhar.
# Uso: bash scripts/prova-transmissao.sh [pasta-de-saida]
set -u
cd "$(dirname "$0")/.."
OUT="${1:-$PWD/shots-transmissao}"; mkdir -p "$OUT"
TMP="$(mktemp -d)"; mkdir -p "$TMP/A" "$TMP/B"
W() { cygpath -w "$1"; }
./build/briks-core.exe mock-realtime 0 > "$TMP/mock.txt" 2>&1 &
MOCK=$!
sleep 1
PORT=$(grep -o '"port":[0-9]*' "$TMP/mock.txt" | grep -o '[0-9]*')
export BRIKS_REALTIME_URL="ws://127.0.0.1:$PORT" BRIKS_REALTIME_KEY=teste BRIKS_CORE_FAKE_AUDIO=1 BRIKS_WINDOW=1280x800
shot() { # nome demo dados dev_share quadros sala
  BRIKS_DEMO="$2" BRIKS_CORE_DATA="$(W "$TMP/$3")" BRIKS_DEV_SHARE="$4" BRIKS_DEV_ROOM="$6" CUI_SCREENSHOT="$(W "$OUT")\\$1.png" CUI_SCREENSHOT_FRAMES="$5" timeout 60 ./build/briks_cui.exe
}
# 1) diálogo: telas, janelas, escolher apps, qualidade aberta com aviso de bitrate alto
shot dialogo-telas call A dialog 200 prova-d1
shot dialogo-janelas call A dialog-windows 200 prova-d2
shot dialogo-apps call A dialog-apps 320 prova-d3
shot dialogo-qualidade call A dialog-quality 200 prova-d4
# 2) A transmite a Tela 1 (prévia no próprio tile) e B assiste no modo Transmissão; C fica no modo Call (não exibe o vídeo)
BRIKS_DEV_NAME="Marina" shot tile-emissor call-broadcast A screen 1300 prova-t &
PA=$!
sleep 3
mkdir -p "$TMP/C"
shot call-sem-video call C "" 700 prova-t &
PC=$!
(sleep 6; powershell -NoProfile -File scripts/medir-transmissao.ps1 > "$OUT/medicao.txt" 2>&1) &
PM=$!
shot tile-receptor call-broadcast B "" 900 prova-t
wait $PM
wait $PA $PC
kill $MOCK 2>/dev/null
# o core fica vivo esperando outra UI quando o app fecha pela captura: encerra os que sobraram deste build
powershell -NoProfile -Command "Get-Process briks-core -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq '$(W "$PWD/build/briks-core.exe")' } | Stop-Process -Force"
ls -la "$OUT"
