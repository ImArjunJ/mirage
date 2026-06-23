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

status_json="${tmpdir}/state/mirage/status.json"
for _ in {1..30}; do
    if [[ -s "${status_json}" ]]; then
        break
    fi
    sleep 0.1
done
test -s "${status_json}"
grep -q '"id":"cast"' "${status_json}"
grep -q '"state":"listening"' "${status_json}"
grep -q '"transport":"cast-v2"' "${status_json}"
grep -q '"advertised":false' "${status_json}"
grep -q '"id":"airplay"' "${status_json}"
grep -q '"state":"disabled"' "${status_json}"

exec 3<>"/dev/tcp/127.0.0.1/${port}"
printf "GET /setup/eureka_info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" >&3
response=$(timeout 2 cat <&3 2>/dev/null || true)
exec 3<&-
exec 3>&-

printf "%s" "${response}" | grep -q "HTTP/1.1 200 OK"
printf "%s" "${response}" | grep -q '"receiver":"cast-v2"'
printf "%s" "${response}" | grep -q '"status":"control_ready"'

python3 - "${port}" <<'PY'
import socket
import ssl
import struct
import sys


def varint(value):
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def key(field, wire):
    return varint((field << 3) | wire)


def string_field(field, value):
    raw = value.encode()
    return key(field, 2) + varint(len(raw)) + raw


def varint_field(field, value):
    return key(field, 0) + varint(value)


def cast_message(namespace, payload):
    body = bytearray()
    body += varint_field(1, 0)
    body += string_field(2, "sender-0")
    body += string_field(3, "receiver-0")
    body += string_field(4, namespace)
    body += varint_field(5, 0)
    body += string_field(6, payload)
    return struct.pack(">I", len(body)) + body


def recv_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise RuntimeError("connection closed")
        data += chunk
    return bytes(data)


with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2) as sock:
    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"GET_STATUS","requestId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"requestId":1' in response
    assert b'"friendlyName":"Mirage"' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"GET_APP_AVAILABILITY","requestId":2,"appId":["CC1AD845"]}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"GET_APP_AVAILABILITY" in response
    assert b'"CC1AD845":"APP_AVAILABLE"' in response
    assert b'"requestId":2' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"LAUNCH","requestId":3,"appId":"CC1AD845"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"appId":"CC1AD845"' in response
    assert b'"transportId":"web-1"' in response
    assert b'"requestId":3' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"GET_STATUS","requestId":6}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"MEDIA_STATUS" in response
    assert b'"status":[]' in response
    assert b'"requestId":6' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"LOAD","requestId":7}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"LOAD_FAILED" in response
    assert b'"reason":"MEDIA_NOT_SUPPORTED"' in response
    assert b'"requestId":7' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"PLAY","requestId":8,"mediaSessionId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"INVALID_REQUEST" in response
    assert b'"reason":"INVALID_MEDIA_SESSION_ID"' in response
    assert b'"requestId":8' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"STOP","requestId":10,"sessionId":"default-media-session"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"applications":[]' in response
    assert b'"requestId":10' in response

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2) as raw:
    with context.wrap_socket(raw, server_hostname="Mirage") as sock:
        sock.sendall(
            cast_message(
                "urn:x-cast:com.google.cast.receiver",
                '{"type":"GET_STATUS","requestId":4}',
            )
        )
        length = struct.unpack(">I", recv_exact(sock, 4))[0]
        response = recv_exact(sock, length)
        assert b"RECEIVER_STATUS" in response
        assert b'"requestId":4' in response
        assert b'"friendlyName":"Mirage"' in response

        sock.sendall(
            cast_message(
                "urn:x-cast:com.google.cast.receiver",
                '{"type":"LAUNCH","requestId":5,"appId":"CC1AD845"}',
            )
        )
        length = struct.unpack(">I", recv_exact(sock, 4))[0]
        response = recv_exact(sock, length)
        assert b"RECEIVER_STATUS" in response
        assert b'"appId":"CC1AD845"' in response
        assert b'"transportId":"web-1"' in response
        assert b'"requestId":5' in response

        sock.sendall(
            cast_message(
                "urn:x-cast:com.google.cast.media",
                '{"type":"GET_STATUS","requestId":9}',
            )
        )
        length = struct.unpack(">I", recv_exact(sock, 4))[0]
        response = recv_exact(sock, length)
        assert b"MEDIA_STATUS" in response
        assert b'"status":[]' in response
        assert b'"requestId":9' in response
PY

kill -INT "${pid}"
wait "${pid}"
pid=

grep -q "Cast stream setup: mode=tls_control_status" "${tmpdir}/err"
test -s "${tmpdir}/state/mirage/identity.key"
grep -Eq '^[A-Za-z0-9+/]{43}=$' "${tmpdir}/state/mirage/identity.key"
