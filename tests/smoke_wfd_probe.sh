#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: smoke_wfd_probe.sh /path/to/mirage" >&2
    exit 2
fi

mirage_bin=$1
port=${MIRAGE_WFD_SMOKE_PORT:-}
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

dump_failure() {
    local code=$?
    echo "wfd probe smoke failed; logs from ${tmpdir}" >&2
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

XDG_CONFIG_HOME="${tmpdir}/config" XDG_STATE_HOME="${tmpdir}/state" \
    "${mirage_bin}" --no-mdns --no-airplay --no-cast --miracast \
    --miracast-port "${port}" --diagnostics >"${tmpdir}/out" 2>"${tmpdir}/err" &
pid=$!

status_json="${tmpdir}/state/mirage/status.json"
for _ in {1..30}; do
    if [[ -s "${status_json}" ]]; then
        break
    fi
    sleep 0.1
done
test -s "${status_json}"
grep -q '"id":"miracast"' "${status_json}"
grep -q '"state":"listening"' "${status_json}"
grep -q '"transport":"wfd"' "${status_json}"
grep -q '"advertised":false' "${status_json}"

python3 - "${port}" "${status_json}" <<'PY'
import pathlib
import socket
import sys
import time


status_path = pathlib.Path(sys.argv[2])


def wait_status_contains(*needles):
    last = ""
    for _ in range(30):
        try:
            last = status_path.read_text()
        except FileNotFoundError:
            last = ""
        if all(needle in last for needle in needles):
            return
        time.sleep(0.1)
    raise AssertionError(f"status did not contain {needles}: {last}")


def recv_response(sock):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed before headers")
        data += chunk
    head, rest = bytes(data).split(b"\r\n\r\n", 1)
    length = 0
    for line in head.decode().split("\r\n")[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    while len(rest) < length:
        chunk = sock.recv(length - len(rest))
        if not chunk:
            raise RuntimeError("connection closed before body")
        rest += chunk
    return head.decode() + "\r\n\r\n" + rest[:length].decode()


with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2) as sock:
    wait_status_contains(
        '"protocol":"miracast"',
        '"kind":"control"',
        '"health":"clean"',
        '"reason":"connected"',
    )
    sock.sendall(
        b"OPTIONS rtsp://127.0.0.1/wfd1.0 RTSP/1.0\r\n"
        b"CSeq: 1\r\n\r\n"
    )
    response = recv_response(sock)
    assert "RTSP/1.0 200 OK" in response, response
    assert "Public: org.wfa.wfd1.0" in response, response

    body = b"wfd_audio_codecs\r\nwfd_content_protection\r\n"
    sock.sendall(
        b"GET_PARAMETER rtsp://127.0.0.1/wfd1.0 RTSP/1.0\r\n"
        b"CSeq: 2\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
    )
    response = recv_response(sock)
    assert "RTSP/1.0 200 OK" in response, response
    assert "wfd_audio_codecs:" in response, response
    assert "wfd_content_protection: none" in response, response
    assert "wfd_video_formats:" not in response, response

    body = b"wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19000 0 mode=play\r\n"
    sock.sendall(
        b"SET_PARAMETER rtsp://127.0.0.1/wfd1.0 RTSP/1.0\r\n"
        b"CSeq: 3\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
    )
    response = recv_response(sock)
    assert "RTSP/1.0 200 OK" in response, response
    wait_status_contains(
        '"protocol":"miracast"',
        '"kind":"control"',
        '"health":"clean"',
        '"reason":"rtp_ports_configured:19000"',
    )

    body = b"wfd_trigger_method: SETUP\r\n"
    sock.sendall(
        b"SET_PARAMETER rtsp://127.0.0.1/wfd1.0 RTSP/1.0\r\n"
        b"CSeq: 4\r\n"
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
    )
    response = recv_response(sock)
    assert "RTSP/1.0 200 OK" in response, response

    sock.sendall(
        b"SETUP rtsp://127.0.0.1/wfd1.0/streamid=0 RTSP/1.0\r\n"
        b"CSeq: 5\r\n\r\n"
    )
    response = recv_response(sock)
    assert "RTSP/1.0 501 Media Not Implemented" in response, response
    assert "wfd_error: media-not-implemented" in response, response
    assert "method: SETUP" in response, response
    assert "trigger: SETUP" in response, response
    wait_status_contains(
        '"protocol":"miracast"',
        '"kind":"media"',
        '"health":"attention"',
        '"reason":"media_not_implemented:SETUP"',
    )
PY

kill -INT "${pid}"
wait "${pid}"
pid=

grep -q "Miracast stream setup: mode=capability_listener" "${tmpdir}/err"
grep -q "Miracast control: client_rtp_port=19000" "${tmpdir}/err"
grep -q "Miracast control: trigger=SETUP accepted, media=unsupported" "${tmpdir}/err"
grep -q "Miracast stream summary: health=attention, reason=media_not_implemented, method=SETUP" "${tmpdir}/err"
