#!/bin/bash

# Falcon 构建脚本

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

set -e  # 遇到错误时退出

# 默认参数
BUILD_TYPE="Release"
ENABLE_TESTS="OFF"
ENABLE_EXAMPLES="OFF"
ENABLE_CLI="OFF"
ENABLE_DAEMON="OFF"
VCPKG_ROOT=""

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            ENABLE_TESTS="ON"
            ENABLE_EXAMPLES="ON"
            shift
            ;;
        -t|--tests)
            ENABLE_TESTS="ON"
            shift
            ;;
        -c|--cli)
            ENABLE_CLI="ON"
            shift
            ;;
        --daemon)
            ENABLE_DAEMON="ON"
            shift
            ;;
        --vcpkg)
            VCPKG_ROOT="$2"
            shift 2
            ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  -d, --debug        启用调试模式"
            echo "  -t, --tests        启用测试"
            echo "  -c, --cli          启用 CLI 工具"
            "  --daemon         启用守护进程"
            echo "  --vcpkg <path>    指定 vcpkg 路径"
            echo "  -h, --help        显示此帮助信息"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            echo "使用 -h 或 --help 查看帮助"
            exit 1
            ;;
    esac
done

# 设置 vcpkg 路径
if [[ -z "$VCPKG_ROOT" ]]; then
    # 尝试从环境变量获取
    VCPKG_ROOT="${VCPKG_ROOT:-}"

    # 尝试常见位置
    if [[ -z "$VCPKG_ROOT" ]]; then
        if [[ -d "$HOME/vcpkg" ]]; then
            VCPKG_ROOT="$HOME/vcpkg"
        elif [[ -d "/usr/local/vcpkg" ]]; then
            VCPKG_ROOT="/usr/local/vcpkg"
        elif [[ -d "/opt/vcpkg" ]]; then
            VCPKG_ROOT="/opt/vcpkg"
        else
            echo -e "${YELLOW}警告: 未找到 vcpkg。请安装 vcpkg 或使用 --vcpkg 指定路径${NC}"
            echo "安装命令："
            echo "  git clone https://github.com/Microsoft/vcpkg.git"
            echo "  ./vcpkg/bootstrap-vcpkg.sh"
            echo ""
            echo "或者使用系统包管理器安装依赖"
            echo "  macOS: brew install curl spdlog nlohmann-json"
            echo "  Ubuntu: sudo apt-get install libcurl4-openssl-dev libspdlog-dev nlohmann-json3-dev"
            exit 1
        fi
    fi
fi

export VCPKG_ROOT

# 检测平台
PLATFORM="$(uname -s)"
case "$PLATFORM" in
    Darwin*)
        TRIPLET="x64-osx"
        TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
        ;;
    Linux*)
        TRIPLET="x64-linux"
        TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
        ;;
    CYGWIN*|MINGW*|MSYS*)
        TRIPLET="x64-windows"
        TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
        ;;
    *)
        echo -e "${RED}错误: 不支持的平台 $PLATFORM${NC}"
        exit 1
        ;;
esac

# 创建构建目录
BUILD_DIR="build-${BUILD_TYPE,,}"
if [[ -d "$BUILD_DIR" ]]; then
    echo -e "${YELLOW}清理旧的构建目录: $BUILD_DIR${NC}"
    rm -rf "$BUILD_DIR"
fi

echo -e "${GREEN}开始构建 Falcon Downloader...${NC}"
echo "构建类型: $BUILD_TYPE"
echo "目标平台: $TRIPLET"
echo "Vcpkg 路径: $VCPKG_ROOT"

# 配置 CMake
echo -e "${YELLOW}配置 CMake...${NC}"
CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -S .
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DFALCON_ENABLE_HTTP=ON
    -DFALCON_ENABLE_FTP=OFF
    -DFALCON_ENABLE_BITTORRENT=OFF
    -DFALCON_BUILD_TESTS="$ENABLE_TESTS"
    -DFALCON_BUILD_EXAMPLES="$ENABLE_EXAMPLES"
    -DFALCON_BUILD_CLI="$ENABLE_CLI"
    -DFALCON_BUILD_DAEMON="$ENABLE_DAEMON"
)

# 如果 vcpkg 存在，使用 vcpkg 工具链
if [[ -d "$VCPKG_ROOT" && -f "$TOOLCHAIN_FILE" ]]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE")
    CMAKE_ARGS+=(-DVCPKG_TARGET_TRIPLET="$TRIPLET")
    echo "使用 vcpkg 工具链"
fi

cmake "${CMAKE_ARGS[@]}"

# 构建
echo -e "${YELLOW}构建项目...${NC}"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel

# 成功信息
echo ""
echo -e "${GREEN}构建成功！${NC}"
echo "输出目录: $(pwd)/$BUILD_DIR"

# 显示生成的文件
echo ""
echo -e "${YELLOW}生成的文件:${NC}"
find "$BUILD_DIR" -name "*.a" -o -name "*.so" -o -name "*.dylib" -o -name "*.exe" 2>/dev/null | while read -r file; do
    echo "  - $file"
done

# 如果构建了 CLI，显示使用方法
if [[ "$ENABLE_CLI" == "ON" ]] && [[ -f "$BUILD_DIR/bin/falcon-cli" ]]; then
    echo ""
    echo -e "${GREEN}CLI 工具已构建！${NC}"
    echo "使用方法:"
    echo "  $BUILD_DIR/bin/falcon-cli --help"
fi

# 如果启用了测试，显示测试命令
if [[ "$ENABLE_TESTS" == "ON" ]]; then
    echo ""
    echo -e "${YELLOW}运行测试:${NC}"
    echo "  cd $BUILD_DIR && ctest --output-on-failure"
fi