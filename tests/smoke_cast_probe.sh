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

dump_failure() {
    local code=$?
    echo "cast probe smoke failed; logs from ${tmpdir}" >&2
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
printf "%s" "${response}" | grep -q '"status":"app_media_ready"'

python3 - "${port}" "${status_json}" <<'PY'
import pathlib
import socket
import ssl
import struct
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
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"control"',
        '"health":"clean"',
        '"reason":"connected"',
    )
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
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"app"',
        '"health":"clean"',
        '"reason":"default_media_running"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"STOP","requestId":4,"sessionId":"other-session"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"appId":"CC1AD845"' in response
    assert b'"sessionId":"default-media-session"' in response
    assert b'"requestId":4' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"SET_VOLUME","requestId":5,"volume":{"level":0.42,"muted":true}}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"level":0.42' in response
    assert b'"muted":true' in response
    assert b'"requestId":5' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"control"',
        '"health":"clean"',
        '"reason":"volume_updated:muted"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"GET_CAST_STATE","requestId":6}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"INVALID_REQUEST" in response
    assert b'"reason":"INVALID_COMMAND"' in response
    assert b'"requestId":6' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"control"',
        '"health":"attention"',
        '"reason":"invalid_request:INVALID_COMMAND"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"GET_STATUS","requestId":7}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"MEDIA_STATUS" in response
    assert b'"status":[]' in response
    assert b'"requestId":7' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"LOAD","requestId":8,'
            '"media":{"contentId":"https://example.test/song.mp3",'
            '"contentType":"audio/mpeg","duration":123.4,'
            '"metadata":{"title":"cast song","artist":"cast artist",'
            '"albumName":"cast album"}},'
            '"currentTime":4.5}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"MEDIA_STATUS" in response
    assert b'"mediaSessionId":1' in response
    assert b'"playerState":"PLAYING"' in response
    assert b'"title":"cast song"' in response
    assert b'"requestId":8' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"media":{"active":true',
        '"title":"cast song"',
        '"artist":"cast artist"',
        '"kind":"media"',
        '"health":"attention"',
        '"reason":"loaded_no_renderer:cast song"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"PLAY","requestId":9,"mediaSessionId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"MEDIA_STATUS" in response
    assert b'"playerState":"PLAYING"' in response
    assert b'"requestId":9' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"media"',
        '"health":"attention"',
        '"reason":"virtual_playback:PLAY"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"QUEUE_UPDATE","requestId":10,"mediaSessionId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"INVALID_REQUEST" in response
    assert b'"reason":"INVALID_COMMAND"' in response
    assert b'"requestId":10' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"media"',
        '"health":"attention"',
        '"reason":"invalid_request:INVALID_COMMAND"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"STOP","requestId":11,"mediaSessionId":999}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"INVALID_REQUEST" in response
    assert b'"reason":"INVALID_MEDIA_SESSION_ID"' in response
    assert b'"requestId":11' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"media"',
        '"health":"attention"',
        '"reason":"invalid_request:INVALID_MEDIA_SESSION_ID"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"STOP","requestId":12,"mediaSessionId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"MEDIA_STATUS" in response
    assert b'"status":[]' in response
    assert b'"requestId":12' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"kind":"control"',
        '"health":"clean"',
        '"reason":"media_stopped"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.media",
            '{"type":"STOP","requestId":13,"mediaSessionId":1}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"INVALID_REQUEST" in response
    assert b'"reason":"INVALID_MEDIA_SESSION_ID"' in response
    assert b'"requestId":13' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"STOP","requestId":14,"sessionId":"default-media-session"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"applications":[]' in response
    assert b'"requestId":14' in response
    wait_status_contains(
        '"protocol":"cast"',
        '"media":{"active":false',
        '"kind":"app"',
        '"health":"clean"',
        '"reason":"default_media_stopped"',
    )

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"STOP","requestId":15,"sessionId":"other-session"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"applications":[]' in response
    assert b'"requestId":15' in response

    sock.sendall(
        cast_message(
            "urn:x-cast:com.google.cast.receiver",
            '{"type":"STOP","requestId":16,"sessionId":"default-media-session"}',
        )
    )
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    response = recv_exact(sock, length)
    assert b"RECEIVER_STATUS" in response
    assert b'"applications":[]' in response
    assert b'"requestId":16' in response

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
grep -q "Cast app: default media receiver running" "${tmpdir}/err"
grep -q "Cast control: volume_updated=muted" "${tmpdir}/err"
grep -q "Cast control: invalid_request=INVALID_COMMAND" "${tmpdir}/err"
grep -q "Cast media: loaded metadata" "${tmpdir}/err"
grep -q "Cast media: virtual playback command=PLAY" "${tmpdir}/err"
grep -q "Cast media: invalid_request=INVALID_COMMAND" "${tmpdir}/err"
test -s "${tmpdir}/state/mirage/identity.key"
grep -Eq '^[A-Za-z0-9+/]{43}=$' "${tmpdir}/state/mirage/identity.key"
