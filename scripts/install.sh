#!/usr/bin/env bash
# Falcon 一键安装脚本（Linux / macOS）——从每日构建（nightly release）安装桌面版。
#
# 用法（匿名下载，无需登录）：
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/falcon/main/scripts/install.sh | bash
#
# 特性：
#   - 自动检测 OS 与 CPU 架构（x86_64 / aarch64 / arm64），未知组合明确报错
#   - 幂等：重跑即升级（覆盖旧安装）
#   - 失败即停（set -euo pipefail）+ 明确中文报错
#   - 验证：AppImage 运行时自检 + 载荷完整性（包内 falcon-cli 存在时做
#     `--version` 真验证；桌面二进制绝不执行——旧产物不识别 --version 会
#     直接拉起 GUI 挂死）
#
# 环境变量（测试接缝 / 高级定制）：
#   FALCON_UNAME_S / FALCON_UNAME_M  覆盖 uname 探测（演练失败路径）
#   FALCON_INSTALL_DIR               Linux 安装目录（默认 $HOME/.local/bin）
set -euo pipefail

REPO="cuihairu/falcon"
RELEASE_TAG="nightly"
BASE_URL="https://github.com/${REPO}/releases/download/${RELEASE_TAG}"
MARKER="# >>> falcon installer >>>"

info() { printf '>>> %s\n' "$*"; }
die()  { printf '\n安装失败：%s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "缺少必要命令：$1（请先安装）"; }

os_name()   { printf '%s' "${FALCON_UNAME_S:-$(uname -s)}"; }
arch_name() { printf '%s' "${FALCON_UNAME_M:-$(uname -m)}"; }

# 按 OS+架构映射 nightly 资产名；不认识的组合 die（绝不猜测下载地址）
select_asset() {
  local os arch
  os=$(os_name)
  arch=$(arch_name)
  case "$os" in
    Linux)
      case "$arch" in
        x86_64|amd64) printf 'falcon-desktop-linux-nightly.AppImage' ;;
        aarch64|arm64) printf 'falcon-desktop-linux-arm64-nightly.AppImage' ;;
        *) die "不支持的 CPU 架构：Linux/${arch}（目前支持 x86_64、aarch64）" ;;
      esac ;;
    Darwin)
      case "$arch" in
        arm64|aarch64) printf 'falcon-desktop-macos-arm64-nightly.dmg' ;;
        x86_64) die "macOS Intel (x86_64) 暂无 nightly 产物——当前仅提供 Apple Silicon 构建。源码构建：https://github.com/cuihairu/falcon#installation" ;;
        *) die "不支持的 CPU 架构：macOS/${arch}" ;;
      esac ;;
    *) die "不支持的操作系统：${os}（Windows 请用 PowerShell：irm https://raw.githubusercontent.com/${REPO}/main/scripts/install.ps1 | iex）" ;;
  esac
}

TMP_WORK=""

cleanup() {
  if [ -n "$TMP_WORK" ] && [ -d "$TMP_WORK" ]; then
    rm -rf "$TMP_WORK"
  fi
}
trap cleanup EXIT

# 下载到 $2；404 与网络失败分别给明确报错（资产不存在 ≠ 网络问题）
download_asset() {
  local url="$1" out="$2"
  info "下载 ${url##*/}"
  if ! curl -fL --retry 3 --connect-timeout 15 --progress-bar -o "$out" "$url"; then
    local code
    code=$(curl -sIL -o /dev/null -w '%{http_code}' --connect-timeout 15 "$url" 2>/dev/null || printf '000')
    case "$code" in
      404) die "资产不存在（HTTP 404）：${url##*/}——该平台的 nightly 产物尚未发布（linux-arm64 需等首次 arm 构建完成），或地址已变更" ;;
      000) die "网络不可达：无法访问 ${url}（检查网络/代理后重试）" ;;
      *)   die "下载失败（HTTP ${code}）：${url}" ;;
    esac
  fi
  [ -s "$out" ] || die "下载文件为空：${out}"
}

# PATH 处理：缺则提示（并幂等写入 ~/.profile 标记块）
ensure_path() {
  local bindir="$1"
  case ":$PATH:" in
    *":$bindir:"*) info "PATH 已包含 ${bindir}" ;;
    *)
      if grep -Fq "$MARKER" "$HOME/.profile" 2>/dev/null; then
        info "PATH 未包含 ${bindir}；~/.profile 已有安装器块，打开新终端后生效"
      else
        info "PATH 未包含 ${bindir}——追加到 ~/.profile（标记块，幂等）"
        {
          printf '\n%s\n' "$MARKER"
          # shellcheck disable=SC2016  # 写入的是字面 $PATH（供新终端展开）
          printf 'export PATH="%s:$PATH"\n' "$bindir"
          printf '# <<< falcon installer <<<\n'
        } >> "$HOME/.profile"
      fi
      export PATH="${bindir}:$PATH"
      ;;
  esac
}

# Linux 验证：① AppImage 运行时自检（--appimage-version 由运行时层拦截，
# 不触达内层桌面程序，缺 FUSE 也不受影响）；② 载荷完整性（解出 usr/bin）；
# ③ 包内 falcon-cli 存在时执行 `--version` 真验证。桌面二进制绝不执行。
verify_appimage() {
  local dest="$1" work="$2"

  if ! "$dest" --appimage-version >/dev/null 2>&1; then
    die "AppImage 运行时自检失败（--appimage-version 非零退出）。文件可能损坏——重跑本脚本重新下载；若确认损坏，可尝试安装 fuse2/fuse3 后手动运行：$dest --appimage-extract-and-run"
  fi
  info "验证：AppImage 运行时自检通过"

  if ! (cd "$work" && "$dest" --appimage-extract usr/bin >/dev/null 2>&1); then
    die "AppImage 载荷解包失败（--appimage-extract usr/bin）——产物可能损坏，重跑本脚本重试"
  fi
  local payload="$work/squashfs-root/usr/bin/falcon-desktop"
  [ -x "$payload" ] || die "AppImage 载荷缺失或不可执行：usr/bin/falcon-desktop"
  info "验证：载荷完整性通过（usr/bin/falcon-desktop 存在且可执行）"

  local cli="$work/squashfs-root/usr/bin/falcon-cli"
  if [ -x "$cli" ]; then
    local ver
    if ! ver=$("$cli" --version 2>&1); then
      die "包内 falcon-cli --version 执行失败：${ver}"
    fi
    info "验证：falcon-cli --version → ${ver}"
  else
    info "说明：本 AppImage 未附带 falcon-cli，跳过 --version 直验（以运行时自检+载荷完整性为准）"
  fi
}

# 桌面入口（.desktop + 图标）：尽力而为，失败不影响安装结果
install_desktop_entry() {
  local dest="$1" work="$2"
  local apps="$HOME/.local/share/applications"
  local icons="$HOME/.local/share/icons/hicolor/256x256/apps"
  mkdir -p "$apps" "$icons" 2>/dev/null || return 0

  (cd "$work" && "$dest" --appimage-extract usr/share/icons >/dev/null 2>&1) || true
  local icon="$work/squashfs-root/usr/share/icons/hicolor/256x256/apps/falcon-desktop.png"
  if [ -f "$icon" ]; then
    cp "$icon" "$icons/falcon-desktop.png" 2>/dev/null || true
  fi

  cat > "$apps/falcon-desktop.desktop" 2>/dev/null <<EOF || true
[Desktop Entry]
Type=Application
Name=Falcon
Comment=Falcon downloader
Exec=${dest}
Icon=falcon-desktop
Terminal=false
Categories=Network;
EOF
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$apps" >/dev/null 2>&1 || true
  fi
  info "桌面入口已写入 ~/.local/share/applications/falcon-desktop.desktop"
}

install_linux() {
  local asset dest_dir dest img
  asset=$(select_asset) || exit 1
  dest_dir="${FALCON_INSTALL_DIR:-$HOME/.local/bin}"
  dest="${dest_dir}/falcon-desktop"

  need curl
  need mktemp
  mkdir -p "$dest_dir" || die "无法创建安装目录：${dest_dir}"
  TMP_WORK=$(mktemp -d) || die "mktemp 失败"
  img="${TMP_WORK}/${asset}"

  download_asset "${BASE_URL}/${asset}" "$img"

  # 先装后验：mv + chmod（重跑 = 覆盖升级，幂等）
  if ! mv -f "$img" "$dest"; then
    die "写入安装目标失败：${dest}（权限不足？检查 ${dest_dir} 的写权限）"
  fi
  chmod +x "$dest" || die "chmod +x 失败：${dest}"

  verify_appimage "$dest" "$TMP_WORK"
  ensure_path "$dest_dir"
  install_desktop_entry "$dest" "$TMP_WORK"

  printf '\n'
  info "安装完成：${dest}"
  info "启动方式：直接运行 ${dest}（PATH 生效后：falcon-desktop）"
  if [ -z "${FALCON_INSTALL_DIR:-}" ]; then
    case ":$PATH:" in
      *":${dest_dir}:"*) : ;;
      *) info "提示：PATH 写入已在 ~/.profile，打开新终端后可直接敲 falcon-desktop" ;;
    esac
  fi
}

install_macos() {
  local asset dest_root dest img mnt app
  asset=$(select_asset) || exit 1
  dest_root="/Applications"
  if [ ! -w "$dest_root" ]; then
    dest_root="$HOME/Applications"
    info "/Applications 不可写，改装到 ${dest_root}"
  fi
  dest="${dest_root}/falcon.app"

  need curl
  need hdiutil
  mkdir -p "$dest_root" || die "无法创建安装目录：${dest_root}"
  TMP_WORK=$(mktemp -d) || die "mktemp 失败"
  img="${TMP_WORK}/${asset}"
  mnt="${TMP_WORK}/mnt"

  download_asset "${BASE_URL}/${asset}" "$img"

  hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$img" >/dev/null \
    || die "挂载 DMG 失败：${img}"

  app=$(find "$mnt" -maxdepth 1 -name '*.app' -type d | head -n 1)
  [ -n "$app" ] || { hdiutil detach "$mnt" >/dev/null 2>&1 || true; die "DMG 内未找到 .app 应用包"; }

  # 重跑 = 覆盖升级；旧版本被占用（正在运行）时 ditto 会失败
  rm -rf "$dest" 2>/dev/null || die "无法清理旧版本：${dest}（Falcon 正在运行？请退出后重试）"
  if ! ditto "$app" "$dest"; then
    hdiutil detach "$mnt" >/dev/null 2>&1 || true
    die "复制到 ${dest} 失败"
  fi
  hdiutil detach "$mnt" >/dev/null 2>&1 || true

  # 验证：文件存在 + 可执行位；绝不执行（旧产物不识别 --version 会拉起 GUI）
  [ -x "${dest}/Contents/MacOS/falcon-desktop" ] \
    || die "应用包不完整：${dest}/Contents/MacOS/falcon-desktop 缺失或不可执行"

  printf '\n'
  info "安装完成：${dest}"
  info "启动方式：在「启动台/应用程序」打开 Falcon，或 open \"${dest}\""
  info "Gatekeeper：本包为 ad-hoc 签名未公证——首次打开请右键 →「打开」，或执行：xattr -dr com.apple.quarantine \"${dest}\""
}

main() {
  case "$(os_name)" in
    Linux)  install_linux ;;
    Darwin) install_macos ;;
    *)      select_asset || exit 1 ;;
  esac
}

main "$@"
