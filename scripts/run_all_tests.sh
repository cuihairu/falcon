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
ENABLE_ALL_PROTOCOLS=ON

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
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --release         使用 Release 构建模式（默认 Debug）"
            echo "  --enable-all      启用所有协议插件"
            echo "  --disable-all     禁用私有协议插件"
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

# 配置 CMake
print_info "配置 CMake (构建类型: $BUILD_TYPE)..."
cmake -B $BUILD_DIR \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DFALCON_BUILD_TESTS=ON \
    -DFALCON_BUILD_EXAMPLES=ON \
    -DFALCON_ENABLE_HTTP=ON \
    -DFALCON_ENABLE_FTP=ON \
    -DFALCON_ENABLE_BITTORRENT=$ENABLE_ALL_PROTOCOLS \
    -DFALCON_ENABLE_THUNDER=$ENABLE_ALL_PROTOCOLS \
    -DFALCON_ENABLE_QQDL=$ENABLE_ALL_PROTOCOLS \
    -DFALCON_ENABLE_FLASHGET=$ENABLE_ALL_PROTOCOLS \
    -DFALCON_ENABLE_ED2K=$ENABLE_ALL_PROTOCOLS \
    -DFALCON_ENABLE_HLS=$ENABLE_ALL_PROTOCOLS

# 编译
print_info "编译项目..."
cmake --build $BUILD_DIR --config $BUILD_TYPE -j$(nproc)

# 运行核心测试
print_info "运行核心测试..."
echo "=========================================="
echo "          核心测试 (Core Tests)"
echo "=========================================="

ctest --test-dir $BUILD_DIR --output-on-failure \
    --label "core" --verbose \
    --output-junit test_results/core_results.xml \
    2>&1 | tee test_results/core_results.log

# 运行 HTTP 插件测试
print_info "运行 HTTP 插件测试..."
ctest --test-dir $BUILD_DIR --output-on-failure \
    --label "http" --verbose \
    --output-junit test_results/http_results.xml \
    2>&1 | tee test_results/http_results.log

# 运行私有协议测试
if [ "$ENABLE_ALL_PROTOCOLS" = "ON" ]; then
    print_info "运行私有协议插件测试..."
    ctest --test-dir $BUILD_DIR --output-on-failure \
        --label "private" --verbose \
        --output-junit test_results/private_results.xml \
        2>&1 | tee test_results/private_results.log
fi

# 运行集成测试
print_info "运行集成测试..."
echo "=========================================="
echo "          集成测试 (Integration Tests)"
echo "=========================================="

ctest --test-dir $BUILD_DIR --output-on-failure \
    --label "integration" --verbose \
    --output-junit test_results/integration_results.xml \
    2>&1 | tee test_results/integration_results.log

# 生成测试报告
print_info "生成测试报告..."
python3 scripts/generate_test_report.py test_results/ > test_reports/test_report.html 2>/dev/null || \
    print_warning "无法生成HTML测试报告（需要Python）"

# 测试覆盖率报告（如果启用）
if command -v gcov &> /dev/null && [ "$BUILD_TYPE" = "Debug" ]; then
    print_info "生成代码覆盖率报告..."
    cd $BUILD_DIR
    gcov -r ../packages/libfalcon/src/*.cpp 2>/dev/null || true
    lcov --capture --directory ../packages/libfalcon/src --output-file coverage.info 2>/dev/null || true
    genhtml coverage.info --output-directory coverage_report 2>/dev/null || true
    cd ..
fi

# 运行示例程序
print_info "运行示例程序..."
echo "=========================================="
echo "          示例程序 (Examples)"
echo "=========================================="

# 私有协议示例
if [ "$ENABLE_ALL_PROTOCOLS" = "ON" ]; then
    print_info "运行私有协议示例..."
    $BUILD_DIR/bin/private_protocols_demo 2>&1 | tee test_results/private_protocols_demo.log
fi

# 总结测试结果
print_info "测试完成！"
echo ""
echo "测试结果文件保存在:"
echo "  - 日志文件: test_results/"
echo "  - XML报告: test_results/*.xml"
echo "  - HTML报告: test_reports/ (如果可用)"
echo "  - 覆盖率报告: build/coverage_report/ (如果可用)"

# 检查测试结果中的错误
error_count=0
for log_file in test_results/*.log; do
    if [ -f "$log_file" ]; then
        errors=$(grep -c "FAILED" "$log_file" 2>/dev/null || echo "0")
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