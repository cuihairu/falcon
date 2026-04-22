/**
 * @file settings_page.hpp
 * @brief Settings page for application configuration
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QWidget>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>

namespace falcon::desktop {

/**
 * @brief Settings page
 *
 * Provides configuration options for the application including:
 * - Clipboard monitoring
 * - Download settings
 * - Connection settings
 * - Notification settings
 */
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);
    ~SettingsPage() override = default;

    void set_clipboard_monitoring_enabled(bool enabled);
    void set_clipboard_detection_delay(int delay_ms);
    void set_default_download_dir(const QString& path);
    void set_max_concurrent_downloads(int count);
    void set_default_connections(int count);
    void set_notifications_enabled(bool enabled);
    void set_sound_notifications_enabled(bool enabled);

    /**
     * @brief Get clipboard monitoring enabled state
     * @return true if enabled
     */
    bool is_clipboard_monitoring_enabled() const;

    /**
     * @brief Get clipboard detection delay
     * @return Delay in milliseconds
     */
    int get_clipboard_detection_delay() const;

    /**
     * @brief Get default download directory
     * @return Directory path
     */
    QString get_default_download_dir() const;

    /**
     * @brief Get maximum concurrent downloads
     * @return Maximum number
     */
    int get_max_concurrent_downloads() const;

    /**
     * @brief Get default connection count per download
     * @return Connection count
     */
    int get_default_connections() const;

    int get_connection_timeout() const;

    int get_retry_count() const;

    /**
     * @brief Get whether to show notifications
     * @return true if notifications enabled
     */
    bool is_notifications_enabled() const;

    bool is_sound_notifications_enabled() const;

signals:
    /**
     * @brief Signal emitted when settings are changed
     */
    void settings_changed();

    /**
     * @brief Signal emitted when clipboard monitoring setting is toggled
     * @param enabled true if monitoring should be enabled
     */
    void clipboard_monitoring_toggled(bool enabled);

    /**
     * @brief Signal emitted when theme toggle is requested
     */
    void theme_toggle_requested();

private slots:
    /**
     * @brief Browse for default download directory
     */
    void browse_download_dir();

    /**
     * @brief Reset all settings to defaults
     */
    void reset_to_defaults();

    /**
     * @brief Apply settings
     */
    void apply_settings();

private:
    void setup_ui();
    QWidget* create_clipboard_section_widget();
    QWidget* create_download_section_widget();
    QWidget* create_connection_section_widget();
    QWidget* create_notification_section_widget();
    QWidget* create_appearance_section_widget();
    QLayout* create_action_buttons_layout();

    void on_theme_button_clicked();

    // Clipboard settings
    QCheckBox* clipboard_monitoring_checkbox_;
    QSpinBox* clipboard_delay_spin_;

    // Download settings
    QLineEdit* download_dir_edit_;
    QSpinBox* max_downloads_spin_;

    // Connection settings
    QSpinBox* default_connections_spin_;
    QSpinBox* connection_timeout_spin_;
    QSpinBox* retry_count_spin_;

    // Notification settings
    QCheckBox* notifications_checkbox_;
    QCheckBox* sound_notification_checkbox_;

    // Appearance settings
    QLabel* current_theme_label_;
    QPushButton* theme_toggle_button_;

    // Action buttons
    QPushButton* apply_button_;
    QPushButton* reset_button_;
};

} // namespace falcon::desktop
