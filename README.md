# mirage

mirage is a local-network receiver.

The long-term shape is one small receiver core with protocol adapters for
AirPlay, Google Cast, and Miracast / Wi-Fi Display. The core owns discovery,
sessions, media setup, decode, rendering, audio output, diagnostics, and daemon
state.

Right now, AirPlay is the useful path. Cast has discovery, probe, and basic
status replies. Miracast / WFD has capability negotiation. Cast and WFD media
streaming are not implemented yet.

## status

- AirPlay mirroring and audio are experimental, but usable on the tested iOS
  path.
- Linux is the main development target.
- Windows builds in CI and has early platform plumbing.
- macOS, Cast media streaming, and Miracast / WFD media streaming are later
  work.
- DRM / HDCP-protected content is not supported.

## build

```sh
cmake -B build
cmake --build build -j$(nproc)
```

You need a C++23 compiler, CMake 3.25+, OpenSSL, FFmpeg, Vulkan, and
`glslc`.

## run

```sh
./build/mirage
```

Useful modes:

```sh
./build/mirage --diagnostics
./build/mirage --debug
./build/mirage --daemon
./build/mirage status -v
./build/mirage paths
./build/mirage stop
```

Common options:

```text
--name <name>          receiver name
--port <port>          AirPlay port, default 7000
--no-mdns              disable built-in discovery
--identity-key <file>  persistent receiver identity
--cast                 enable experimental Cast probe adapter
--miracast             enable experimental Miracast capability listener
```

## config

Mirage loads a per-user config file when it exists:

- Linux: `~/.config/mirage/config.conf`
- Windows: `%APPDATA%\mirage\config.conf`

Command-line flags override the file.

```text
name = Mirage
airplay_port = 7000
enable_airplay = true
enable_cast = false
enable_miracast = false
hardware_decode = true
log_level = info
identity_key = /path/to/identity.key
```

## test

```sh
ctest --test-dir build --output-on-failure
```

For a quick real-device check:

```sh
./build/mirage --diagnostics
```

Connect from AirPlay, mirror or play audio, disconnect, then reconnect once.
Healthy sessions end with `health=clean` in the audio and video summaries.

## notes

Mirage is for interoperability with unprotected local streaming. It should fail
clearly when content is protected.

GPL receiver projects are useful behavioral references, but their code should
not be copied into this MIT project unless the licensing strategy changes.

## license

MIT
