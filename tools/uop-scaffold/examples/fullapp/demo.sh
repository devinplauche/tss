#!/usr/bin/env bash
# Full-app demo: the end-system path with a front end and external storage.
#
#   vectors.txt -> feeder_uop --TSS--> frontend_uop --TSS--> feeder_uop -> check
#                                            |  ^
#                                   STORE_REQ | PUT / STORE_RESP
#                                            v  |
#                                     storage_uop -> app_state.txt
#
# The frontend holds live application state (mult/add) in RAM, loads it
# from the storage UoP on startup, mutates it through scripted GUI
# actions, and offloads it to the storage UoP on shutdown -- every one of
# those steps goes through a standard UoP interface over TSS. The feeder
# verifies each RESULT against the parameters the frontend echoes back.
#
# Address plan: one base port P; connection index i -> tcp://127.0.0.1:(P+i).
#   P+0 STIMULUS    feeder pub  / frontend sub
#   P+1 RESULT      frontend pub / feeder sub
#   P+2 STORE_REQ   frontend pub / storage sub   (storage runs base P+2)
#   P+3 STORE_RESP  storage pub / frontend sub
#   P+4 STORE_PUT   frontend pub / storage sub
#
#   ./demo.sh                    (all defaults)
#   BASE_PORT=5200 TIMEOUT=90 ./demo.sh
#
# TSS_ROOT defaults to the tss repo checkout that contains this tool.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SCAFFOLD=$(cd "$HERE/../.." && pwd)
TSS_ROOT=${TSS_ROOT:-$(cd "$SCAFFOLD/../.." && pwd)}
GEN=$HERE/gen
TIMEOUT=${TIMEOUT:-60}

pick_port() {
    # Find 5 consecutive free ports on 127.0.0.1 starting at/above 5150.
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
for base in range(5150, 5350):
    if all(free(base + i) for i in range(5)):
        print(base)
        break
else:
    raise SystemExit("no free 5-port block found")
EOF
}

BASE_PORT=${BASE_PORT:-$(pick_port)}
STORE_BASE=$((BASE_PORT + 2))
echo "== base port: $BASE_PORT (storage base: $STORE_BASE) =="

rm -rf "$GEN"
mkdir -p "$GEN"

echo "== generate =="
cd "$SCAFFOLD"
for uop in feeder frontend storage; do
    python3 -m scaffold.gen_uop "$HERE/${uop}_uop.yaml" --out "$GEN/$uop"
done

echo "== apply user code =="
python3 "$HERE/apply_user_code.py" "$GEN/feeder" feeder_uop \
    "$HERE/user_code/feeder"
python3 "$HERE/apply_user_code.py" "$GEN/frontend" frontend_uop \
    "$HERE/user_code/frontend"
python3 "$HERE/apply_user_code.py" "$GEN/storage" storage_uop \
    "$HERE/user_code/storage"

echo "== regenerate (user code must survive) =="
for uop in feeder frontend storage; do
    python3 -m scaffold.gen_uop "$HERE/${uop}_uop.yaml" --out "$GEN/$uop"
done
grep -q "feeder_uop: collected" "$GEN/feeder/feeder_uop.c" \
    || { echo "FAIL: feeder user code lost"; exit 1; }
grep -q "GUI action" "$GEN/frontend/frontend_uop.c" \
    || { echo "FAIL: frontend user code lost"; exit 1; }
grep -q "storage_uop: stored" "$GEN/storage/storage_uop.c" \
    || { echo "FAIL: storage user code lost"; exit 1; }
echo "user code preserved"

echo "== build =="
for uop in feeder frontend storage; do
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

echo "== run full app =="
RESULTS=$GEN/feeder_results.txt
STORE_FILE=$GEN/app_state.txt
FEEDER_MARKER=$GEN/feeder.shutdown.marker
FRONTEND_MARKER=$GEN/frontend.shutdown.marker
STORAGE_MARKER=$GEN/storage.shutdown.marker
rm -f "$RESULTS" "$STORE_FILE" \
    "$FEEDER_MARKER" "$FRONTEND_MARKER" "$STORAGE_MARKER" "$GEN"/*.log

# Seed external storage *before* the app starts: the frontend must load
# exactly these parameters (proves load-on-startup).
cp "$HERE/seed_state.txt" "$STORE_FILE"

wait_for_banner() { # pid logfile banner
    local pid=$1 log=$2 banner=$3
    for i in $(seq 1 100); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "FAIL: $log exited early:"; cat "$log"; exit 1
        fi
        grep -q "$banner" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "FAIL: banner '$banner' never appeared in $log:"; cat "$log"; exit 1
}

# Storage first: it only listens/dials on the storage addresses.
STORE_FILE="$STORE_FILE" STORAGE_MARKER="$STORAGE_MARKER" \
    "$GEN/storage/build/storage_uop" "$STORE_BASE" \
    > "$GEN/storage.log" 2>&1 &
STORE_PID=$!
wait_for_banner $STORE_PID "$GEN/storage.log" "storage_uop: running"
echo "storage up (pid $STORE_PID)"

# Frontend second: loads state from storage in its startup region.
FRONTEND_ACTIONS="$HERE/gui_actions.txt" FRONTEND_MARKER="$FRONTEND_MARKER" \
    "$GEN/frontend/build/frontend_uop" "$BASE_PORT" \
    > "$GEN/frontend.log" 2>&1 &
FRONT_PID=$!
wait_for_banner $FRONT_PID "$GEN/frontend.log" "frontend_uop: running"
echo "frontend up (pid $FRONT_PID)"

# Feeder last: its startup waits for the frontend's "ready" line, so no
# stimulus is transformed with uninitialized state.
FEEDER_VECTORS="$HERE/vectors.txt" FEEDER_OUT="$RESULTS" \
    FEEDER_MARKER="$FEEDER_MARKER" FRONTEND_LOG="$GEN/frontend.log" \
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

# Tear down front-to-back: the frontend's shutdown offloads state to the
# storage UoP, which must still be up to receive the PUT.
kill -TERM $FRONT_PID 2>/dev/null || true
wait $FRONT_PID 2>/dev/null || true
kill -TERM $STORE_PID 2>/dev/null || true
wait $STORE_PID 2>/dev/null || true

echo "--- feeder log ---"; cat "$GEN/feeder.log"
echo "--- frontend log ---"; cat "$GEN/frontend.log"
echo "--- storage log ---"; cat "$GEN/storage.log"

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
for m in "$FEEDER_MARKER" "$FRONTEND_MARKER" "$STORAGE_MARKER"; do
    [ -f "$m" ] || { echo "FAIL: missing shutdown marker $m"; exit 1; }
done
echo "shutdown regions ran in all three UoPs"
# Load proof: the frontend picked up the seeded parameters from storage.
grep -q "ready (mult=7 add=3, loaded from storage)" "$GEN/frontend.log" \
    || { echo "FAIL: frontend did not load seeded state"; exit 1; }
echo "load-on-startup proven (seed 7 3 picked up from storage)"
# Offload proof: after GUI actions (2 -> 5 1) and (4 -> 9 -3), the final
# shutdown offload must have stored "9 -3".
[ "$(tr -d ' \n' < "$STORE_FILE")" = "9-3" ] \
    || { echo "FAIL: storage content wrong:"; cat "$STORE_FILE"; exit 1; }
echo "offload-on-shutdown proven (storage now holds 9 -3)"
echo "--- results ---"; cat "$RESULTS"
echo "FULLAPP PASS: 5/5 vectors file -> feeder_uop -> TSS -> frontend_uop (+GUI actions, +storage load/offload) -> TSS -> feeder_uop, all values match"
