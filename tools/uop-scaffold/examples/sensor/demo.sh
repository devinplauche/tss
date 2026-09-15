#!/usr/bin/env bash
# One-command demo: descriptor in, tested UoP out.
#
#   ./demo.sh [output-dir]        (default: ./gen)
#
# Steps: generate -> apply user code -> regenerate (proves preservation) ->
# build with -Werror -> ctest (generated harness tests generated UoP).
#
# TSS_ROOT defaults to the tss repo checkout that contains this tool.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SCAFFOLD=$(cd "$HERE/../.." && pwd)
TSS_ROOT=${TSS_ROOT:-$(cd "$SCAFFOLD/../.." && pwd)}
GEN=${1:-$HERE/gen}

echo "== generate =="
cd "$SCAFFOLD"
python3 -m scaffold.gen_uop "$HERE/sensor_uop.yaml" --out "$GEN"

echo "== apply user code =="
python3 "$HERE/apply_user_code.py" "$GEN" sensor_uop

echo "== regenerate (user code must survive) =="
python3 -m scaffold.gen_uop "$HERE/sensor_uop.yaml" --out "$GEN"
grep -q "next_track_id" "$GEN/sensor_uop.c" || { echo "FAIL: UoP user code lost"; exit 1; }
grep -q "send_RAW_DETECTION(tss" "$GEN/sensor_uop_harness.c" || { echo "FAIL: drive code lost"; exit 1; }
echo "user code preserved"

echo "== build =="
if ! cmake -S "$GEN" -B "$GEN/build" -DTSS_ROOT="$TSS_ROOT" > "$GEN/cmake-config.log" 2>&1; then
  echo "cmake configure failed:"
  cat "$GEN/cmake-config.log"
  exit 1
fi
cmake --build "$GEN/build"

echo "== test =="
(cd "$GEN/build" && ctest --output-on-failure)
