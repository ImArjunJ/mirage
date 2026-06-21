# mirage

mirage is an experimental cross-protocol local-network receiver. The target is
one polished receiver core for AirPlay, Google Cast, and Miracast/Wi-Fi Display,
with protocol adapters sharing discovery, session control, decode, render,
audio, diagnostics, and daemon plumbing.

The goal is a modern, high-quality receiver for Linux, Windows, macOS, and
headless systems: low latency, hardware decode, useful diagnostics, persistent
receiver identity, and clean integration with native audio/video sinks.

## status

This project is an early prototype, not a drop-in Apple TV, Chromecast, or
Miracast replacement yet. AirPlay is currently the most complete path because it
is driving the shared receiver foundation first; Cast and Miracast remain active
targets, not rejected scope.

| area | linux | macos | windows |
| --- | --- | --- | --- |
| airplay discovery | prototype | not implemented | partial |
| airplay rtsp / raop | experimental, audio-only baseline | not implemented | partial |
| airplay mirroring | experimental, working on tested iOS path | not implemented | partial |
| google cast | discovery + accept-loop stub | not implemented | partial stub |
| miracast / wfd | stub | not implemented | not implemented |
| hw video decode | vaapi / nvdec prototype, CPU copy-back | not implemented | planned d3d11va |
| rendering | vulkan + wayland/x11 | not implemented | win32 work in progress |
| audio output | pulseaudio / alsa | not implemented | wasapi work in progress |

Protected DRM/HDCP content is out of scope unless support is added through a
licensed route. mirage should interoperate with unprotected local streaming,
fail clearly when content is protected, and avoid depending on vendor private
keys, certificate extraction, or downgrade attacks.

## build

```sh
cmake -B build
cmake --build build -j$(nproc)
```

Runtime dependencies include a C++23 compiler, CMake 3.25+, OpenSSL, FFmpeg,
Vulkan, and `glslc`/shaderc.

## run

```sh
./build/mirage --debug
```

Useful options:

```text
--name <name>       device name (default: Mirage)
--port <port>       AirPlay RTSP port (default: 7000)
--no-airplay        disable AirPlay
--cast              enable experimental Google Cast stub
--miracast          enable experimental Miracast stub
--no-cast           disable Google Cast if enabled by config
--no-miracast       disable Miracast if enabled by config
--no-mdns           disable built-in mDNS broadcaster
--diagnostics       show compact session summaries
--verbose           show more output
--debug             show protocol events
--trace             show packet-level logs
--config <file>     config file path
--identity-key <file>
                    persistent receiver identity key
--daemon            run in background
status [-v]         show daemon state
stop                stop background instance
```

If `--config` is omitted, Mirage loads a per-user `mirage/config.conf` when it
exists (`$XDG_CONFIG_HOME/mirage/config.conf` or `~/.config/mirage/config.conf`
on Unix-like systems, `%APPDATA%\mirage\config.conf` on Windows). Command-line
flags override config file values.

Supported config keys use `key = value` syntax:

```text
name = Mirage
airplay_port = 7000
cast_port = 8009
miracast_port = 7236
enable_airplay = true
enable_cast = false
enable_miracast = false
hardware_decode = true
log_level = info
identity_key = /path/to/identity.key
```

Receiver identity is persistent by default. Mirage stores an Ed25519 identity
key under the per-user state directory unless `identity_key` or `--identity-key`
points somewhere else.

## test

```sh
ctest --test-dir build --output-on-failure
```

On Unix-like hosts, CTest also runs a local RAOP RTSP smoke test that starts
Mirage on a high port with mDNS disabled and verifies `ANNOUNCE`, `SETUP`, and
`TEARDOWN`.

For a longer local RAOP control-plane soak:

```sh
MIRAGE_SMOKE_ITERATIONS=50 ctest --test-dir build -R raop_smoke --output-on-failure
```

For real devices, prefer compact diagnostics first:

```sh
./build/mirage --diagnostics
```

Use `--debug` when diagnosing protocol setup, and `--trace` only when packet-level
timing, mDNS, RTP, or NTP logs are needed.

## real-device validation

For a useful AirPlay test run:

```sh
./build/mirage --debug --name Mirage
```

Then test these paths from an iPhone, iPad, or macOS sender on the same network:

1. Start Screen Mirroring, move around the device, play audio, then stop mirroring.
2. Start audio-only AirPlay from Music, Safari, Spotify, or another app, then stop playback.
3. Reconnect once more, then press `Ctrl-C`.

Healthy logs should include RTSP pairing, `SETUP type 110` for mirroring,
`SETUP type 96` or `RAOP ANNOUNCE`/`RAOP SETUP` for audio, `Audio: ... packets
decoded` during playback, and final `Audio stream summary` / `Video stream
summary` lines with `health=clean`. If an audio stream is negotiated but no
audio is playing, the summary may show `decoded_packets=0` with
`silent_or_marker` packets. `redundant_after_decode` packets are normal on some
iOS senders when gaps, resend requests, and invalid counts stay at zero.
`Ctrl-C` should exit cleanly without a segfault.

## roadmap

1. Make the AirPlay path reliable for unprotected mirroring and audio.
2. Replace ad hoc plist/protocol parsing with tested parsers and replay fixtures.
3. Add robust timing: jitter buffers, retransmit handling, drift correction,
   frame scheduling, and latency/drop metrics.
4. Move video toward zero-copy hardware decode and render paths.
5. Add real platform backends for macOS and complete Windows support.
6. Implement Google Cast using a receiver-app-compatible architecture instead
   of raw socket stubs.
7. Treat Miracast as a separate Wi-Fi Direct/WFD project after AirPlay is solid.

## reverse engineering policy

Reverse engineering is used for interoperability and compatibility testing.
Code imported from GPL projects such as UxPlay cannot be copied into this MIT
project unless the licensing strategy changes. Use those projects as behavioral
references, test peers, or optional external components.

## license

MIT
