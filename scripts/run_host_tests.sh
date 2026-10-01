#!/usr/bin/env bash
# SomnoPal — host contract test runner (spec §14.1)
# Compiles pure-logic components with stub ESP headers and runs the suite.
set -euo pipefail
cd "$(dirname "$0")/.."

INC="-Iscripts/test_include"
INC="$INC -Icomponents/ezshare/include"
INC="$INC -Icomponents/catalog/include"
INC="$INC -Icomponents/scheduler/include"
INC="$INC -Icomponents/ble_api/include"
INC="$INC -Icomponents/drift/include"
INC="$INC -Icomponents/journal/include"

SRCS="components/ezshare/ezs_parse.c"
SRCS="$SRCS components/ezshare/ezs_path.c"
SRCS="$SRCS components/catalog/catalog.c"
SRCS="$SRCS components/scheduler/therapy.c"
SRCS="$SRCS components/scheduler/phase.c"
SRCS="$SRCS components/ble_api/ble_proto.c"
SRCS="$SRCS components/drift/drift.c"
SRCS="$SRCS components/drift/edf_hdr.c"
SRCS="$SRCS components/journal/journal.c"

OUT="${TMPDIR:-/tmp}/somnopal-host-test"
cc -std=c11 -Wall -Wextra -Werror -g -O0 \
    $INC $SRCS scripts/host_test.c -o "$OUT"
"$OUT"
