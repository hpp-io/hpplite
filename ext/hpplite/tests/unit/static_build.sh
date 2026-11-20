#!/bin/sh
# The static build (#269): tools/linux/static/build.sh leaves hpplite-sequencer and hpplite-verify with no
# dynamic dependencies (the Dockerfile refuses a binary ldd can read), and the one binary runs in an Alpine
# and a Debian image alike: in each, on l1=file://, it prints its 'synced height' line (#264). And its HTTPS
# clients find the certificates to trust (#377): compiled on Alpine, libcurl looks for /etc/ssl/cert.pem, which a
# Debian or an Ubuntu does not have. In a Debian image with the distribution's bundle at its usual path and no
# HPPLITE_CA_BUNDLE, the sequencer's RPC calls reach a TLS server whose certificate is in that bundle; with no
# bundle anywhere it says so, and the remedy. Exit 77 when docker (or podman) is missing.
#   tests/unit/static_build.sh <path/to/tools/linux/static/build.sh>
set -u
script=$1
docker=$(command -v docker || command -v podman) || { echo "static_build: docker or podman not found; skipped"; exit 77; }
# under the working directory, not the system temp: the container runtime (colima, Docker Desktop) shares the home
# directory with its VM, and bind mounts from elsewhere arrive empty
d=$(mktemp -d "$(pwd)/hpplite-static-test.XXXXXX")
trap 'rm -rf "$d"' EXIT
fail() { echo "static_build: FAIL: $1"; exit 1; }
sh "$script" "$d/out" >"$d/build.log" 2>&1 || { tail -40 "$d/build.log"; fail "the build did not produce the binaries"; }
[ -x "$d/out/hpplite-sequencer" ] && [ -x "$d/out/hpplite-verify" ] || fail "binaries missing in $d/out"
n=0
for image in alpine:3.20 debian:bookworm-slim; do
    # pulled only when it is not here: a registry that does not answer must not fail a run that has the image (#278)
    "$docker" image inspect "$image" >/dev/null 2>&1 || pulled=$("$docker" pull -q "$image" 2>&1) || { echo "$pulled" | tail -3; fail "cannot pull $image"; }
    # a directory of its own per image, never one emptied from the host: on a virtiofs mount (colima) the guest
    # keeps a deleted file's name for about a second, and a create of it then fails (ENOENT; #277)
    n=$((n + 1)); w="$d/w$n"
    mkdir -p "$w"
    # a key, so the open takes the lease and the line comes; interval=0: nothing else happens
    out=$("$docker" run --rm -v "$d/out:/b:ro" -v "$w:/w" -e "HPPLITE_PARAMS=l1=file:///w/chain&interval=0&datadir=/w&privkey=0x0101010101010101010101010101010101010101010101010101010101010101" \
          "$image" sh -c 'timeout 20 /b/hpplite-sequencer /w/app.db & pid=$!; for i in $(seq 1 100); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done; kill -TERM $pid 2>/dev/null; wait $pid' 2>&1)
    echo "$out" | grep -q "synced height" || { echo "$out" | head -5; fail "the static sequencer did not come up in $image"; }
    echo "static_build: $image: ok"
done

# HTTPS with no HPPLITE_CA_BUNDLE (#377). The image has python for the TLS stand-in of an RPC (tls_stub.py), and
# Debian's bundle at /etc/ssl/certs/ca-certificates.crt; the stand-in's certificate is added to it, as a site's
# own CA is. $1: what is done to the image's bundles first; the sequencer's output is printed, and the stand-in's
# log of what it was asked is left in $2/asked.
image=python:3.12-slim-bookworm
"$docker" image inspect "$image" >/dev/null 2>&1 || pulled=$("$docker" pull -q "$image" 2>&1) || { echo "$pulled" | tail -3; fail "cannot pull $image"; }
command -v openssl >/dev/null || fail "openssl not found (the stand-in's certificate)"
mkdir -p "$d/cert"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$d/cert/server.key" -out "$d/cert/server.pem" \
    -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 -days 2 >/dev/null 2>&1 || fail "openssl could not make the certificate"
chmod 644 "$d/cert/server.key"
tests=$(cd "$(dirname "$0")" && pwd)
https_run() {
    mkdir -p "$2"
    "$docker" run --rm -v "$d/out:/b:ro" -v "$2:/w" -v "$d/cert:/c:ro" -v "$tests:/t:ro" \
        -e "HPPLITE_PARAMS=rpc=https://127.0.0.1:8545&contract=0x1111111111111111111111111111111111111111&interval=0&datadir=/w&privkey=0x0101010101010101010101010101010101010101010101010101010101010101" \
        "$image" sh -c "$1"'
            python3 -I /t/tls_stub.py --port 8545 --cert /c/server.pem --key /c/server.key --log /w/asked & stub=$!
            for i in $(seq 1 100); do python3 -I -c "import socket; socket.create_connection((\"127.0.0.1\", 8545), 1)" 2>/dev/null && break; sleep 0.1; done
            timeout 20 /b/hpplite-sequencer /w/app.db & pid=$!
            for i in $(seq 1 80); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
            kill -TERM $pid 2>/dev/null; wait $pid; kill $stub 2>/dev/null' 2>&1
}
line="the certificate of 127.0.0.1 could not be checked: no CA bundle found; set HPPLITE_CA_BUNDLE"
out=$(https_run 'cat /c/server.pem >> /etc/ssl/certs/ca-certificates.crt;' "$d/h1")
grep -q "^eth_" "$d/h1/asked" 2>/dev/null || { echo "$out" | head -8; fail "with the distribution's bundle at its usual path and no HPPLITE_CA_BUNDLE, the static sequencer's RPC calls should come through TLS"; }
echo "$out" | grep -q "could not be checked" && { echo "$out" | head -8; fail "a certificate in the distribution's bundle was not checked against it"; }
echo "static_build: https, the distribution's bundle: ok"
out=$(https_run 'rm -f /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem /etc/ssl/ca-bundle.pem;' "$d/h2")
[ -s "$d/h2/asked" ] && { cat "$d/h2/asked" | head -3; fail "with no bundle anywhere nothing should come through TLS"; }
echo "$out" | grep -q "^hpplite: .*$line" || { echo "$out" | head -8; fail "with no bundle anywhere the sequencer should say: $line"; }
echo "static_build: https, no bundle anywhere: ok"
echo "static_build: ok"
