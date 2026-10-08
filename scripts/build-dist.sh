#!/usr/bin/env bash
# SomnoBridge — build a single merged, flashable image into dist/.
#
#   ./scripts/build-dist.sh              # -> dist/somnobridge-<version>-merged.bin
#   ./scripts/build-dist.sh --clean      # wipe build/ and reconfigure first
#
# The merged image contains bootloader + partition table + app with correct
# flash headers, so it flashes with one write at offset 0x0:
#
#   esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 dist/somnobridge-<version>-merged.bin
#
# Data partitions (nvs/phy/evidence/metadata) sit above the app, outside the
# merged span, so saved settings and BLE bonds survive a reflash. The guard
# below fails the build if the partition layout ever violates that.
set -euo pipefail

IMAGE="espressif/idf:${IDF_TAG:-v5.5.5}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)"
OUT="$ROOT/dist"
mkdir -p "$OUT"

args=(-v "$ROOT:/project" -w /project)
if [ -t 0 ]; then
    args+=(-it)
fi
# The GEEK re-enumerates between ttyACM0/ttyACM1 on every reset; forward
# every live serial node so esptool/monitor survive the flip.
for d in /dev/ttyACM[0-9]*; do
    [ -e "$d" ] && args+=(--device "$d")
done

docker run --rm "${args[@]}" "$IMAGE" bash -ec '
    cd /project
    if [ "$1" = "--clean" ]; then
        rm -rf build
        idf.py set-target esp32s3
    elif [ ! -f sdkconfig ]; then
        idf.py set-target esp32s3
    fi
    idf.py build
    # Guard: a merged image spans 0x0..app_end and pads inter-segment gaps
    # with 0xFF — any data partition inside that span would be erased by
    # every merged reflash.
    python3 /opt/esp/idf/components/partition_table/gen_esp32part.py \
        build/partition_table/partition-table.bin /tmp/part.csv
    python3 -c "
import csv, sys
def sz(v):
    v = v.strip()
    m = {\"K\": 1024, \"M\": 1048576, \"G\": 1073741824}.get(v[-1:], 1)
    return int(v[:-1] if m > 1 else v, 0) * m
rows = [r for r in csv.reader(open(\"/tmp/part.csv\")) if r and not r[0].startswith(\"#\")]
app_end = 0
data = []
for r in rows:
    if r[1] == \"app\":
        app_end = max(app_end, int(r[3], 0) + sz(r[4]))
    else:
        data.append((r[0], int(r[3], 0)))
bad = [n for n, o in data if o < app_end]
if bad:
    sys.exit(\"data partitions inside the merged-image span (erased by every merged reflash): \" + \", \".join(bad))
print(\"partition span check ok: all data partitions above app end\")
"
    idf.py merge-bin -o "/project/dist/somnobridge-$2-merged.bin"
' inner "${1:-}" "$VER"

echo
echo "Merged image: dist/somnobridge-$VER-merged.bin"
echo "Flash at offset 0x0:"
echo "  esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 dist/somnobridge-$VER-merged.bin"
