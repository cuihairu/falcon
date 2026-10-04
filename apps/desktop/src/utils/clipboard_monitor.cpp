/**
 * @file clipboard_monitor.cpp
 * @brief Clipboard monitor implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "clipboard_monitor.hpp"

#include <QApplication>

namespace falcon::desktop {

//==============================================================================
// Constructor / Destructor
//==============================================================================

ClipboardMonitor::ClipboardMonitor(QClipboard* clipboard, QObject* parent)
    : QObject(parent)
    , clipboard_(clipboard)
    , check_timer_(new QTimer(this))
    , is_monitoring_(false)
    , detection_delay_(1000) // Default 1 second delay
{
    // Connect timer to check slot
    connect(check_timer_, &QTimer::timeout, this, &ClipboardMonitor::check_clipboard);

    // Connect clipboard changed signal (Qt will call this when clipboard changes)
    if (clipboard_) {
        connect(clipboard_, &QClipboard::changed, this, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard) {
                check_clipboard();
            }
        });
    }
}

//==============================================================================
// Public Methods
//==============================================================================

void ClipboardMonitor::start()
{
    if (!is_monitoring_) {
        is_monitoring_ = true;
        // 去重基线 = 开始监听那一刻的剪贴板内容：只对**监听期间新复制**
        // 的内容弹窗（主流下载器语义）。清空基线的旧实现会对开启瞬间
        // 既有的剪贴板内容立即弹窗——默认开启监控后变成每次启动都弹
        // 旧链接。
        last_clipboard_text_ = clipboard_ ? clipboard_->text() : QString();
        check_timer_->start(detection_delay_);
    }
}

void ClipboardMonitor::stop()
{
    if (is_monitoring_) {
        is_monitoring_ = false;
        check_timer_->stop();
        last_clipboard_text_.clear();
    }
}

void ClipboardMonitor::set_enabled(bool enabled)
{
    if (enabled) {
        start();
    } else {
        stop();
    }
}

//==============================================================================
// Private Slots
//==============================================================================

void ClipboardMonitor::check_clipboard()
{
    if (!is_monitoring_ || !clipboard_) {
        return;
    }

    // Get current clipboard text
    QString current_text = clipboard_->text();

    // Check if text changed
    if (current_text == last_clipboard_text_) {
        return;
    }

    last_clipboard_text_ = current_text;
    process_text(current_text);
}

//==============================================================================
// Private Methods
//==============================================================================

void ClipboardMonitor::process_text(const QString& text)
{
    if (text.isEmpty()) {
        return;
    }

    // Check if text contains a supported URL
    if (!UrlDetector::contains_url(text)) {
        return;
    }

    // Parse the URL
    UrlInfo url_info = UrlDetector::parse_url(text);

    if (url_info.is_valid) {
        last_url_ = url_info;
        emit url_detected(url_info);
    }
}

} // namespace falcon::desktop
