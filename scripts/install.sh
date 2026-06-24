#!/usr/bin/env sh
set -eu

prefix="${MIRAGE_PREFIX:-$HOME/.local}"
build_dir="build-release"
config="Release"
skip_build=0

usage() {
    cat <<'EOF'
usage: scripts/install.sh [options]

builds mirage and installs it to a user prefix.

options:
  --prefix <dir>      install prefix, default ~/.local
  --build-dir <dir>   build directory, default build-release
  --debug             build debug instead of release
  --no-build          only run cmake --install
  -h, --help          show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            if [ "$#" -lt 2 ]; then
                echo "missing value for --prefix" >&2
                exit 2
            fi
            prefix="$2"
            shift 2
            ;;
        --build-dir)
            if [ "$#" -lt 2 ]; then
                echo "missing value for --build-dir" >&2
                exit 2
            fi
            build_dir="$2"
            shift 2
            ;;
        --debug)
            config="Debug"
            build_dir="build"
            shift
            ;;
        --no-build)
            skip_build=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ ! -f "$source_dir/CMakeLists.txt" ]; then
    echo "could not find CMakeLists.txt next to scripts/" >&2
    exit 1
fi

if [ "$skip_build" -eq 0 ]; then
    cmake -S "$source_dir" -B "$source_dir/$build_dir" \
        -DCMAKE_BUILD_TYPE="$config" \
        -DBUILD_TESTING=OFF
    cmake --build "$source_dir/$build_dir" --config "$config" --target mirage --parallel
fi

cmake --install "$source_dir/$build_dir" --config "$config" --prefix "$prefix"

installed="$prefix/bin/mirage"
if [ ! -x "$installed" ]; then
    echo "installed binary missing: $installed" >&2
    exit 1
fi

"$installed" --version >/dev/null

echo "installed mirage to $installed"
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *)
        echo "add this to your shell profile if mirage is not on your path:"
        echo "  export PATH=\"$prefix/bin:\$PATH\""
        ;;
esac
echo "run:"
echo "  mirage doctor"
echo "  mirage --diagnostics"
