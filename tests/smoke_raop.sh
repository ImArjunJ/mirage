#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: smoke_raop.sh /path/to/mirage" >&2
    exit 2
fi

mirage_bin=$1
port=${MIRAGE_SMOKE_PORT:-}
if [[ -z "${port}" ]]; then
    port=$(python3 - <<'PY'
import socket

with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
fi
iterations=${MIRAGE_SMOKE_ITERATIONS:-1}
tmpdir=$(mktemp -d)
pid=

dump_failure() {
    local code=$?
    echo "raop smoke failed; logs from ${tmpdir}" >&2
    if [[ -f "${tmpdir}/out" ]]; then
        echo "--- stdout ---" >&2
        cat "${tmpdir}/out" >&2
    fi
    if [[ -f "${tmpdir}/err" ]]; then
        echo "--- stderr ---" >&2
        cat "${tmpdir}/err" >&2
    fi
    if [[ -f "${tmpdir}/state/mirage/status.json" ]]; then
        echo "--- status.json ---" >&2
        cat "${tmpdir}/state/mirage/status.json" >&2
        echo >&2
    fi
    return "${code}"
}

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -INT "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    rm -rf "${tmpdir}"
}
trap dump_failure ERR
trap cleanup EXIT

XDG_CONFIG_HOME="${tmpdir}/config" XDG_STATE_HOME="${tmpdir}/state" "${mirage_bin}" --no-mdns --diagnostics --port "${port}" >"${tmpdir}/out" 2>"${tmpdir}/err" &
pid=$!

status_json="${tmpdir}/state/mirage/status.json"
for _ in {1..30}; do
    if [[ -s "${status_json}" ]]; then
        break
    fi
    sleep 0.1
done
test -s "${status_json}"
grep -q '"id":"airplay"' "${status_json}"
grep -q '"state":"listening"' "${status_json}"
grep -q '"transport":"rtsp/raop"' "${status_json}"
grep -q '"advertised":false' "${status_json}"

for ((i = 1; i <= iterations; ++i)); do
    exec 3<>"/dev/tcp/127.0.0.1/${port}"
    for _ in {1..30}; do
        if grep -q '"clients":\[{' "${status_json}" &&
            grep -q '"protocol":"airplay"' "${status_json}" &&
            grep -q '"address":"127.0.0.1"' "${status_json}"; then
            break
        fi
        sleep 0.1
    done
    grep -q '"clients":\[{' "${status_json}"
    grep -q '"protocol":"airplay"' "${status_json}"
    grep -q '"address":"127.0.0.1"' "${status_json}"

    sdp=$'v=0\r\no=iTunes 1 0 IN IP4 127.0.0.1\r\ns=Mirage Smoke\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless/44100/2\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n'
    printf "ANNOUNCE rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 1\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s" "${#sdp}" "${sdp}" >&3
    timeout 2 grep -m1 "RTSP/1.0 200 OK" <&3 >/dev/null

    printf "SETUP rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002\r\n\r\n" >&3
    timeout 2 grep -m1 "Transport: RTP/AVP/UDP" <&3 >/dev/null

    printf "POST /command RTSP/1.0\r\nCSeq: 3\r\nContent-Length: 0\r\n\r\n" >&3
    timeout 2 grep -m1 "RTSP/1.0 200 OK" <&3 >/dev/null

    printf "POST /feedback RTSP/1.0\r\nCSeq: 4\r\nContent-Length: 0\r\n\r\n" >&3
    timeout 2 grep -m1 "RTSP/1.0 200 OK" <&3 >/dev/null

    printf "POST /audioMode RTSP/1.0\r\nCSeq: 5\r\nContent-Length: 0\r\n\r\n" >&3
    timeout 2 grep -m1 "RTSP/1.0 200 OK" <&3 >/dev/null

    printf "TEARDOWN rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 6\r\n\r\n" >&3
    timeout 2 grep -m1 "RTSP/1.0 200 OK" <&3 >/dev/null
    exec 3<&-
    exec 3>&-
    for _ in {1..30}; do
        if grep -q '"clients":\[\]' "${status_json}"; then
            break
        fi
        sleep 0.1
    done
    grep -q '"clients":\[\]' "${status_json}"
done

kill -INT "${pid}"
wait "${pid}"
pid=

grep -q "RAOP audio setup: codec=ALAC" "${tmpdir}/err"
grep -Eq "Audio stream summary: health=clean, decoded_packets=[0-9]+, silent_or_marker=[0-9]+" "${tmpdir}/err"
test -s "${tmpdir}/state/mirage/identity.key"
grep -Eq '^[A-Za-z0-9+/]{43}=$' "${tmpdir}/state/mirage/identity.key"
if grep -Eq "Audio stream summary:.*pending=[0-9]+\\)" "${tmpdir}/err"; then
    echo "audio stream summary has an unmatched trailing parenthesis" >&2
    exit 1
fi
