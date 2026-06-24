# mirage

local-network receiver.

mirage is a small receiver core with adapters for airplay, cast, and
miracast / wi-fi display. the main usable path today is airplay from ios.

## quick start

download the latest release:

<https://github.com/ImArjunJ/mirage/releases/latest>

linux:

```sh
curl -LO https://github.com/ImArjunJ/mirage/releases/download/v0.1.0/mirage-0.1.0-Linux-x86_64.zip
unzip mirage-0.1.0-Linux-x86_64.zip
cd mirage-0.1.0-Linux-x86_64
./install.sh
export PATH="$HOME/.local/bin:$PATH"
mirage doctor
mirage --diagnostics
```

windows powershell:

```powershell
Invoke-WebRequest https://github.com/ImArjunJ/mirage/releases/download/v0.1.0/mirage-0.1.0-Windows-AMD64.zip -OutFile mirage.zip
Expand-Archive .\mirage.zip -DestinationPath .\mirage
cd .\mirage\mirage-0.1.0-Windows-AMD64
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
open a new terminal if `mirage` is not found after install.

## status

- airplay mirroring and audio work on the tested ios + linux path
- cast and miracast are experimental foundations, not complete media receivers
- windows builds in ci; runtime testing is early
- background service commands work on linux and windows
- protected content is not supported

full setup and troubleshooting: [docs/user-guide.md](docs/user-guide.md)

source builds are covered in the user guide.

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
