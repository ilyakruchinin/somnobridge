#!/usr/bin/env bash
# SomnoBridge — ESP-IDF toolchain runner (Docker).
# Wraps the official Espressif image so Docker is the only host dependency.
#
#   ./scripts/idf.sh build            # idf.py build
#   ./scripts/idf.sh flash monitor    # idf.py flash monitor (forwards IDF_PORT)
#   ./scripts/idf.sh exec <cmd>       # arbitrary command in the container
#   ./scripts/idf.sh                  # interactive shell
set -euo pipefail

IMAGE="espressif/idf:${IDF_TAG:-v5.5.1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

args=()
if [ -t 0 ]; then
    args+=(-it)
fi
# Forward the serial device only when it exists on the host.
PORT="${IDF_PORT:-/dev/ttyACM0}"
if [ -e "$PORT" ]; then
    args+=(--device "$PORT")
fi
if [ $# -eq 0 ]; then
    set -- /bin/bash
elif [ "$1" = "exec" ]; then
    shift
else
    set -- idf.py "$@"
fi
exec docker run --rm "${args[@]}" -v "${ROOT}:/project" -w /project "$IMAGE" "$@"
