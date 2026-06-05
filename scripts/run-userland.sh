#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: scripts/run-userland.sh [--no-build] [--build-dir DIR] [--] [COMMAND [ARG ...]]

Start an isolated newos userland session using the freestanding binaries.

Options:
  --no-build       Do not run "make freestanding" before starting the session.
  --build-dir DIR  Use DIR as the freestanding binary directory.
  -h, --help       Show this help.

With no COMMAND, an interactive newos sh is started. With COMMAND, the first
argument is used as a newos sh -c command string, and remaining arguments are
passed as positional parameters.
EOF
}

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TARGET_ARCH=${TARGET_ARCH:-$(uname -m)}
BUILD_DIR=${NEWOS_FREESTANDING_BUILD_DIR:-"$ROOT_DIR/build/freestanding-linux-$TARGET_ARCH"}
RUN_BUILD=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-build)
            RUN_BUILD=0
            shift
            ;;
        --build-dir)
            if [ "$#" -lt 2 ]; then
                echo "run-userland: --build-dir requires a directory" >&2
                exit 2
            fi
            BUILD_DIR=$2
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR=${1#--build-dir=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR=$ROOT_DIR/$BUILD_DIR ;;
esac

if [ "$RUN_BUILD" -ne 0 ]; then
    (cd "$ROOT_DIR" && make freestanding)
fi

for tool in env sh man; do
    if [ ! -x "$BUILD_DIR/$tool" ]; then
        echo "run-userland: missing executable $BUILD_DIR/$tool" >&2
        echo "run-userland: run make freestanding or pass --build-dir DIR" >&2
        exit 1
    fi
done

TERM_VALUE=${TERM:-xterm}

if [ "$#" -gt 0 ]; then
    command_text=$1
    shift
    exec "$BUILD_DIR/env" -i \
        PATH="$BUILD_DIR" \
        MANPATH="$ROOT_DIR/man" \
        SHELL="$BUILD_DIR/sh" \
        TERM="$TERM_VALUE" \
        NEWOS_USERLAND=1 \
        "$BUILD_DIR/sh" -c "$command_text" newos-userland "$@"
fi

exec "$BUILD_DIR/env" -i \
    PATH="$BUILD_DIR" \
    MANPATH="$ROOT_DIR/man" \
    SHELL="$BUILD_DIR/sh" \
    TERM="$TERM_VALUE" \
    NEWOS_USERLAND=1 \
    "$BUILD_DIR/sh"