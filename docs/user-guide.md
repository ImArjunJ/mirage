# user guide

mirage is usable today as a local-network airplay receiver for technical users.
the main tested path is ios to linux.

cast and miracast are present so the receiver core can grow in that direction,
but they should be treated as experimental for now.

## runtime requirements

- openssl runtime libraries
- ffmpeg runtime libraries
- vulkan runtime
- pulseaudio or pipewire-pulse for audio output on linux

on arch linux, the useful runtime packages are:

```sh
sudo pacman -S openssl ffmpeg vulkan-loader
```

package names differ by distro.

## install from a package

download and extract a release zip for your platform.

linux:

```sh
unzip mirage-*.zip
cd mirage-*
./install.sh
mirage doctor
mirage --diagnostics
```

optional service install:

```sh
./install.sh --start-service
mirage service status
```

windows powershell:

```powershell
Expand-Archive .\mirage-*.zip
cd .\mirage-*
.\install.ps1 -AddToPath
mirage doctor
mirage --diagnostics
```

open a new terminal after using `-AddToPath`.

optional service install, from an elevated powershell:

```powershell
.\install.ps1 -AddToPath -InstallService -StartService
mirage service status
```

the package installer copies the binary, shaders, and docs into a user-writable
prefix. on linux the default is `~/.local`; on windows it is
`%LOCALAPPDATA%\Programs\mirage`.

## install from source

the source-tree installer builds release mode and installs to `~/.local`:

```sh
./scripts/install.sh
mirage doctor
mirage --diagnostics
```

use a custom prefix when needed:

```sh
./scripts/install.sh --prefix /opt/mirage
```

install and start the user service:

```sh
./scripts/install.sh --start-service
```

source builds need a c++23 compiler, cmake 3.25+, vulkan headers, and `glslc`.

on arch linux:

```sh
sudo pacman -S cmake ninja clang openssl ffmpeg vulkan-headers vulkan-loader shaderc
```

## developer build

```sh
cmake --preset dev
cmake --build --preset dev
```

plain cmake:

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
mirage doctor
```

start with diagnostics the first time:

```sh
mirage --diagnostics
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
mirage
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

foreground:

```sh
mirage
```

background service:

```sh
mirage service install
mirage service start
mirage service status
mirage service stop
mirage service uninstall
```

these commands are the same on linux and windows. linux uses a per-user systemd
service. windows uses the service manager and needs an elevated powershell for
install or uninstall.

other useful commands:

```sh
mirage paths
```

service install stores runtime options. use an explicit config path if you want
predictable service config:

```sh
mirage service install --config ~/.config/mirage/config.conf
```

```powershell
mirage service install --config C:\ProgramData\mirage\config.conf
```

use a persistent identity key if you do not want clients to see a fresh receiver
identity after every rebuild:

```sh
mirage --identity-key ~/.config/mirage/identity.key
```

advanced linux fallback, for machines without user systemd:

```sh
mirage --daemon
mirage status -v
mirage stop
```

## network notes

mirage is for the local network. the sender and receiver should be on the same
wifi or ethernet segment.

if the receiver does not appear:

- run `mirage doctor`
- make sure multicast dns is allowed on udp 5353
- allow mirage through the local firewall
- avoid guest wifi networks that isolate devices
- try `--name <name>` if another receiver has the same name

airplay control starts on tcp 7000 by default, then negotiates dynamic media
ports. restrictive firewalls should allow the mirage process on the local
network, not only tcp 7000.

## troubleshooting

receiver appears but video does not open:

- run `mirage --diagnostics`
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
- run `mirage --no-mdns` only if another discovery service is handling ads

protected video from streaming services is not supported.

## reporting issues

include:

- operating system and version
- sender device and os version
- command used to start mirage
- output from `mirage doctor`
- diagnostics summaries from `mirage --diagnostics`
- whether mirroring, audio-only, cast, or miracast was being tested
