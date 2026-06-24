# user guide

mirage is usable today as a local-network airplay receiver for technical users.
the main tested path is ios to linux.

cast and miracast are present so the receiver core can grow in that direction,
but they should be treated as experimental for now.

## requirements

- c++23 compiler
- cmake 3.25+
- openssl
- ffmpeg libraries
- vulkan runtime
- `glslc`
- pulseaudio or pipewire-pulse for audio output on linux

on arch linux, the useful starting point is:

```sh
sudo pacman -S cmake ninja clang openssl ffmpeg vulkan-headers vulkan-loader shaderc
```

package names differ by distro.

## build

```sh
cmake --preset dev
cmake --build --preset dev
```

manual build:

```sh
cmake -B build
cmake --build build -j$(nproc)
```

install to your user prefix:

```sh
cmake --install build --prefix ~/.local
```

## first run

run the environment check:

```sh
./build/mirage doctor
```

start with diagnostics the first time:

```sh
./build/mirage --diagnostics
```

from an iphone or ipad on the same local network, open airplay and choose
`Mirage`. mirror the screen or play audio, then disconnect.

a healthy session prints summaries like:

```text
[diag] Audio stream summary: health=clean, decoded_packets=...
[diag] Video stream summary: health=clean, frames=...
```

after the first successful run, plain mode is enough:

```sh
./build/mirage
```

## config

default paths:

- linux: `~/.config/mirage/config.conf`
- windows: `%APPDATA%\mirage\config.conf`

command-line flags override the config file.

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

set `hardware_decode = false` if gpu decode causes startup, cuda, vaapi, or
rendering problems.

## daily use

```sh
./build/mirage --daemon
./build/mirage status -v
./build/mirage stop
./build/mirage paths
```

use a persistent identity key if you do not want clients to see a fresh receiver
identity after every rebuild:

```sh
./build/mirage --identity-key ~/.config/mirage/identity.key
```

## network notes

mirage is for the local network. the sender and receiver should be on the same
wifi or ethernet segment.

if the receiver does not appear:

- run `./build/mirage doctor`
- make sure multicast dns is allowed on udp 5353
- allow mirage through the local firewall
- avoid guest wifi networks that isolate devices
- try `--name <name>` if another receiver has the same name

airplay control starts on tcp 7000 by default, then negotiates dynamic media
ports. restrictive firewalls should allow the mirage process on the local
network, not only tcp 7000.

## troubleshooting

receiver appears but video does not open:

- run `./build/mirage --diagnostics`
- set `hardware_decode = false`
- confirm vulkan is installed and the gpu driver is working
- try `--debug` only when you need detailed logs

audio is missing:

- confirm pulseaudio or pipewire-pulse is running
- check the diagnostics audio summary
- make sure the sender is not muted

receiver does not appear:

- confirm both devices are on the same local network
- check firewall and multicast dns
- run `./build/mirage --no-mdns` only if another discovery service is handling ads

protected video from streaming services is not supported.

## reporting issues

include:

- operating system and version
- sender device and os version
- command used to start mirage
- output from `./build/mirage doctor`
- diagnostics summaries from `./build/mirage --diagnostics`
- whether mirroring, audio-only, cast, or miracast was being tested
