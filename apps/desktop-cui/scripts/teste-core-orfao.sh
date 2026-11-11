#!/usr/bin/env bash
# Prova de que o briks-core não sobrevive ao app: abre o Briks numa janela de teste na 2ª tela (sem foco), espera o core subir,
# mata o app À FORÇA e confere que o core sumiu sozinho. Não captura tela nenhuma. Uso: bash scripts/teste-core-orfao.sh
set -u
cd "$(dirname "$0")/.."
BUILD="$(cygpath -w "$PWD/build")"
count() { powershell -NoProfile -Command "(Get-Process $1 -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$BUILD*' } | Measure-Object).Count" | tr -d '\r'; }
killall() { powershell -NoProfile -Command "Get-Process briks-core,briks_cui -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$BUILD*' } | Stop-Process -Force"; }
trap killall EXIT
killall
BRIKS_TEST_WINDOW="${BRIKS_TEST_WINDOW:-3540,100}" BRIKS_DEMO=call BRIKS_DEV_ROOM=orfao BRIKS_CORE_FAKE_AUDIO=1 BRIKS_CORE_DATA="$(cygpath -w "$(mktemp -d)")" ./build/briks_cui.exe &
for i in $(seq 1 40); do sleep 0.25; [ "$(count briks-core)" = "1" ] && break; done
echo "antes de matar o app: briks_cui=$(count briks_cui) briks-core=$(count briks-core)"
powershell -NoProfile -Command "Get-Process briks_cui -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '$BUILD*' } | Stop-Process -Force"
sleep 2
C=$(count briks-core)
echo "2 s depois de matar o app à força: briks_cui=$(count briks_cui) briks-core=$C"
[ "$C" = "0" ] && echo "OK: o core encerrou sozinho" || { echo "FALHA: core órfão"; exit 1; }
