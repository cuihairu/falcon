#!/bin/sh
# Builds the three OpenWrt .ipk packages for falcon (meshlet precedent):
#   falcon             musl binaries (falcon-daemon + falcon-cli) + procd init
#                      + UCI config, arch-specific
#   luci-app-falcon    LuCI controller, settings page and transfer panel
#                      (arch all)
#   app-meta-falcon    iStore app metadata (icon + app json), arch all
#
# Usage: build-ipk.sh <bin-dir> [out-dir] [--arch x86_64|aarch64] [--rel N]
#
# <bin-dir> must contain the release binaries falcon-daemon and falcon-cli
# (e.g. a FALCON_USE_SYSTEM_LIBS=ON musl build tree's bin directory). The
# package architecture is derived from the daemon ELF (`file`); --arch
# overrides it. The .ipk container is the tar.gz layout (debian-binary +
# control.tar.gz + data.tar.gz as a single tar), the same form opkg on
# iStoreOS 24.10 installs. data.tar.gz archives whole directories so the
# member set includes directory entries: opkg's unpacker does not create
# missing parent directories for file-only members.
set -eu

die() { printf 'build-ipk: %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC="$SCRIPT_DIR"
REPO_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)

BINDIR=""
OUT="."
ARCH_OVERRIDE=""
REL_OVERRIDE=""
while [ $# -gt 0 ]; do
	case "$1" in
		--rel)
			[ $# -ge 2 ] || die "--rel requires a value"
			REL_OVERRIDE="$2"
			shift 2
			;;
		--arch)
			[ $# -ge 2 ] || die "--arch requires a value (x86_64|aarch64)"
			ARCH_OVERRIDE="$2"
			shift 2
			;;
		--*)
			die "unknown option: $1"
			;;
		*)
			if [ -z "$BINDIR" ]; then
				BINDIR="$1"
			else
				OUT="$1"
			fi
			shift
			;;
	esac
done

[ -n "$BINDIR" ] || die "usage: build-ipk.sh <bin-dir> [out-dir] [--arch x86_64|aarch64] [--rel N]"
[ -d "$BINDIR" ] || die "bin dir not found: $BINDIR"
for b in falcon-daemon falcon-cli; do
	[ -f "$BINDIR/$b" ] || die "missing binary in $BINDIR: $b"
done
[ -f "$SRC/app-meta/falcon.json" ] || die "app-meta/falcon.json missing"
[ -f "$REPO_DIR/assets/falcon.png" ] || die "assets/falcon.png missing"

# Version single source of truth = project(VERSION) in falcon-cli's
# CMakeLists (same source the CLI binary reports).
VERSION=$(sed -n 's/^    VERSION \(.*\)$/\1/p' "$REPO_DIR/apps/cli/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || die "cannot read version from apps/cli/CMakeLists.txt"
REL="${VERSION}-1"
[ -n "$REL_OVERRIDE" ] && REL="${VERSION}-${REL_OVERRIDE}"

if [ -n "$ARCH_OVERRIDE" ]; then
	case "$ARCH_OVERRIDE" in
		x86_64 | aarch64) ARCH="$ARCH_OVERRIDE" ;;
		*) die "unsupported --arch: $ARCH_OVERRIDE (expected x86_64|aarch64)" ;;
	esac
else
	# `file` reports the ELF machine, mapped to opkg's architecture names.
	ARCH=$(file -b "$BINDIR/falcon-daemon" | sed -n 's/.*[, ]\(x86-64\|aarch64\).*/\1/p' | head -1)
	case "$ARCH" in
		x86-64) ARCH=x86_64 ;;
		aarch64) ARCH=aarch64 ;;
		*) die "unsupported binary architecture: $ARCH (pass --arch)" ;;
	esac
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# ---- falcon (arch-specific) -------------------------------------------------
R="$STAGE/falcon"
mkdir -p "$R/data/etc/init.d" "$R/data/etc/config" "$R/data/usr/bin" "$R/CONTROL"
cp "$SRC/etc/init.d/falcon" "$R/data/etc/init.d/"
cp "$SRC/etc/config/falcon" "$R/data/etc/config/"
cp "$BINDIR/falcon-daemon" "$BINDIR/falcon-cli" "$R/data/usr/bin/"
chmod 755 "$R/data/etc/init.d/falcon" "$R/data/usr/bin/falcon-daemon" "$R/data/usr/bin/falcon-cli"

# Drives SPI 插件：构建树 build/falcon/plugins 下的 MODULE so 一并入包；
# 静态构建（build-static.sh）没有这些产物时静默跳过。
PLUGIN_SRC="$BINDIR/../falcon/plugins"
if [ -d "$PLUGIN_SRC" ]; then
    mkdir -p "$R/data/usr/lib/falcon/plugins"
    cp "$PLUGIN_SRC"/*.so "$R/data/usr/lib/falcon/plugins/" 2>/dev/null || true
fi

cat > "$R/CONTROL/conffiles" <<'EOF'
/etc/config/falcon
/etc/init.d/falcon
EOF
cp "$SRC/CONTROL/postinst" "$R/CONTROL/postinst"
cp "$SRC/CONTROL/prerm" "$R/CONTROL/prerm"
SIZE=$(du -sk "$R/data" | cut -f1)
cat > "$R/CONTROL/control" <<EOF
Package: falcon
Version: $REL
Depends: libc
Source: https://github.com/cuihairu/falcon
SourceName: falcon
License: Apache-2.0
Section: net
SourceDateEpoch: 1794931200
Maintainer: cuihairu <chuihairu@gmail.com>
Architecture: $ARCH
Installed-Size: $SIZE
Description: Multi-protocol download engine: HTTP/FTP/SFTP/WebDAV/Metalink with an aria2-compatible JSON-RPC daemon. Enabled via UCI falcon.falcon.enabled; the procd init renders /var/etc/falcon-daemon.json on start.
 多协议下载引擎：HTTP/FTP/SFTP/WebDAV/Metalink，aria2 兼容 JSON-RPC 守护进程。经 UCI falcon.falcon.enabled 启用，procd init 启动时渲染 /var/etc/falcon-daemon.json。
EOF

chmod 755 "$R"/CONTROL/postinst "$R"/CONTROL/prerm
# --owner/--group 0: the builder's uid would otherwise ride inside the
# archives and surface as a nonexistent uid on the device (observed 1001
# from the CI runner on real hardware).
tar --owner=0 --group=0 -czf "$R/control.tar.gz" -C "$R/CONTROL" ./control ./postinst ./prerm ./conffiles
tar --owner=0 --group=0 -czf "$R/data.tar.gz" -C "$R/data" etc usr
echo "2.0" > "$R/debian-binary"
mkdir -p "$OUT"
tar -czf "$OUT/falcon_${REL}_${ARCH}.ipk" -C "$R" \
	./debian-binary ./control.tar.gz ./data.tar.gz
echo "built: $OUT/falcon_${REL}_${ARCH}.ipk"

# ---- luci-app-falcon (all) ---------------------------------------------------
R="$STAGE/luci"
mkdir -p "$R/data/usr/lib/lua/luci/controller" "$R/data/usr/lib/lua/luci/model/cbi" \
	"$R/data/usr/lib/lua/luci/view/falcon" "$R/CONTROL"
cp "$SRC/usr/lib/lua/luci/controller/falcon.lua" "$R/data/usr/lib/lua/luci/controller/"
cp "$SRC/usr/lib/lua/luci/model/cbi/falcon.lua" "$R/data/usr/lib/lua/luci/model/cbi/"
cp "$SRC/usr/lib/lua/luci/view/falcon/status.htm" "$R/data/usr/lib/lua/luci/view/falcon/"

cat > "$R/CONTROL/postinst" <<'EOF'
#!/bin/sh
# Drop the LuCI menu/index caches so the new controller routes are picked
# up without a reboot (idempotent; nothing to do when the cache is absent).
rm -f /tmp/luci-indexcache* 2>/dev/null || true
exit 0
EOF
cat > "$R/CONTROL/prerm" <<'EOF'
#!/bin/sh
exit 0
EOF
SIZE=$(du -sk "$R/data" | cut -f1)
cat > "$R/CONTROL/control" <<EOF
Package: luci-app-falcon
Version: $REL
Depends: falcon, curl
Source: https://github.com/cuihairu/falcon
SourceName: luci-app-falcon
License: Apache-2.0
Section: net
SourceDateEpoch: 1794931200
Maintainer: cuihairu <chuihairu@gmail.com>
Architecture: all
Installed-Size: $SIZE
Description: LuCI interface for falcon: settings page (UCI) and a transfer panel (service state, active transfers via aria2 tellActive, global stats, recent logs; add/pause/resume/remove tasks over the daemon JSON-RPC).
 falcon 的 LuCI 界面：设置页（UCI）与传输面板（服务状态/活动传输/全局统计/最近日志，经 daemon JSON-RPC 添加/暂停/继续/删除任务）。
EOF

chmod 755 "$R"/CONTROL/postinst "$R"/CONTROL/prerm
tar --owner=0 --group=0 -czf "$R/control.tar.gz" -C "$R/CONTROL" ./control ./postinst ./prerm
tar --owner=0 --group=0 -czf "$R/data.tar.gz" -C "$R/data" usr
echo "2.0" > "$R/debian-binary"
tar -czf "$OUT/luci-app-falcon_${REL}_all.ipk" -C "$R" \
	./debian-binary ./control.tar.gz ./data.tar.gz
echo "built: $OUT/luci-app-falcon_${REL}_all.ipk"

# ---- app-meta-falcon (all) — the iStore-facing metadata package --------------
R="$STAGE/meta"
mkdir -p "$R/data/usr/lib/opkg/meta" "$R/data/www/luci-static/resources/app-icons" \
	"$R/CONTROL"
# The manifest version tracks the CLI project version at build time.
sed "s/\"version\": \"[^\"]*\"/\"version\": \"$VERSION\"/" \
	"$SRC/app-meta/falcon.json" > "$R/data/usr/lib/opkg/meta/falcon.json"
cp "$REPO_DIR/assets/falcon.png" "$R/data/www/luci-static/resources/app-icons/falcon.png"

cat > "$R/CONTROL/postinst" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$R/CONTROL/prerm" <<'EOF'
#!/bin/sh
exit 0
EOF
SIZE=$(du -sk "$R/data" | cut -f1)
cat > "$R/CONTROL/control" <<EOF
Package: app-meta-falcon
Version: $REL
Depends: luci-app-falcon
Source: https://github.com/cuihairu/falcon
SourceName: app-meta-falcon
License: Apache-2.0
Section: net
SourceDateEpoch: 1794931200
Maintainer: cuihairu <chuihairu@gmail.com>
Architecture: all
Installed-Size: $SIZE
Description: iStore app metadata for falcon (icon and app manifest).
 falcon 的 iStore 应用元数据（图标与应用清单）。
EOF

chmod 755 "$R"/CONTROL/postinst "$R"/CONTROL/prerm
tar --owner=0 --group=0 -czf "$R/control.tar.gz" -C "$R/CONTROL" ./control ./postinst ./prerm
tar --owner=0 --group=0 -czf "$R/data.tar.gz" -C "$R/data" usr www
echo "2.0" > "$R/debian-binary"
tar -czf "$OUT/app-meta-falcon_${REL}_all.ipk" -C "$R" \
	./debian-binary ./control.tar.gz ./data.tar.gz
echo "built: $OUT/app-meta-falcon_${REL}_all.ipk"
