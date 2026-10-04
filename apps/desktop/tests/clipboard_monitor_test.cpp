/**
 * @file clipboard_monitor_test.cpp
 * @brief ClipboardMonitor 单测（QApplication 级测试基建首发）
 *
 * xvfb 口径：QClipboard 需要平台层支撑，验证以 xvfb-run（xcb 平台）
 * 为准；无显示环境回落 offscreen（进程内剪贴板往返 + changed 信号
 * 均可用——探针程序实证，见 main 的平台选择逻辑）。
 *
 * 检测路径驱动：生产路径是 QClipboard::changed(Clipboard) 直连
 * check_clipboard（setText 同步触发），轮询 QTimer 是兜底；去重语义
 * 用小间隔定时器路径独立钉住（不依赖 changed 重复置位语义）。
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "utils/clipboard_monitor.hpp"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>

#include <functional>

namespace {

using falcon::desktop::ClipboardMonitor;

// 事件循环自旋等待（沿 ipc_server_test 口径：等待必须 processEvents 驱动）
bool spin_until(const std::function<bool()>& pred, int timeout_ms = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() >= timeout_ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

// 泵事件一段时间（不设条件，用于定时器路径的观察窗口）
void pump_events(int duration_ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

/// 每用例唯一 URL：排除剪贴板单例跨用例残留对 dedup 观察的干扰
int g_url_seq = 0;
QString unique_url(const char* suffix)
{
    return QString("https://example.com/%1-%2/file.zip")
        .arg(++g_url_seq)
        .arg(suffix);
}

class ClipboardMonitorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        monitor = std::make_unique<ClipboardMonitor>(QGuiApplication::clipboard());
        QObject::connect(monitor.get(), &ClipboardMonitor::url_detected,
                         monitor.get(), [this](const falcon::desktop::UrlInfo&) { ++emissions; });
        EXPECT_FALSE(monitor->is_monitoring());
        EXPECT_FALSE(monitor->last_url().is_valid);
    }

    std::unique_ptr<ClipboardMonitor> monitor;
    int emissions = 0;
};

} // namespace

// ---------- 构造与生命周期 ----------

TEST_F(ClipboardMonitorTest, NullClipboardConstructedAndCycledSafely)
{
    // clipboard 可注入空：check_clipboard 有守卫，定时器空转不崩
    ClipboardMonitor null_monitor(nullptr);
    null_monitor.set_detection_delay(40); // 让定时器在观察窗内真实触发守卫
    EXPECT_FALSE(null_monitor.is_monitoring());

    null_monitor.start();
    EXPECT_TRUE(null_monitor.is_monitoring());
    pump_events(200); // 定时器触发 check_clipboard（空指针守卫路径）

    null_monitor.stop();
    EXPECT_FALSE(null_monitor.is_monitoring());
}

TEST_F(ClipboardMonitorTest, StartStopToggleMonitoringState)
{
    monitor->start();
    EXPECT_TRUE(monitor->is_monitoring());

    monitor->stop();
    EXPECT_FALSE(monitor->is_monitoring());

    // 重入安全：重复 start/stop 幂等
    monitor->start();
    monitor->start();
    EXPECT_TRUE(monitor->is_monitoring());
    monitor->stop();
    monitor->stop();
    EXPECT_FALSE(monitor->is_monitoring());
}

TEST_F(ClipboardMonitorTest, SetEnabledTogglesMonitoring)
{
    monitor->set_enabled(true);
    EXPECT_TRUE(monitor->is_monitoring());

    monitor->set_enabled(false);
    EXPECT_FALSE(monitor->is_monitoring());

    // 重复置位幂等
    monitor->set_enabled(false);
    EXPECT_FALSE(monitor->is_monitoring());
}

// ---------- 检测语义 ----------

TEST_F(ClipboardMonitorTest, UrlInClipboardEmitsAndPopulatesLastUrl)
{
    monitor->start();

    const QString url = unique_url("detect");
    QGuiApplication::clipboard()->setText(url);
    EXPECT_TRUE(spin_until([&] { return emissions == 1; })) << "URL 到达剪贴板必须触发一次 url_detected";

    const auto& info = monitor->last_url();
    EXPECT_TRUE(info.is_valid);
    EXPECT_EQ(info.protocol, falcon::desktop::UrlProtocol::HTTPS);
    EXPECT_EQ(info.decoded_url.toStdString(), url.toStdString());
    EXPECT_EQ(info.file_name.toStdString(), "file.zip");
}

TEST_F(ClipboardMonitorTest, DuplicateClipboardTextIgnored)
{
    // 小间隔定时器路径独立钉住去重：changed 路径与轮询路径读到同一文本
    // 都不得重复派发（40ms 轮询 × 300ms 窗口 ≥ 7 次读取）
    monitor->set_detection_delay(40);
    monitor->start();

    const QString url = unique_url("dedup");
    QGuiApplication::clipboard()->setText(url);
    EXPECT_TRUE(spin_until([&] { return emissions == 1; }));

    pump_events(300); // 定时器反复读同一文本
    EXPECT_EQ(emissions, 1) << "同一文本重复读取不得重复派发";

    // 再次显式置同一文本（changed 再触发）同样被去重
    QGuiApplication::clipboard()->setText(url);
    pump_events(120);
    EXPECT_EQ(emissions, 1);
}

TEST_F(ClipboardMonitorTest, NonUrlTextIgnored)
{
    monitor->set_detection_delay(40);
    monitor->start();

    // 纯文本不含任何 URL（contains_url 对嵌入 URL 的文本会命中，须用纯文本）
    QGuiApplication::clipboard()->setText(QString("普通文本，不是链接 %1").arg(QChar(0x65E5)));
    pump_events(300);
    EXPECT_EQ(emissions, 0);
    EXPECT_FALSE(monitor->last_url().is_valid);
}

TEST_F(ClipboardMonitorTest, EmptyTextAfterDetectionIgnored)
{
    monitor->start();

    QGuiApplication::clipboard()->setText(unique_url("before-empty"));
    EXPECT_TRUE(spin_until([&] { return emissions == 1; }));

    QGuiApplication::clipboard()->setText("");
    pump_events(200);
    EXPECT_EQ(emissions, 1) << "空文本不得派发";
}

TEST_F(ClipboardMonitorTest, StartSeedsBaselineExistingContentNotDetected)
{
    // start() 以当前剪贴板内容做去重基线：开启监听瞬间已存在的旧内容
    // 不得立即弹窗（主流下载器语义——只对监听期间**新复制**的内容反应；
    // 旧实现清空基线 → 默认开启监控后每次启动都弹旧链接）
    QGuiApplication::clipboard()->setText(unique_url("stale"));
    monitor->set_detection_delay(40);
    monitor->start();

    pump_events(300);
    EXPECT_EQ(emissions, 0) << "开启监听时既有的剪贴板内容不得派发";

    // 监听期间新复制的内容照常检测
    QGuiApplication::clipboard()->setText(unique_url("fresh"));
    EXPECT_TRUE(spin_until([&] { return emissions == 1; })) << "监听期间新复制的链接必须派发";
}

TEST_F(ClipboardMonitorTest, RestartWithUnchangedClipboardDoesNotRedetect)
{
    // stop→start 基线重播种：剪贴板内容未变化时重启监听不得把旧内容
    // 再派发一次（与 StartSeedsBaseline 同一语义；小间隔轮询路径独立
    // 验证，不依赖 changed 信号的平台差异）
    monitor->set_detection_delay(40);
    monitor->start();
    const QString url = unique_url("restart");
    QGuiApplication::clipboard()->setText(url);
    EXPECT_TRUE(spin_until([&] { return emissions == 1; }));

    monitor->stop();
    monitor->start();
    pump_events(300);
    EXPECT_EQ(emissions, 1) << "剪贴板未变化时重启监听不得重复派发";
}

TEST_F(ClipboardMonitorTest, StoppedMonitorIgnoresClipboardChanges)
{
    monitor->start();
    monitor->stop();

    QGuiApplication::clipboard()->setText(unique_url("after-stop"));
    pump_events(300); // changed 直连与轮询定时器（已停）都不触发
    EXPECT_EQ(emissions, 0);
}

TEST_F(ClipboardMonitorTest, DetectionDelayChangeAppliesToSubsequentStart)
{
    // 延迟可在 start 前调整；changed 路径不受延迟影响，检测照常发生
    monitor->set_detection_delay(50);
    monitor->start();

    QGuiApplication::clipboard()->setText(unique_url("delay"));
    EXPECT_TRUE(spin_until([&] { return emissions == 1; }));
}

int main(int argc, char** argv)
{
    // xvfb 口径：优先环境给出的真实平台（xvfb-run 的 xcb / 桌面会话）；
    // 无显示环境回落 offscreen——QClipboard 在 offscreen 下进程内往返
    // 与 changed 信号均可用（探针实证），保证 headless ctest 可跑。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
