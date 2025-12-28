/**
 * @file add_download_dialog.hpp
 * @brief Add download dialog
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>

#include "../utils/url_detector.hpp"

namespace falcon::desktop {

/**
 * @brief Dialog for adding new download tasks
 *
 * Displays detected URL information and allows user to configure
 * download options before starting the download.
 */
class AddDownloadDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddDownloadDialog(const UrlInfo& url_info, QWidget* parent = nullptr);
    ~AddDownloadDialog() override = default;

    /**
     * @brief Get final download URL
     * @return Download URL
     */
    QString get_url() const;

    /**
     * @brief Get save path
     * @return Save directory path
     */
    QString get_save_path() const;

    /**
     * @brief Get file name
     * @return File name
     */
    QString get_file_name() const;

    /**
     * @brief Get number of concurrent connections
     * @return Connection count
     */
    int get_connections() const;

    /**
     * @brief Get selected user agent
     * @return User agent string
     */
    QString get_user_agent() const;

private slots:
    /**
     * @brief Browse for save directory
     */
    void browse_directory();

    /**
     * @brief Start download
     */
    void start_download();

    /**
     * @brief Cancel dialog
     */
    void cancel_dialog();

private:
    void setup_ui();
    QWidget* create_url_section_widget();
    QWidget* create_file_section_widget();
    QWidget* create_options_section_widget();
    QLayout* create_button_layout();

    // URL Info
    UrlInfo url_info_;

    // UI Components
    QLabel* url_label_;
    QLineEdit* url_edit_;
    QLabel* protocol_label_;
    QLineEdit* file_name_edit_;
    QLineEdit* save_path_edit_;
    QPushButton* browse_button_;
    QSpinBox* connections_spin_;
    QComboBox* user_agent_combo_;
    QPushButton* start_button_;
    QPushButton* cancel_button_;
};

} // namespace falcon::desktop
