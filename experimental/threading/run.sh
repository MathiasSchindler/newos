#!/bin/sh
set -eu

DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$DIR"

make freestanding
exec ./build/threadbench "$@"