# mirage

local-network receiver.

mirage is a small receiver core with adapters for airplay, cast, and
miracast / wi-fi display. the main usable path today is airplay from ios.

## quick start

arch linux:

```sh
sudo pacman -S --needed base-devel git cmake ninja clang pkgconf openssl ffmpeg vulkan-headers vulkan-loader shaderc libx11 wayland libva
git clone https://github.com/ImArjunJ/mirage.git
cd mirage
CMAKE_GENERATOR=Ninja CC=clang CXX=clang++ ./scripts/install.sh
export PATH="$HOME/.local/bin:$PATH"
mirage doctor
mirage --diagnostics
```

ubuntu/debian:

```sh
sudo apt update
sudo apt install -y build-essential git cmake ninja-build clang pkg-config libssl-dev ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libvulkan-dev glslc libx11-dev libwayland-dev libva-dev
git clone https://github.com/ImArjunJ/mirage.git
cd mirage
CMAKE_GENERATOR=Ninja CC=clang CXX=clang++ ./scripts/install.sh
export PATH="$HOME/.local/bin:$PATH"
mirage doctor
mirage --diagnostics
```

windows from a zip:

```powershell
Expand-Archive .\mirage-*.zip -DestinationPath .\mirage-package
cd .\mirage-package\mirage-*
.\install.ps1 -AddToPath
mirage doctor
mirage --diagnostics
```

leave `mirage --diagnostics` running, open airplay on an iphone or ipad on the
same network, and choose `Mirage`. press ctrl+c after testing. if it works,
install the background service:

```sh
mirage service install
mirage service start
mirage service status
mirage service logs -f
```

on windows, run the service install commands from an elevated powershell.

## status

- airplay mirroring and audio work on the tested ios + linux path
- cast and miracast are experimental foundations, not complete media receivers
- windows builds in ci; runtime testing is early
- background service commands work on linux and windows
- protected content is not supported

full setup and troubleshooting: [docs/user-guide.md](docs/user-guide.md)

## common commands

```sh
mirage doctor
mirage
mirage --diagnostics
mirage paths
```

background service:

```sh
mirage service install
mirage service start
mirage service status
mirage service logs -f
mirage service stop
```

the service commands are the same on linux and windows. linux uses a per-user
systemd service. windows uses the service manager and needs an elevated
powershell for install or uninstall.

for background diagnostics, install the service with diagnostics enabled:

```sh
mirage service stop
mirage service install --diagnostics
mirage service start
mirage service logs -f
```

## config

mirage reads `~/.config/mirage/config.conf` on linux and
`%APPDATA%\mirage\config.conf` on windows.

```text
name = Mirage
airplay_port = 7000
enable_airplay = true
enable_cast = false
enable_miracast = false
hardware_decode = true
```

## test

```sh
ctest --preset dev
```

## license

MIT
