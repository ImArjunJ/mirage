# mirage

turn any machine into an airplay, google cast, and miracast receiver.

## quick start

download the [latest release](https://github.com/ImArjunJ/mirage/releases/latest), unzip, run:

    ./mirage

your phone will see "mirage" as an available display. that's it.

## usage

    mirage                      run in foreground
    mirage --daemon             run in background
    mirage stop                 stop background instance
    mirage status               show current state

## options

    --name <name>       device name (default: mirage)
    --port <port>       airplay port (default: 7000)
    --no-airplay        disable airplay
    --no-cast           disable google cast
    --no-miracast       disable miracast
    --verbose           show more output
    --debug             show everything
    --config <file>     config file path
    --version           print version

## platform support

|                   | linux            | macos        | windows |
| ----------------- | ---------------- | ------------ | ------- |
| airplay mirroring | ✓                | soon         | soon    |
| airplay audio     | ✓                | soon         | soon    |
| google cast       | stub             | stub         | stub    |
| miracast          | stub             | stub         | stub    |
| hw video decode   | vaapi, nvdec     | videotoolbox | d3d11va |
| windowing         | wayland, x11     | cocoa        | win32   |
| audio output      | pulseaudio, alsa | coreaudio    | wasapi  |

## building from source

    cmake -B build
    cmake --build build -j$(nproc)

needs cmake 3.25+, c++23 compiler, openssl, ffmpeg, vulkan, shaderc

## license

mit
