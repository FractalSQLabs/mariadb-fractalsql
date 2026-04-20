# syntax=docker/dockerfile:1.7
#
# mariadb-fractalsql Factory build — zero-dependency posture.
#
# Produces ONE fractalsql.so per target architecture (driven by
# --platform). The UDF ABI is stable across MariaDB 10.6 / 10.11 /
# 11.4 LTS and 12.2 rolling, so a single .so works on every supported
# major — release packages declare mariadb-server generically rather
# than pinning a version.
#
# Build posture
#   LuaJIT is compiled from source as a PIC static archive and folded
#   into fractalsql.so. The resulting extension depends on glibc only:
#   no libluajit-5.1.so, no libstdc++, no dynamic C++ ABI.
#
# Output (collected in the final `export` stage, emitted to the host
# via `docker buildx --output type=local,dest=...`):
#   /fractalsql.so
#
# Usage:
#   DOCKER_BUILDKIT=1 docker buildx build \
#     --platform linux/amd64 \
#     --target export \
#     --output type=local,dest=./dist/amd64 \
#     -f docker/Dockerfile .
#
# build.sh drives this for amd64 and arm64 in turn.
#
# --------------------------------------------------------------------
# Important build-system notes:
#
#  * The distro libluajit-5.1-dev ships a non-PIC .a, which fails to
#    link into a .so on most toolchains. We build LuaJIT 2.1 ourselves
#    with -fPIC + BUILDMODE=static and stage it into /opt/luajit.
#
#  * We clone LuaJIT from the GitHub mirror, not luajit.org/git. The
#    latter serves dumb HTTP and doesn't advertise shallow-clone
#    capabilities, so `git clone --depth=1` fails with "dumb http
#    transport does not support shallow capabilities".
#
#  * -fvisibility=hidden is deliberately NOT set on the extension TU.
#    MariaDB's loader resolves fractal_search / fractalsql_edition /
#    fractalsql_version (plus their _init/_deinit triplets) via the
#    .dynsym of the opened .so; hiding them breaks CREATE FUNCTION.
#
#  * Post-link assertions (ldd / nm / size / exports) run in the
#    builder stage so the build fails fast if a dynamic luajit or
#    libstdc++ dependency sneaks back in, or if a UDF entry point
#    falls out of .dynsym.

# --------------------------------------------------------------------
# GLOBAL ARGs — declared before the first FROM so every stage below
# can re-import them with bare `ARG NAME` and pick up these defaults.
# ARGs declared INSIDE a stage are stage-scoped; later stages that
# re-import them pick up empty strings, which silently drops flags.
# --------------------------------------------------------------------
ARG LUAJIT_GIT_URL=https://github.com/LuaJIT/LuaJIT.git
ARG LUAJIT_GIT_REF=v2.1

# EXT_CFLAGS / EXT_LDFLAGS — the hardened flag set for fractalsql.so.
#
# -D_GLIBCXX_USE_CXX11_ABI=0: pre-C++11 std::string ABI; symbols never
#   reference __cxx11::basic_string even if the toolchain pulls
#   libstdc++ headers.
# -ffunction-sections / -fdata-sections: per-symbol section emission,
#   pairs with -Wl,--gc-sections to drop unused LuaJIT code paths.
# -fvisibility=default: the UDF entry points must live in .dynsym.
#   If any upstream CFLAGS injects -fvisibility=hidden later, the
#   last -fvisibility=* on the gcc command line wins and restores
#   visibility. We want this AFTER any other visibility flag — the
#   Dockerfile controls the invocation order.
# -static-libgcc / -static-libstdc++: link the C/C++ runtime helpers
#   statically; combined with --exclude-libs,ALL they stay invisible
#   in .dynsym.
# -Wl,--strip-all: drop .symtab. .dynsym survives, which is what the
#   MariaDB loader needs for UDF symbol resolution.
ARG EXT_CFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0 -ffunction-sections -fdata-sections -fvisibility=default"
ARG EXT_LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL"

# Ceiling on the resulting .so. Generous (3 MiB) — fractalsql.so on
# amd64 lands comfortably under 1 MiB. Bump only with a note in the
# commit message; unexpected growth usually means something got
# linked in that shouldn't have.
ARG SIZE_CEILING_BYTES=3145728

# --------------------------------------------------------------------
# Stage: PIC-built LuaJIT static archive.
# --------------------------------------------------------------------
FROM debian:bookworm-slim AS luajit-static
ARG LUAJIT_GIT_URL
ARG LUAJIT_GIT_REF
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth=1 --branch "${LUAJIT_GIT_REF}" \
        "${LUAJIT_GIT_URL}" /src/luajit
WORKDIR /src/luajit
# BUILDMODE=static avoids producing the .so we don't need; XCFLAGS=-fPIC
# makes the .a foldable into a downstream shared object. Explicit
# TARGET_ARCH= unset because LuaJIT's Makefile uses it as a gcc flag
# knob — any outer env value makes gcc treat it as a linker input.
RUN unset TARGET_ARCH \
 && make BUILDMODE=static XCFLAGS="-fPIC" PREFIX=/opt/luajit -j"$(nproc)" TARGET_ARCH= \
 && make install BUILDMODE=static PREFIX=/opt/luajit \
 && test -f /opt/luajit/lib/libluajit-5.1.a \
 && ! test -f /opt/luajit/lib/libluajit-5.1.so

# --------------------------------------------------------------------
# Stage: fractalsql.so builder. One stage covers all MariaDB majors —
# we build against libmariadb-dev headers from the debian/ubuntu repo;
# the UDF_INIT / UDF_ARGS / MYSQL_ERRMSG_SIZE definitions haven't
# changed across 10.6 / 10.11 / 11.4 / 12.2.
# --------------------------------------------------------------------
FROM debian:bookworm-slim AS builder
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        binutils \
        file \
        libmariadb-dev \
        libmariadb-dev-compat \
        pkg-config \
 && rm -rf /var/lib/apt/lists/*
COPY --from=luajit-static /opt/luajit /opt/luajit

WORKDIR /build
COPY src/                /build/src/
COPY include/            /build/include/
COPY docker/assert_so.sh /build/docker/assert_so.sh

# Re-import the global ARGs into this stage. Bare `ARG NAME` — no
# default — pulls in the value declared before the first FROM.
ARG EXT_CFLAGS
ARG EXT_LDFLAGS
ARG SIZE_CEILING_BYTES

RUN set -eux; \
    MARIADB_CFLAGS="$(mariadb_config --cflags 2>/dev/null \
                        || mysql_config --cflags 2>/dev/null \
                        || echo '-I/usr/include/mariadb')"; \
    echo "MARIADB_CFLAGS=${MARIADB_CFLAGS}"; \
    gcc -Wall -Wextra -O3 -fPIC \
        ${MARIADB_CFLAGS} \
        -I/opt/luajit/include/luajit-2.1 \
        -Iinclude \
        ${EXT_CFLAGS} \
        -shared -o /fractalsql.so \
        src/fractalsql.c \
        /opt/luajit/lib/libluajit-5.1.a \
        -lm -ldl -lpthread \
        ${EXT_LDFLAGS}; \
    sh /build/docker/assert_so.sh /fractalsql.so "${SIZE_CEILING_BYTES}"

# --------------------------------------------------------------------
# Export stage — scratch, so `buildx --output=local` drops just the
# .so file onto the host.
# --------------------------------------------------------------------
FROM scratch AS export
COPY --from=builder /fractalsql.so /fractalsql.so
