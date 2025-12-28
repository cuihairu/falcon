/**
 * @file clipboard_monitor.hpp
 * @brief Clipboard monitoring for download URLs
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QObject>
#include <QClipboard>
#include <QTimer>
#include <QString>

#include "url_detector.hpp"

namespace falcon::desktop {

/**
 * @brief Clipboard monitor for detecting download URLs
 *
 * Monitors the clipboard for supported download URLs and emits
 * a signal when a valid URL is detected.
 */
class ClipboardMonitor : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardMonitor(QClipboard* clipboard, QObject* parent = nullptr);
    ~ClipboardMonitor() override = default;

    /**
     * @brief Start monitoring clipboard
     */
    void start();

    /**
     * @brief Stop monitoring clipboard
     */
    void stop();

    /**
     * @brief Check if monitoring is active
     * @return true if monitoring
     */
    bool is_monitoring() const { return is_monitoring_; }

    /**
     * @brief Set monitoring enabled
     * @param enabled Enable or disable monitoring
     */
    void set_enabled(bool enabled);

    /**
     * @brief Get last detected URL
     * @return Last detected URL info
     */
    const UrlInfo& last_url() const { return last_url_; }

    /**
     * @brief Set minimum delay between detections (milliseconds)
     * @param delay_ms Delay in milliseconds
     */
    void set_detection_delay(int delay_ms) { detection_delay_ = delay_ms; }

signals:
    /**
     * @brief Signal emitted when a supported URL is detected
     * @param url_info Detected URL information
     */
    void url_detected(const UrlInfo& url_info);

private slots:
    /**
     * @brief Check clipboard for changes
     */
    void check_clipboard();

private:
    /**
     * @brief Process clipboard text
     * @param text Clipboard text
     */
    void process_text(const QString& text);

    QClipboard* clipboard_;
    QTimer* check_timer_;
    QString last_clipboard_text_;
    UrlInfo last_url_;
    bool is_monitoring_;
    int detection_delay_;
};

} // namespace falcon::desktop
