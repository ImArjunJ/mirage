# mirage

local-network receiver.

mirage is a small receiver core with adapters for airplay, cast, and
miracast / wi-fi display.

## status

- airplay mirroring and audio work on the tested ios + linux path
- cast and miracast are experimental foundations, not complete media receivers
- windows builds in ci; runtime testing is early
- protected content is not supported

## install

from source:

```sh
./scripts/install.sh
mirage doctor
mirage --diagnostics
```

from a release zip:

```sh
unzip mirage-*.zip
cd mirage-*
./install.sh
mirage --diagnostics
```

then connect from airplay on the same local network.

full setup and troubleshooting: [docs/user-guide.md](docs/user-guide.md)

## common commands

```sh
mirage
mirage --diagnostics
mirage status -v
mirage paths
mirage stop
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
