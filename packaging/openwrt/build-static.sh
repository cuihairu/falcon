#!/bin/sh
# Full-static musl build of falcon-daemon + falcon-cli inside an alpine
# container. The script locates the repo from its own path, so any mount
# point works:
#
#   docker run --rm -v "$PWD":/work alpine:3.22 \
#       sh /work/packaging/openwrt/build-static.sh
#
# Output: build-ipk/bin-<arch>/falcon-{daemon,cli} (stripped, statically
# linked against musl) — feed them to packaging/openwrt/build-ipk.sh.
# Optional env:
#   FALCON_APK_MIRROR  mirror prefix substituted for dl-cdn.alpinelinux.org
#                      (local setups; must not contain '#'; CI keeps the
#                      upstream default)
#
# Static-link mechanism (probe-validated 2026-11-18, alpine 3.22):
#   - CMAKE_EXE_LINKER_FLAGS_INIT "-static" via a top-level include set
#     before project().
#   - Command-line cache seeds force the find_package modules to resolve
#     static archives. CMAKE_FIND_LIBRARY_SUFFIXES is a plain variable
#     overwritten by platform init inside project(), so suffix settings
#     in toolchain/top-level files are defeated; pre-seeded cache entries
#     are authoritative (probe: all five libraries resolve .a only with
#     the seeds, .so otherwise).
#   - The link tail uses short-form -l flags which resolve the .a under
#     -static; full-path .so entries are what poisoned earlier attempts.
#   - expat needs a pre-created UNKNOWN IMPORTED target (see the comment
#     at the post-project include below).
# No spdlog: alpine has no static spdlog, falcon falls back to its basic
# logger (compile/link proven in the earlier attempt).
#
# alpine 3.22 is the version this recipe was validated on (full green build
# + RPC smoke); pin it — the c-ares static-archive layout differs on 3.20.
set -eux

ARCH=$(uname -m)
case "$ARCH" in
	x86_64|aarch64) ;;
	*) echo "unsupported arch: $ARCH (need x86_64 or aarch64)" >&2; exit 1 ;;
esac

REPO_DIR=$(cd "$(dirname "$0")/../.." && pwd)
OUT_DIR="$REPO_DIR/build-ipk"
LOG="$OUT_DIR/musl-build-$ARCH.log"
STAGING="$OUT_DIR/bin-$ARCH"
mkdir -p "$OUT_DIR"
: > "$LOG"

if [ -n "${FALCON_APK_MIRROR:-}" ]; then
	sed -i "s#https://dl-cdn.alpinelinux.org#${FALCON_APK_MIRROR}#g" /etc/apk/repositories
fi

apk add --no-cache \
	gcc g++ musl-dev cmake ninja pkgconf nlohmann-json file \
	curl curl-dev curl-static \
	brotli-static expat-static expat-dev openssl-dev openssl-libs-static \
	libssh2-dev libssh2-static nghttp2-static libidn2-static libunistring-static \
	libpsl-static sqlite-dev sqlite-static zlib-dev zlib-static \
	zstd-static c-ares c-ares-dev

# Proof that the c-ares archive is present. The package layout is
# version-specific: v3.20 splits libcares.a into a separate c-ares-static
# package (c-ares-dev carries only the .so symlink) while v3.22 ships it
# unsplit inside c-ares-dev (no c-ares-static package exists there). This
# recipe is pinned to 3.22; if re-pinned to 3.20, c-ares-static must be
# added to the list above or this gate will fail with the message below.
ls -la /usr/lib/libcares.a >> "$LOG" 2>&1 || {
	echo "FATAL: /usr/lib/libcares.a missing (c-ares-static not installed?)" >&2
	tail -30 "$LOG" >&2
	exit 1
}

# Tail of static libs appended to every link. Short-form -l resolves .a
# under -static; entries without a static archive are pruned here.
#
# Two filename/token mismatches to map explicitly:
#   - c-ares is installed as /usr/lib/libcares.a while the link token is
#     ares (curl's asyn-ares.o references ares_* symbols).
#   - Under musl -static the driver builds a FIXED libc search list
#     (libc.a, libgcc.a, libm.a, libpthread.a); libcares.a is not on it
#     and its name does not match the token, so "-lares" cannot resolve
#     ("cannot find -lares"). The exact-name form -l:libcares.a bypasses
#     that list entirely.
TAIL=""
for name in curl ares ssh2 expat sqlite3 ssl crypto brotlidec brotlicommon \
	zstd psl unistring idn2 nghttp2 z pthread dl rt m; do
	case "$name" in
		ares) arc=/usr/lib/libcares.a; flag='-l:libcares.a' ;;
		*) arc="/usr/lib/lib${name}.a"; flag="-l$name" ;;
	esac
	if [ -f "$arc" ]; then
		TAIL="$TAIL $flag"
	else
		echo "tail: no static lib for $name ($arc), skipped" >> "$LOG"
	fi
done
echo "TAIL=$TAIL" >> "$LOG"

# Runners executed at two different points, because CMAKE_FIND_LIBRARY_SUFFIXES
# is a plain variable that platform init resets INSIDE project(). The top-level
# include is read before project() and is therefore defeated for suffixes; it
# must only carry the pre-project state plus a pointer at the post-project file.
#
# CMAKE_PROJECT_INCLUDE is read after the top-level project(), i.e. before any
# add_subdirectory(). Setting ".a" first there makes every post-project
# find_library prefer the static archive.
#
# Root cause of the expat poisoning (probed in-image 2026-11-18): alpine's
# expat-dev ships a CMake config package (/usr/lib/cmake/expat-2.8.1) whose
# only target is `expat::expat` SHARED IMPORTED at libexpat.so.1.13.1 — no
# static variant. third_party/libmetalink calls find_package(expat CONFIG
# QUIET) first, so config mode wins its `if(NOT TARGET expat::expat)` guard,
# the FindEXPAT module path never runs, and the -DEXPAT_LIBRARY seed is never
# consumed. Fix: pre-create expat::expat here as an UNKNOWN IMPORTED at the
# static archive. The guard then skips BOTH the config package and the module
# path (expat's own config file also guards target creation the same way, so
# no double definition), and libmetalink links libexpat.a.
cat > /tmp/static-post.cmake <<EOF
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.so")
if(NOT TARGET expat::expat)
	add_library(expat::expat UNKNOWN IMPORTED)
	set_target_properties(expat::expat PROPERTIES
		IMPORTED_LOCATION "/usr/lib/libexpat.a"
		INTERFACE_INCLUDE_DIRECTORIES "/usr/include")
endif()
EOF

cat > /tmp/static.cmake <<EOF
set(CMAKE_PROJECT_INCLUDE "/tmp/static-post.cmake")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_C_STANDARD_LIBRARIES "$TAIL")
set(CMAKE_CXX_STANDARD_LIBRARIES "$TAIL")
set(CMAKE_C_STANDARD_LIBRARIES_INIT "$TAIL")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "$TAIL")
EOF
cat /tmp/static.cmake /tmp/static-post.cmake >> "$LOG"

# pkg-config resolves through PkgConfig::LIBSSH2 (alpine ships no libssh2
# CMake config package, so the protocols CMakeLists falls back to it). Without
# static mode it reports /usr/lib/libssh2.so and poisons the same -static link.
export PKG_CONFIG_STATIC=1

cmake -S "$REPO_DIR" -B /tmp/build -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=/tmp/static.cmake \
	-DCURL_LIBRARY=/usr/lib/libcurl.a \
	-DOPENSSL_SSL_LIBRARY=/usr/lib/libssl.a \
	-DOPENSSL_CRYPTO_LIBRARY=/usr/lib/libcrypto.a \
	-DSQLite3_LIBRARY=/usr/lib/libsqlite3.a \
	-DEXPAT_LIBRARY=/usr/lib/libexpat.a \
	-DZLIB_LIBRARY=/usr/lib/libz.a \
	-DZLIB_INCLUDE_DIR=/usr/include \
	-DFALCON_USE_SYSTEM_LIBS=ON \
	-DFALCON_ENABLE_BITTORRENT=OFF \
	-DFALCON_ENABLE_NAT=OFF \
	-DFALCON_BUILD_SWARMD=OFF \
	-DFALCON_BUILD_DESKTOP=OFF \
	-DFALCON_BUILD_TESTS=OFF \
	-DFALCON_BUILD_EXAMPLES=OFF \
	>> "$LOG" 2>&1 || {
		echo "=== configure failed ===" >> "$LOG"
		tail -40 "$LOG" >&2
		exit 1
	}

echo "=== cache library resolution ===" >> "$LOG"
grep -E "_LIBRARY" /tmp/build/CMakeCache.txt \
	| grep -iE "curl|ssl|crypto|sqlite|expat|zlib" \
	>> "$LOG" 2>&1 || true

if ninja -C /tmp/build >> "$LOG" 2>&1; then
	echo "=== done ===" >> "$LOG"
else
	echo "=== build failed ===" >> "$LOG"
	tail -60 "$LOG" >&2
	exit 1
fi

echo "=== binaries ===" >> "$LOG"
file /tmp/build/bin/falcon-daemon /tmp/build/bin/falcon-cli >> "$LOG" 2>&1

strip /tmp/build/bin/falcon-daemon /tmp/build/bin/falcon-cli
ls -la /tmp/build/bin/falcon-daemon /tmp/build/bin/falcon-cli >> "$LOG"

echo "=== smoke: falcon-cli --version ===" >> "$LOG"
/tmp/build/bin/falcon-cli --version >> "$LOG" 2>&1 || {
	echo "=== cli --version failed ===" >> "$LOG"
	tail -30 "$LOG" >&2
	exit 1
}

echo "=== smoke: daemon aria2.getVersion ===" >> "$LOG"
cat > /tmp/d.json <<'EOF'
{ "rpc": { "enabled": true, "host": "127.0.0.1", "port": 16800 },
  "storage": { "task_db_path": "/tmp/tasks.db" } }
EOF
/tmp/build/bin/falcon-daemon --conf-path /tmp/d.json >> "$LOG" 2>&1 &
DPID=$!
# Retry loop: under qemu user-mode emulation first boot is slow; on native
# x86_64 the first attempt answers immediately.
SMOKE_OK=0
i=0
while [ "$i" -lt 30 ]; do
	if curl -sf --max-time 5 http://127.0.0.1:16800/jsonrpc \
		-H 'Content-Type: application/json' \
		-d '{"jsonrpc":"2.0","id":"q","method":"aria2.getVersion","params":[]}' \
		> /tmp/smoke.json 2>> "$LOG"; then
		SMOKE_OK=1
		break
	fi
	sleep 1
	i=$((i + 1))
done
echo "smoke attempts: $((i + 1))" >> "$LOG"
kill "$DPID" 2>/dev/null || true
if [ "$SMOKE_OK" != 1 ]; then
	echo "=== smoke failed: daemon RPC never answered ===" >> "$LOG"
	tail -40 "$LOG" >&2
	exit 1
fi
cat /tmp/smoke.json >> "$LOG"
echo >> "$LOG"

mkdir -p "$STAGING"
cp /tmp/build/bin/falcon-daemon /tmp/build/bin/falcon-cli "$STAGING/"
echo "=== staged to $STAGING ===" >> "$LOG"
