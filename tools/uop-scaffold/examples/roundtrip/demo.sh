#!/usr/bin/env bash
# Two-UoP round-trip demo: the full end-system path.
#
#   vectors.txt -> feeder_uop --TSS--> processor_uop --TSS--> feeder_uop
#         (file)    (FACE UoP, mock     (FACE UoP, dummy      (checks every
#                     stimulus src)     transform 3x+7)       value vs file)
#
# Steps: generate both UoPs -> apply user code -> regenerate (proves
# preservation) -> build with -Werror -> orchestrate the run:
# processor first (it listens), feeder second; the feeder publishes its
# vectors, collects the transformed results, verifies them against
# vectors.txt, and exits 0 only if every value matches.
#
#   ./demo.sh                    (all defaults)
#   BASE_PORT=5200 TIMEOUT=60 ./demo.sh
#
# TSS_ROOT defaults to the tss repo checkout that contains this tool.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SCAFFOLD=$(cd "$HERE/../.." && pwd)
TSS_ROOT=${TSS_ROOT:-$(cd "$SCAFFOLD/../.." && pwd)}
GEN=$HERE/gen
TIMEOUT=${TIMEOUT:-30}

pick_port() {
    # Find a base port with base and base+1 both free on 127.0.0.1.
    python3 - <<'EOF'
import socket
def free(p):
    s = socket.socket()
    s.settimeout(0.3)
    try:
        s.connect(("127.0.0.1", p))
        return False
    except OSError:
        return True
    finally:
        s.close()
for p in range(5150, 5350):
    if free(p) and free(p + 1):
        print(p)
        break
else:
    raise SystemExit("no free port pair found")
EOF
}

BASE_PORT=${BASE_PORT:-$(pick_port)}
echo "== base port: $BASE_PORT =="

rm -rf "$GEN"
mkdir -p "$GEN"

echo "== generate =="
cd "$SCAFFOLD"
python3 -m scaffold.gen_uop "$HERE/feeder_uop.yaml" --out "$GEN/feeder"
python3 -m scaffold.gen_uop "$HERE/processor_uop.yaml" --out "$GEN/processor"

echo "== apply user code =="
python3 "$HERE/apply_user_code.py" "$GEN/feeder" feeder_uop \
    "$HERE/user_code/feeder"
python3 "$HERE/apply_user_code.py" "$GEN/processor" processor_uop \
    "$HERE/user_code/processor"

echo "== regenerate (user code must survive) =="
python3 -m scaffold.gen_uop "$HERE/feeder_uop.yaml" --out "$GEN/feeder"
python3 -m scaffold.gen_uop "$HERE/processor_uop.yaml" --out "$GEN/processor"
grep -q "feeder_uop: published" "$GEN/feeder/feeder_uop.c" \
    || { echo "FAIL: feeder user code lost"; exit 1; }
grep -q "msg.value \* 3 + 7" "$GEN/processor/processor_uop.c" \
    || { echo "FAIL: processor user code lost"; exit 1; }
echo "user code preserved"

echo "== build =="
for uop in feeder processor; do
    if ! cmake -S "$GEN/$uop" -B "$GEN/$uop/build" -DTSS_ROOT="$TSS_ROOT" \
        > "$GEN/$uop/cmake-config.log" 2>&1; then
        echo "cmake configure failed for $uop:"
        cat "$GEN/$uop/cmake-config.log"
        exit 1
    fi
    cmake --build "$GEN/$uop/build" > "$GEN/$uop/build.log" 2>&1 \
        || { echo "build failed for $uop:"; tail -30 "$GEN/$uop/build.log"; exit 1; }
done
echo "builds clean (-Werror)"

echo "== run round-trip =="
RESULTS=$GEN/feeder_results.txt
rm -f "$RESULTS" "$GEN/processor.log" "$GEN/feeder.log"

# Processor first: its publishers listen, the feeder's subscribers dial.
"$GEN/processor/build/processor_uop" "$BASE_PORT" \
    > "$GEN/processor.log" 2>&1 &
PROC_PID=$!
for i in $(seq 1 100); do
    if ! kill -0 $PROC_PID 2>/dev/null; then
        echo "FAIL: processor exited early:"; cat "$GEN/processor.log"; exit 1
    fi
    grep -q "processor_uop: running" "$GEN/processor.log" 2>/dev/null && break
    sleep 0.1
done
grep -q "processor_uop: running" "$GEN/processor.log" \
    || { echo "FAIL: processor never came up:"; cat "$GEN/processor.log"; exit 1; }
echo "processor up (pid $PROC_PID)"

FEEDER_VECTORS="$HERE/vectors.txt" FEEDER_OUT="$RESULTS" \
    "$GEN/feeder/build/feeder_uop" "$BASE_PORT" \
    > "$GEN/feeder.log" 2>&1 &
FEED_PID=$!
echo "feeder up (pid $FEED_PID)"

# Wait for the feeder to finish its run (it exits itself after the last
# result); TIMEOUT bounds the whole thing.
FEED_STATUS="timeout"
for i in $(seq 1 $((TIMEOUT * 10))); do
    if ! kill -0 $FEED_PID 2>/dev/null; then
        wait $FEED_PID && FEED_STATUS=0 || FEED_STATUS=$?
        break
    fi
    sleep 0.1
done

kill -TERM $PROC_PID 2>/dev/null || true
wait $PROC_PID 2>/dev/null || true

echo "--- feeder log ---"; cat "$GEN/feeder.log"
echo "--- processor log ---"; cat "$GEN/processor.log"

echo "== verify =="
NVEC=$(grep -c -v '^[[:space:]]*\(#\|$\)' "$HERE/vectors.txt")
[ "$FEED_STATUS" = "0" ] || { echo "FAIL: feeder exited $FEED_STATUS"; exit 1; }
[ -f "$RESULTS" ] || { echo "FAIL: no results file"; exit 1; }
NRES=$(wc -l < "$RESULTS" | tr -d ' ')
[ "$NRES" = "$NVEC" ] || \
    { echo "FAIL: $NRES results, want $NVEC"; exit 1; }
if grep -q "MISMATCH" "$RESULTS"; then
    echo "FAIL: mismatches in results:"; grep "MISMATCH" "$RESULTS"; exit 1
fi
echo "--- results ---"; cat "$RESULTS"
echo "ROUNDTRIP PASS: $NRES/$NVEC vectors file -> feeder_uop -> TSS -> processor_uop -> TSS -> feeder_uop, all values match"
