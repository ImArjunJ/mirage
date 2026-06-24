#!/usr/bin/env sh
set -eu

prefix="${MIRAGE_PREFIX:-$HOME/.local}"
install_service=0
start_service=0

usage() {
    cat <<'EOF'
usage: ./install.sh [options]

installs this mirage package to a user prefix.

options:
  --prefix <dir>      install prefix, default ~/.local
  --install-service   install the user service after copying files
  --start-service     install and start the user service
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
        --install-service)
            install_service=1
            shift
            ;;
        --start-service)
            install_service=1
            start_service=1
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

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -x "$package_dir/bin/mirage" ]; then
    echo "package binary missing: $package_dir/bin/mirage" >&2
    exit 1
fi

mkdir -p "$prefix/bin" "$prefix/share" "$prefix/share/doc"

cp "$package_dir/bin/mirage" "$prefix/bin/mirage"
chmod 755 "$prefix/bin/mirage"

if [ -d "$package_dir/share/mirage" ]; then
    rm -rf "$prefix/share/mirage"
    cp -R "$package_dir/share/mirage" "$prefix/share/mirage"
fi

if [ -d "$package_dir/share/doc/mirage" ]; then
    rm -rf "$prefix/share/doc/mirage"
    cp -R "$package_dir/share/doc/mirage" "$prefix/share/doc/mirage"
fi

"$prefix/bin/mirage" --version >/dev/null

if [ "$install_service" -eq 1 ]; then
    "$prefix/bin/mirage" service install
fi

if [ "$start_service" -eq 1 ]; then
    "$prefix/bin/mirage" service start
fi

echo "installed mirage to $prefix/bin/mirage"
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *)
        echo "add this to your shell profile if mirage is not on your path:"
        echo "  export PATH=\"$prefix/bin:\$PATH\""
        ;;
esac
