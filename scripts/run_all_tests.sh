#!/bin/bash

# Falcon 下载器 - 所有测试运行脚本
# 用于执行所有单元测试和集成测试

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi

    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
        return
    fi

    if command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
        return
    fi

    echo 4
}

run_gtest_binary() {
    local binary="$1"
    local xml_name="$2"
    local log_name="$3"

    if [ ! -x "$binary" ]; then
        print_warning "跳过未生成的测试二进制: $binary"
        return
    fi

    print_info "运行 $(basename "$binary")..."
    "$binary" --gtest_brief=1 --gtest_output="xml:test_results/$xml_name" \
        2>&1 | tee "test_results/$log_name"
}

resolve_plugin_flag() {
    local requested="$1"
    local path="$2"

    if [ "$requested" != "ON" ]; then
        echo "OFF"
        return
    fi

    if [ -e "$path" ]; then
        echo "ON"
    else
        print_warning "仓库中不存在插件实现，已自动禁用: $path" >&2
        echo "OFF"
    fi
}

# 检查是否在正确的目录
if [ ! -f "CMakeLists.txt" ]; then
    print_error "请在项目根目录运行此脚本"
    exit 1
fi

# 创建测试输出目录
mkdir -p test_results
mkdir -p test_reports

# 设置测试环境变量
export FALCON_TEST_MODE=1
export GTEST_OUTPUT="xml:test_results/"

# 构建配置
BUILD_DIR="build"
BUILD_TYPE="Debug"
ENABLE_ALL_PROTOCOLS=OFF
ENABLE_COVERAGE=OFF
BUILD_DESKTOP=ON
JOBS=$(detect_jobs)

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --enable-all)
            ENABLE_ALL_PROTOCOLS=ON
            shift
            ;;
        --disable-all)
            ENABLE_ALL_PROTOCOLS=OFF
            shift
            ;;
        --clean)
            print_info "清理构建目录..."
            rm -rf $BUILD_DIR
            shift
            ;;
        --coverage)
            ENABLE_COVERAGE=ON
            BUILD_DIR="build-coverage"
            shift
            ;;
        --skip-desktop)
            BUILD_DESKTOP=OFF
            shift
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --release         使用 Release 构建模式（默认 Debug）"
            echo "  --enable-all      启用所有协议插件"
            echo "  --disable-all     禁用私有协议插件"
            echo "  --coverage        启用覆盖率构建并生成 gcov 摘要"
            echo "  --skip-desktop    跳过桌面应用构建"
            echo "  --clean           清理构建目录"
            echo "  --help, -h        显示此帮助信息"
            exit 0
            ;;
        *)
            print_error "未知选项: $1"
            echo "使用 --help 查看帮助"
            exit 1
            ;;
    esac
done

print_info "开始构建 Falcon 下载器..."

ENABLE_BITTORRENT=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/bittorrent/CMakeLists.txt")
ENABLE_THUNDER=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/thunder/thunder_plugin.cpp")
ENABLE_QQDL=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/qqdl/qqdl_plugin.cpp")
ENABLE_FLASHGET=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/flashget/flashget_plugin.cpp")
ENABLE_ED2K=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/ed2k/ed2k_plugin.cpp")
ENABLE_HLS=$(resolve_plugin_flag "$ENABLE_ALL_PROTOCOLS" "packages/libfalcon/plugins/hls/hls_plugin.cpp")

if [ "$ENABLE_COVERAGE" = "ON" ] && [ -d "$BUILD_DIR" ]; then
    find "$BUILD_DIR" -name '*.gcda' -delete
fi

# 配置 CMake
print_info "配置 CMake (构建类型: $BUILD_TYPE)..."
cmake -B $BUILD_DIR \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DFALCON_BUILD_TESTS=ON \
    -DFALCON_BUILD_EXAMPLES=OFF \
    -DFALCON_BUILD_DESKTOP=$BUILD_DESKTOP \
    -DFALCON_ENABLE_COVERAGE=$ENABLE_COVERAGE \
    -DFALCON_ENABLE_HTTP=ON \
    -DFALCON_ENABLE_FTP=ON \
    -DFALCON_ENABLE_BITTORRENT=$ENABLE_BITTORRENT \
    -DFALCON_ENABLE_THUNDER=$ENABLE_THUNDER \
    -DFALCON_ENABLE_QQDL=$ENABLE_QQDL \
    -DFALCON_ENABLE_FLASHGET=$ENABLE_FLASHGET \
    -DFALCON_ENABLE_ED2K=$ENABLE_ED2K \
    -DFALCON_ENABLE_HLS=$ENABLE_HLS

# 编译
print_info "编译项目..."
cmake --build $BUILD_DIR --config $BUILD_TYPE -j"$JOBS" --target \
    falcon \
    falcon-cli \
    falcon-daemon \
    falcon_core_tests \
    falcon_cli_tests \
    falcon_daemon_rpc_tests \
    falcon_daemon_storage_tests

if [ "$BUILD_DESKTOP" = "ON" ]; then
    if cmake --build $BUILD_DIR --target help 2>/dev/null | grep -q '^falcon-desktop:'; then
        cmake --build $BUILD_DIR --config $BUILD_TYPE -j"$JOBS" --target falcon-desktop
    else
        print_warning "当前构建配置未生成 falcon-desktop 目标，已跳过"
    fi
fi

# 运行 todo.md 中列出的测试目标
print_info "运行测试目标..."
echo "=========================================="
echo "          验证测试 (Verification Tests)"
echo "=========================================="

run_gtest_binary "$BUILD_DIR/bin/falcon_core_tests" "falcon_core_tests.xml" "falcon_core_tests.log"
run_gtest_binary "$BUILD_DIR/bin/falcon_cli_tests" "falcon_cli_tests.xml" "falcon_cli_tests.log"
run_gtest_binary "$BUILD_DIR/bin/falcon_daemon_rpc_tests" "falcon_daemon_rpc_tests.xml" "falcon_daemon_rpc_tests.log"
run_gtest_binary "$BUILD_DIR/bin/falcon_daemon_storage_tests" "falcon_daemon_storage_tests.xml" "falcon_daemon_storage_tests.log"

# 生成测试报告
print_info "生成测试报告..."
python3 scripts/generate_test_report.py test_results/ > test_reports/test_report.html 2>/dev/null || \
    print_warning "无法生成HTML测试报告（需要Python）"

# 测试覆盖率报告（如果启用）
if [ "$ENABLE_COVERAGE" = "ON" ] && command -v gcov >/dev/null 2>&1; then
    print_info "生成代码覆盖率摘要..."
    COVERAGE_REPORT="$BUILD_DIR/coverage-summary.txt"
    gcov -b -c \
        "$BUILD_DIR/packages/libfalcon/CMakeFiles/falcon.dir/src/download_engine.cpp.gcno" \
        "$BUILD_DIR/packages/libfalcon/CMakeFiles/falcon.dir/src/task_manager.cpp.gcno" \
        "$BUILD_DIR/packages/libfalcon/CMakeFiles/falcon.dir/src/download_task.cpp.gcno" \
        "$BUILD_DIR/packages/libfalcon/CMakeFiles/falcon.dir/src/download_engine_v2.cpp.gcno" \
        > "$COVERAGE_REPORT" 2>&1 || true

    if [ -s "$COVERAGE_REPORT" ]; then
        print_success "覆盖率摘要已生成: $COVERAGE_REPORT"
    else
        print_warning "未生成覆盖率摘要，请确认测试已产出 .gcda 文件"
    fi
fi

# 总结测试结果
print_info "测试完成！"
echo ""
echo "测试结果文件保存在:"
echo "  - 日志文件: test_results/"
echo "  - XML报告: test_results/*.xml"
echo "  - HTML报告: test_reports/ (如果可用)"
echo "  - 覆盖率摘要: $BUILD_DIR/coverage-summary.txt (如果启用)"

# 检查测试结果中的错误
error_count=0
for log_file in test_results/*.log; do
    if [ -f "$log_file" ]; then
        errors=$(grep -c "FAILED" "$log_file" 2>/dev/null || true)
        errors=${errors:-0}
        if [ "$errors" -gt 0 ]; then
            print_error "$(basename $log_file): $errors 个失败"
            error_count=$((error_count + errors))
        fi
    fi
done

# 退出状态
if [ $error_count -gt 0 ]; then
    print_error "总共 $error_count 个测试失败"
    exit 1
else
    print_success "所有测试通过！"
    exit 0
fi
