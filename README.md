# mirage

local-network receiver.

one small receiver core, with adapters for airplay, cast, and miracast / wi-fi
display.

airplay is the useful path today. cast and miracast are present as early
adapters, not full media receivers yet.

## status

- airplay mirroring and audio work on the tested ios path
- cast has discovery, probe, tls control, and default media app state
- miracast / wfd has capability negotiation
- linux is the main runtime target
- windows builds in ci; runtime support is early
- macos, cast media streaming, and wfd media streaming are later
- protected content is not supported

## build

```sh
cmake -B build
cmake --build build -j$(nproc)
```

needs c++23, cmake 3.25+, openssl, ffmpeg, vulkan, and `glslc`.

## run

```sh
./build/mirage
```

common modes:

```sh
./build/mirage --diagnostics
./build/mirage --debug
./build/mirage --daemon
./build/mirage status -v
./build/mirage paths
./build/mirage stop
```

common options:

```text
--name <name>          receiver name, default Mirage
--port <port>          airplay port, default 7000
--no-mdns              disable built-in discovery
--identity-key <file>  persistent receiver identity
--cast                 enable experimental cast adapter
--miracast             enable experimental miracast adapter
```

## config

mirage loads a per-user config file when it exists:

- linux: `~/.config/mirage/config.conf`
- windows: `%APPDATA%\mirage\config.conf`

flags override the file.

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

## test

```sh
ctest --test-dir build --output-on-failure
```

real-device smoke:

```sh
./build/mirage --diagnostics
```

connect from airplay, mirror or play audio, disconnect, then reconnect once.
healthy sessions end with `health=clean` in the audio and video summaries.

## license

MIT
