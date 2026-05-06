#!/bin/bash

# Falcon Desktop macOS 打包脚本
# 使用 vcpkg 管理所有依赖，包括 Qt6

set -e

# ============================================================================
# 配置
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${PROJECT_ROOT}/build-desktop"
OUTPUT_DIR="${PROJECT_ROOT}/dist"
APP_NAME="Falcon"
APP_BUNDLE="${APP_NAME}.app"
VERSION="$(git -C "$PROJECT_ROOT" describe --tags --always 2>/dev/null || echo '0.1.0')"
DMG_NAME="${APP_NAME}-${VERSION}-macos-intel"

# vcpkg 配置
DEFAULT_VCPKG_ROOT="$HOME/vcpkg"
VCPKG_ROOT="${VCPKG_ROOT:-$DEFAULT_VCPKG_ROOT}"
VCPKG_TRIPLET="x64-osx"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# ============================================================================
# 工具函数
# ============================================================================

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_ok() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_err() { echo -e "${RED}[ERROR]${NC} $1"; }

die() { log_err "$1"; exit 1; }

# ============================================================================
# 预检查
# ============================================================================

log_info "Falcon Desktop 打包脚本"
log_info "版本: $VERSION"

# 检查 vcpkg
if [[ ! -d "$VCPKG_ROOT" ]]; then
    die "vcpkg 未找到: $VCPKG_ROOT
请安装: git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg && cd ~/vcpkg && ./bootstrap-vcpkg.sh"
fi

VCPKG_TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "$VCPKG_TOOLCHAIN" ]]; then
    die "vcpkg 工具链文件不存在: $VCPKG_TOOLCHAIN"
fi

log_ok "vcpkg: $VCPKG_ROOT"

# 检查必要的命令
command -v cmake >/dev/null 2>&1 || die "请安装 cmake: brew install cmake"
command -v git >/dev/null 2>&1 || die "请安装 git"

# 检查 vcpkg Qt 安装
QT_ROOT="$VCPKG_ROOT/installed/$VCPKG_TRIPLET"
if [[ ! -d "$QT_ROOT/include/qt6" ]]; then
    log_warn "vcpkg 未安装 Qt，正在安装..."
    cd "$VCPKG_ROOT"
    ./vcpkg install qt[qtbase,qttools] --triplet="$VCPKG_TRIPLET"
    cd "$PROJECT_ROOT"
fi

log_ok "Qt6: $QT_ROOT"

# ============================================================================
# 清理旧构建
# ============================================================================

log_info "清理旧构建..."
rm -rf "$BUILD_DIR"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# 禁用 CMake 自动创建 .app，我们将手动创建
export CMAKE_MACOSX_BUNDLE=FALSE

# ============================================================================
# 配置和构建
# ============================================================================

log_info "配置 CMake..."

# 创建预加载脚本定义 BZip2::BZip2 目标（修复 vcpkg freetype 依赖问题）
PRELOAD_FILE="$BUILD_DIR/preload.cmake"
mkdir -p "$BUILD_DIR"
cat > "$PRELOAD_FILE" <<EOF
# 修复 vcpkg freetype BZip2 依赖问题
# 使用绝对路径因为预加载阶段 VCPKG 变量可能未设置
if(NOT TARGET BZip2::BZip2)
  if(EXISTS "$VCPKG_ROOT/installed/$VCPKG_TRIPLET/lib/libbz2.a")
    add_library(BZip2::BZip2 STATIC IMPORTED GLOBAL)
    set_target_properties(BZip2::BZip2 PROPERTIES
      IMPORTED_LOCATION "$VCPKG_ROOT/installed/$VCPKG_TRIPLET/lib/libbz2.a"
      INTERFACE_INCLUDE_DIRECTORIES "$VCPKG_ROOT/installed/$VCPKG_TRIPLET/include"
    )
  endif()
endif()
EOF

cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
    -C "$PRELOAD_FILE" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN" \
    -DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET" \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DCMAKE_INSTALL_PREFIX="$OUTPUT_DIR" \
    -DCMAKE_MODULE_PATH="$PROJECT_ROOT/cmake" \
    -DCMAKE_OSX_ARCHITECTURES="x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="10.15" \
    -DCMAKE_MACOSX_BUNDLE=OFF \
    -DFALCON_BUILD_DESKTOP=ON \
    -DFALCON_BUILD_TESTS=OFF \
    -DFALCON_BUILD_EXAMPLES=OFF \
    -DFALCON_BUILD_CLI=OFF \
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=OFF \
    -DCMAKE_MAKE_PROGRAM="$(command -v make)" \
    -DCMAKE_C_COMPILER="$(command -v clang)" \
    -DCMAKE_CXX_COMPILER="$(command -v clang++)"

log_info "编译..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target falcon-desktop --parallel

# ============================================================================
# 创建 .app 捆绑包
# ============================================================================

log_info "创建 .app 捆绑包..."

APP_PATH="$OUTPUT_DIR/$APP_BUNDLE"
CONTENTS="$APP_PATH/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"
FRAMEWORKS="$CONTENTS/Frameworks"
PLUGINS="$CONTENTS/PlugIns"

mkdir -p "$MACOS" "$RESOURCES" "$FRAMEWORKS" "$PLUGINS"

# 复制可执行文件
cp "$BUILD_DIR/bin/falcon-desktop" "$MACOS/$APP_NAME"

# 创建 Info.plist
cat > "$CONTENTS/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>
    <string>com.falcon.downloader</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.15</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
EOF

# 复制图标
if [[ -f "$PROJECT_ROOT/assets/falcon.icns" ]]; then
    cp "$PROJECT_ROOT/assets/falcon.icns" "$RESOURCES/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile AppIcon.icns" "$CONTENTS/Info.plist" 2>/dev/null || true
fi

# ============================================================================
# 部署 Qt 依赖
# ============================================================================

log_info "部署 Qt 框架和插件..."

# 复制 Qt 框架
QT_FRAMEWORKS=("QtCore" "QtGui" "QtWidgets" "QtNetwork" "QtConcurrent")
for fw in "${QT_FRAMEWORKS[@]}"; do
    if [[ -d "$QT_ROOT/lib/${fw}.framework" ]]; then
        log_info "  复制 $fw..."
        cp -R "$QT_ROOT/lib/${fw}.framework" "$FRAMEWORKS/"
    fi
done

# 复制插件 (vcpkg 插件位置)
mkdir -p "$PLUGINS/platforms"
if [[ -f "$QT_ROOT/share/qt6/plugins/platforms/libqcocoa.dylib" ]]; then
    cp "$QT_ROOT/share/qt6/plugins/platforms/libqcocoa.dylib" "$PLUGINS/platforms/"
elif [[ -f "$QT_ROOT/plugins/platforms/libqcocoa.dylib" ]]; then
    cp "$QT_ROOT/plugins/platforms/libqcocoa.dylib" "$PLUGINS/platforms/"
fi

# 复制 OpenSSL (vcpkg 安装在 lib 目录)
if [[ -f "$QT_ROOT/lib/libssl.dylib" ]]; then
    cp "$QT_ROOT/lib/libssl.dylib" "$FRAMEWORKS/"
fi
if [[ -f "$QT_ROOT/lib/libcrypto.dylib" ]]; then
    cp "$QT_ROOT/lib/libcrypto.dylib" "$FRAMEWORKS/"
fi

# ============================================================================
# 修复动态库路径
# ============================================================================

log_info "修复动态库链接路径..."

change_id() {
    local lib="$1"
    local new_id="$2"
    install_name_tool -id "$new_id" "$lib" 2>/dev/null || true
}

change_ref() {
    local bin="$1"
    local old="$2"
    local new="$3"
    install_name_tool -change "$old" "$new" "$bin" 2>/dev/null || true
}

# 修复框架 ID
for fw in "${QT_FRAMEWORKS[@]}"; do
    fw_path="$FRAMEWORKS/${fw}.framework/${fw}"
    if [[ -f "$fw_path" ]]; then
        change_id "$fw_path" "@executable_path/../Frameworks/${fw}.framework/${fw}"
    fi
done

# 修复插件
for plugin in "$PLUGINS"/**/*.dylib; do
    if [[ -f "$plugin" ]]; then
        name=$(basename "$plugin")
        dir=$(basename "$(dirname "$plugin")")
        change_id "$plugin" "@executable_path/../PlugIns/${dir}/${name}"
    fi
done

# 修复 OpenSSL
if [[ -f "$FRAMEWORKS/libssl.dylib" ]]; then
    change_id "$FRAMEWORKS/libssl.dylib" "@executable_path/../Frameworks/libssl.dylib"
fi
if [[ -f "$FRAMEWORKS/libcrypto.dylib" ]]; then
    change_id "$FRAMEWORKS/libcrypto.dylib" "@executable_path/../Frameworks/libcrypto.dylib"
fi

# 修复可执行文件引用
change_ref "$MACOS/$APP_NAME" "$QT_ROOT/lib/" "@executable_path/../Frameworks/"

# 修复框架之间的相互引用
for fw in "${QT_FRAMEWORKS[@]}"; do
    change_ref "$MACOS/$APP_NAME" "$fw.framework" "@executable_path/../Frameworks/${fw}.framework/${fw}"
done

# 修复插件的 Qt 引用
for plugin in "$PLUGINS"/**/*.dylib; do
    if [[ -f "$plugin" ]]; then
        for fw in "${QT_FRAMEWORKS[@]}"; do
            change_ref "$plugin" "$QT_ROOT/lib/${fw}.framework/${fw}" \
                "@executable_path/../Frameworks/${fw}.framework/${fw}"
        done
    fi
done

# 添加 rpath
install_name_tool -add_rpath "@executable_path/../Frameworks" "$MACOS/$APP_NAME" 2>/dev/null || true

# ============================================================================
# 代码签名 (临时签名，用于分发)
# ============================================================================

log_info "添加临时签名..."
codesign --force --deep --sign - "$APP_PATH" 2>/dev/null || log_warn "签名失败，继续..."

# ============================================================================
# 创建 DMG
# ============================================================================

if [[ "$1" == "--dmg" || "$1" == "-d" ]]; then
    log_info "创建 DMG 镜像..."

    DMG_PATH="${OUTPUT_DIR}/${DMG_NAME}.dmg"

    # 使用 hdiutil 创建
    hdiutil create -volname "$APP_NAME" \
        -srcfolder "$APP_PATH" \
        -ov -format UDZO \
        -imagekey zlib-level=9 \
        "$DMG_PATH"

    log_ok "DMG 已创建: $DMG_PATH"
fi

# ============================================================================
# 完成
# ============================================================================

echo ""
log_ok "打包完成！"
echo ""
echo "输出: $OUTPUT_DIR"
echo "应用: $APP_PATH"
echo ""

if [[ -d "$APP_PATH" ]]; then
    echo "应用大小:"
    du -sh "$APP_PATH"
    echo ""
    echo "测试运行:"
    echo "  open \"$APP_PATH\""
fi
