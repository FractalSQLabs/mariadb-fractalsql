#!/usr/bin/env bash
#
# scripts/package.sh — mariadb-fractalsql packaging.
#
# Assumes ./build.sh ${ARCH} has produced:
#   dist/${ARCH}/fractalsql.so
#
# Emits one .deb and one .rpm per arch into dist/packages/:
#   dist/packages/mariadb-fractalsql-amd64.deb
#   dist/packages/mariadb-fractalsql-amd64.rpm
#   dist/packages/mariadb-fractalsql-arm64.deb
#   dist/packages/mariadb-fractalsql-arm64.rpm
#
# One binary covers MariaDB 10.6 / 10.11 / 11.4 LTS and 12.2 rolling —
# the UDF ABI is stable across those majors, so the package depends on
# mariadb-server generically rather than pinning a specific major.
#
# Usage:
#   scripts/package.sh [amd64|arm64]     # default: amd64

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="1.0.0"
ITERATION="1"
DIST_DIR="dist/packages"
PKG_NAME="mariadb-fractalsql"
mkdir -p "${DIST_DIR}"

# Absolute repo root, captured before any -C chdir'd fpm invocation.
REPO_ROOT="$(pwd)"
for f in LICENSE LICENSE-THIRD-PARTY; do
    if [ ! -f "${REPO_ROOT}/${f}" ]; then
        echo "missing ${REPO_ROOT}/${f} — refusing to package without it" >&2
        exit 1
    fi
done

PKG_ARCH="${1:-amd64}"
case "${PKG_ARCH}" in
    amd64|arm64) ;;
    *)
        echo "unknown arch '${PKG_ARCH}' — expected amd64 or arm64" >&2
        exit 2
        ;;
esac

case "${PKG_ARCH}" in
    amd64) RPM_ARCH="x86_64"  ;;
    arm64) RPM_ARCH="aarch64" ;;
esac

SO="dist/${PKG_ARCH}/fractalsql.so"
if [ ! -f "${SO}" ]; then
    echo "missing ${SO} — run ./build.sh ${PKG_ARCH} first" >&2
    exit 1
fi

DEB_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.deb"
RPM_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.rpm"

# Build a staging root that mirrors the on-disk layout so fpm can
# just tar it up.
#
# LICENSE ledger: staged into /usr/share/doc/<pkg>/ via install -Dm0644
# BEFORE running fpm. Explicit fpm src=dst mappings break here — fpm's
# -C chroots absolute source paths too, so ${REPO_ROOT}/LICENSE gets
# resolved as ${STAGE}${REPO_ROOT}/LICENSE and fpm bails with
# "Cannot chdir to ...".
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

install -Dm0755 "${SO}" \
    "${STAGE}/usr/lib/mysql/plugin/fractalsql.so"
install -Dm0644 sql/install_udf.sql \
    "${STAGE}/usr/share/${PKG_NAME}/install_udf.sql"
install -Dm0644 "${REPO_ROOT}/LICENSE" \
    "${STAGE}/usr/share/doc/${PKG_NAME}/LICENSE"
install -Dm0644 "${REPO_ROOT}/LICENSE-THIRD-PARTY" \
    "${STAGE}/usr/share/doc/${PKG_NAME}/LICENSE-THIRD-PARTY"

echo "------------------------------------------"
echo "Packaging ${PKG_NAME} (${PKG_ARCH})"
echo "------------------------------------------"

# LuaJIT is statically linked into fractalsql.so — no libluajit-5.1-2
# (Debian) or luajit (RPM) runtime dependency is declared.
fpm -s dir -t deb \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${PKG_ARCH}" \
    --iteration "${ITERATION}" \
    --description "FractalSQL: Stochastic Fractal Search UDF for MariaDB (10.6 / 10.11 / 11.4 LTS, 12.2 rolling)" \
    --license "MIT" \
    --depends "libc6 (>= 2.38)" \
    --depends "mariadb-server" \
    -C "${STAGE}" \
    -p "${DEB_OUT}" \
    usr

fpm -s dir -t rpm \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${RPM_ARCH}" \
    --iteration "${ITERATION}" \
    --description "FractalSQL: Stochastic Fractal Search UDF for MariaDB (10.6 / 10.11 / 11.4 LTS, 12.2 rolling)" \
    --license "MIT" \
    --depends "mariadb-server" \
    -C "${STAGE}" \
    -p "${RPM_OUT}" \
    usr

rm -rf "${STAGE}"
trap - EXIT

echo
echo "Done. Packages in ${DIST_DIR}:"
ls -l "${DIST_DIR}"
