#!/bin/sh
# Serve the HPPLite timeline demo and expose it with a Cloudflare quick tunnel.
#   ext/hpplite/examples/demo/run_demo.sh            # needs ext/hpplite/.env (HPPLITE_PRIVATE_KEY)
# Only the private key is taken from .env: the old HPPLITE_CONTRACT_ADDRESS there
# must not be used (the rollup comes from the factory via l1=hpp-sepolia).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ext=${HPPLITE_EXT:-$root/build_real/hpplite.dylib}
py=${PYTHON:-/opt/homebrew/bin/python3.13}
port=${PORT:-8787}
[ -f "$ext" ] || { echo "build the real-L1 tree first: cmake -S $root -B $root/build_real -DHPPLITE_USE_REAL_L1=ON -DHPPLITE_SQLITE_SRC=<amalgamation dir> && cmake --build $root/build_real"; exit 1; }
if [ -z "${HPPLITE_PRIVATE_KEY:-}" ]; then
    HPPLITE_PRIVATE_KEY=$(sed -n 's/^HPPLITE_PRIVATE_KEY=//p' "$root/.env" | head -1)
    export HPPLITE_PRIVATE_KEY
fi
[ -n "$HPPLITE_PRIVATE_KEY" ] || { echo "HPPLITE_PRIVATE_KEY not set"; exit 1; }
unset HPPLITE_CONTRACT_ADDRESS HPPLITE_FACTORY_ADDRESS HPPLITE_L1_RPC HPPLITE_DATA_DIR
cd "$here"
# the app loads a copy: a rebuild of build_real (development goes on beside a
# public demo) must not change the module under a running process
mkdir -p "$here/data"
cp "$ext" "$here/data/hpplite-module.dylib" && ext="$here/data/hpplite-module.dylib"
# the store (#164): the batch bytes go to a pin service here (free mode, data/pins), the
# chain gets one anchor per 30 minutes; the receipt key is the service's own, nothing valuable
pinport=${PIN_PORT:-8788}
mkdir -p "$here/data/pins"
[ -f "$here/data/pin_signer.key" ] || printf '0x%s\n' "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$here/data/pin_signer.key"
"$py" -I "$root/examples/pin_service/pin_service.py" --port "$pinport" --dir "$here/data/pins" --signer-key "$here/data/pin_signer.key" \
    --pay-to 0x1111111111111111111111111111111111111111 --network eip155:181228 >> "$here/data/pin.log" 2>&1 &
pin=$!
for i in $(seq 1 30); do curl -s -m 2 "http://127.0.0.1:$pinport/status" 2>/dev/null | grep -q signer && break; sleep 0.5; done
export HPPLITE_DEMO_STORE="http://127.0.0.1:$pinport"
echo "store: $HPPLITE_DEMO_STORE (log: data/pin.log)"
start_app() {
    "$py" app.py "$ext" "$port" >> "$here/data/app.log" 2>&1 &
    app=$!
    for i in $(seq 1 60); do
        kill -0 $app 2>/dev/null || { tail -20 "$here/data/app.log"; return 1; }
        curl -s -m 5 "http://localhost:$port/api/status" >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}
start_app || exit 1
echo "app: http://localhost:$port (log: data/app.log)"
echo "agents: $(curl -s -m 5 "http://localhost:$port/api/status" | sed -n 's/.*"writer": "\([^"]*\)".*/\1/p')"
# the tunnel in the background (the quick-tunnel API is occasionally slow: retried), the URL to stdout
( while :; do
    cloudflared tunnel --url "http://localhost:$port" --edge-ip-version 4 --protocol http2 2>&1 | grep --line-buffered -E "trycloudflare.com|error|failed"
    sleep 5
  done ) &
tunnel=$!
trap 'kill $tunnel $app 2>/dev/null; wait $app 2>/dev/null; kill $pin 2>/dev/null' EXIT INT TERM
# watchdog: the app once died without a word; a public demo must not stay dark
while :; do
    sleep 15
    if ! kill -0 $app 2>/dev/null || ! curl -s -m 10 "http://localhost:$port/api/status" >/dev/null 2>&1; then
        echo "$(date -u +%FT%TZ) app not answering; restarting (the lease is re-taken at open)" | tee -a "$here/data/app.log"
        # the app is usually already dead here: kill and wait then fail, and
        # with set -e either one silently ended the launcher instead of
        # restarting the app
        kill $app 2>/dev/null || true
        wait $app 2>/dev/null || true
        start_app || { echo "restart failed"; exit 1; }
    fi
done
