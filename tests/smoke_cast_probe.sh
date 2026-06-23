#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: smoke_cast_probe.sh /path/to/mirage" >&2
    exit 2
fi

mirage_bin=$1
port=${MIRAGE_CAST_SMOKE_PORT:-}
if [[ -z "${port}" ]]; then
    port=$(python3 - <<'PY'
import socket

with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
fi
tmpdir=$(mktemp -d)
pid=

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -INT "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    rm -rf "${tmpdir}"
}
trap cleanup EXIT

XDG_CONFIG_HOME="${tmpdir}/config" XDG_STATE_HOME="${tmpdir}/state" \
    "${mirage_bin}" --no-mdns --no-airplay --cast --cast-port "${port}" --diagnostics \
    >"${tmpdir}/out" 2>"${tmpdir}/err" &
pid=$!
sleep 1

exec 3<>"/dev/tcp/127.0.0.1/${port}"
printf "GET /setup/eureka_info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" >&3
response=$(timeout 2 cat <&3 || true)
exec 3<&-
exec 3>&-

printf "%s" "${response}" | grep -q "HTTP/1.1 501 Not Implemented"
printf "%s" "${response}" | grep -q '"receiver":"cast-v2"'
printf "%s" "${response}" | grep -q '"status":"not_implemented"'

kill -INT "${pid}"
wait "${pid}"
pid=

grep -q "Cast stream setup: mode=probe_only" "${tmpdir}/err"
test -s "${tmpdir}/state/mirage/identity.key"
grep -Eq '^[A-Za-z0-9+/]{43}=$' "${tmpdir}/state/mirage/identity.key"
