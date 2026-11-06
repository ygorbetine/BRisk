#!/bin/sh
set -eu

require_value() {
  variable_name="$1"
  eval "variable_value=\${$variable_name:-}"
  if [ -z "$variable_value" ]; then
    echo "[briks-coturn] $variable_name não foi definido" >&2
    exit 1
  fi
}

require_number() {
  variable_name="$1"
  eval "variable_value=\${$variable_name:-}"
  case "$variable_value" in
    ''|*[!0-9]*)
      echo "[briks-coturn] $variable_name precisa ser um inteiro não negativo" >&2
      exit 1
      ;;
  esac
}

require_value TURN_REALM
require_value TURN_EXTERNAL_IP
require_number TURN_USER_QUOTA
require_number TURN_TOTAL_QUOTA
require_number TURN_MAX_BPS
require_number TURN_BPS_CAPACITY

case "$TURN_REALM" in
  *[!A-Za-z0-9.-]*|.*|*..*|*.)
    echo "[briks-coturn] TURN_REALM precisa ser um hostname DNS" >&2
    exit 1
    ;;
esac
case "$TURN_EXTERNAL_IP" in
  *[!0-9A-Fa-f:./]*)
    echo "[briks-coturn] TURN_EXTERNAL_IP precisa conter IP_PUBLICO ou IP_PUBLICO/IP_PRIVADO" >&2
    exit 1
    ;;
esac

turn_secret="$(tr -d '\r\n' < /run/secrets/turn_secret)"
if [ "${#turn_secret}" -lt 32 ]; then
  echo "[briks-coturn] o secret TURN precisa ter pelo menos 32 caracteres" >&2
  exit 1
fi
case "$turn_secret" in
  *[!A-Za-z0-9_-]*)
    echo "[briks-coturn] o secret TURN deve usar somente A-Z, a-z, 0-9, _ ou -" >&2
    exit 1
    ;;
esac

umask 077
runtime_config=/tmp/turnserver.runtime.conf
cp /etc/coturn/turnserver.conf "$runtime_config"
{
  printf '\nrealm=%s\n' "$TURN_REALM"
  printf 'static-auth-secret=%s\n' "$turn_secret"
  printf 'external-ip=%s\n' "$TURN_EXTERNAL_IP"
  printf 'cert=/etc/coturn/tls/fullchain.pem\n'
  printf 'pkey=/etc/coturn/tls/privkey.pem\n'
  printf 'user-quota=%s\n' "$TURN_USER_QUOTA"
  printf 'total-quota=%s\n' "$TURN_TOTAL_QUOTA"
  printf 'max-bps=%s\n' "$TURN_MAX_BPS"
  printf 'bps-capacity=%s\n' "$TURN_BPS_CAPACITY"
  printf 'proc-user=nobody\n'
  printf 'proc-group=nogroup\n'
} >> "$runtime_config"
unset turn_secret

exec turnserver -c "$runtime_config"
