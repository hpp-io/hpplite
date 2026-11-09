# A fully static hpplite-sequencer and hpplite-verify (#269): built on Alpine (musl), every library linked in,
# so the one binary runs in Alpine and Debian images alike - the sidecar a platform copies into its users'
# images. The context is the staging directory tools/linux/static/build.sh makes: ext/hpplite's sources under
# ext/hpplite and the SQLite amalgamation under sqlite.
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base cmake pkgconf git autoconf automake libtool \
        curl-dev curl-static openssl-dev openssl-libs-static zlib-dev zlib-static \
        nghttp2-static brotli-static zstd-static libpsl-static libunistring-static libidn2-static c-ares-static
# libsecp256k1: Alpine ships no static archive; built from source, static only, the recovery module in
ARG SECP256K1_TAG=v0.6.0
RUN git clone -q --depth 1 --branch ${SECP256K1_TAG} https://github.com/bitcoin-core/secp256k1 /tmp/secp256k1 \
 && cd /tmp/secp256k1 && ./autogen.sh >/dev/null && ./configure -q --disable-shared --enable-static --disable-tests --disable-benchmark \
        --disable-exhaustive-tests --enable-module-recovery --enable-module-ecdh \
 && make -j"$(nproc)" >/dev/null && make install >/dev/null
COPY . /src
RUN cmake -S /src/ext/hpplite -B /build -DCMAKE_BUILD_TYPE=Release -DHPPLITE_SQLITE_SRC=/src/sqlite \
        -DHPPLITE_BUILD_TESTS=OFF -DHPPLITE_BUILD_EXTENSION=OFF -DHPPLITE_STATIC_TOOLS=ON >/dev/null \
 && cmake --build /build --target hpplite-sequencer hpplite-verify -j"$(nproc)" \
 && strip /build/hpplite-sequencer /build/hpplite-verify \
 && for b in hpplite-sequencer hpplite-verify; do if ldd /build/$b >/dev/null 2>&1; then echo "$b is not static:"; ldd /build/$b; exit 1; fi; done \
 && /build/hpplite-sequencer 2>&1 | head -1
FROM scratch
COPY --from=build /build/hpplite-sequencer /build/hpplite-verify /
