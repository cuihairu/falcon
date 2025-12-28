/**
 * @file styles.hpp
 * @brief Global UI Styles - Modern Clean Design
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QString>

namespace falcon::desktop {

// Color Palette
// Primary: #6200EE (Deep Purple) -> #5c6bc0 (Indigo)
// Secondary: #03DAC6 (Teal)
// Background: #F5F7FA
// Surface: #FFFFFF
// Text: #2D3748
// Text Secondary: #718096
// Border: #E2E8F0

/**
 * @brief Get global application stylesheet
 */
inline QString get_global_stylesheet()
{
    return R"(
        /* Global Reset */
        QWidget {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            font-size: 13px;
            color: #2D3748;
            outline: none;
        }

        /* Window Background */
        QMainWindow, QDialog {
            background-color: #F5F7FA;
        }

        /* Tooltips */
        QToolTip {
            background-color: #2D3748;
            color: #FFFFFF;
            border: none;
            padding: 5px;
            border-radius: 4px;
        }

        /* Modern Scrollbar */
        QScrollBar:vertical {
            border: none;
            background: transparent;
            width: 8px;
            margin: 0px;
        }

        QScrollBar::handle:vertical {
            background: #CBD5E0;
            min-height: 40px;
            border-radius: 4px;
        }

        QScrollBar::handle:vertical:hover {
            background: #A0AEC0;
        }

        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }

        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }

        QScrollBar:horizontal {
            border: none;
            background: transparent;
            height: 8px;
            margin: 0px;
        }

        QScrollBar::handle:horizontal {
            background: #CBD5E0;
            min-width: 40px;
            border-radius: 4px;
        }

        QScrollBar::handle:horizontal:hover {
            background: #A0AEC0;
        }

        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
    )";
}

/**
 * @brief Get button stylesheet
 */
inline QString get_button_stylesheet(bool primary = true)
{
    if (primary) {
        return R"(
            QPushButton {
                background-color: #5C6BC0;
                color: white;
                border: none;
                border-radius: 6px;
                padding: 8px 16px;
                font-weight: 600;
                font-size: 13px;
            }

            QPushButton:hover {
                background-color: #4F5EAA;
            }

            QPushButton:pressed {
                background-color: #3F51B5;
            }

            QPushButton:disabled {
                background-color: #E2E8F0;
                color: #A0AEC0;
            }
        )";
    } else {
        return R"(
            QPushButton {
                background-color: #FFFFFF;
                color: #4A5568;
                border: 1px solid #E2E8F0;
                border-radius: 6px;
                padding: 8px 16px;
                font-weight: 500;
                font-size: 13px;
            }

            QPushButton:hover {
                background-color: #F7FAFC;
                border-color: #CBD5E0;
                color: #2D3748;
            }

            QPushButton:pressed {
                background-color: #EDF2F7;
            }

            QPushButton:disabled {
                background-color: #F7FAFC;
                color: #CBD5E0;
                border-color: #EDF2F7;
            }
        )";
    }
}

/**
 * @brief Get icon button stylesheet (for toolbar actions)
 */
inline QString get_icon_button_stylesheet()
{
    return R"(
        QPushButton {
            background-color: transparent;
            color: #718096;
            border: none;
            border-radius: 4px;
            padding: 6px;
            font-size: 16px;
        }

        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.04);
            color: #4A5568;
        }

        QPushButton:pressed {
            background-color: rgba(0, 0, 0, 0.08);
            color: #2D3748;
        }

        QPushButton:disabled {
            color: #CBD5E0;
        }
    )";
}

/**
 * @brief Get input field stylesheet
 */
inline QString get_input_stylesheet()
{
    return R"(
        QLineEdit {
            border: 1px solid #E2E8F0;
            border-radius: 6px;
            padding: 8px 12px;
            background: #FFFFFF;
            color: #2D3748;
            selection-background-color: #5C6BC0;
            selection-color: white;
        }

        QLineEdit:focus {
            border: 1px solid #5C6BC0;
            background: #FFFFFF;
        }

        QLineEdit:hover {
            border-color: #CBD5E0;
        }

        QLineEdit:disabled {
            background-color: #F7FAFC;
            color: #A0AEC0;
        }
    )";
}

/**
 * @brief Get combobox stylesheet
 */
inline QString get_combo_stylesheet()
{
    return R"(
        QComboBox {
            border: 1px solid #E2E8F0;
            border-radius: 6px;
            padding: 8px 12px;
            background: #FFFFFF;
            color: #2D3748;
            min-height: 20px;
        }

        QComboBox:focus {
            border: 1px solid #5C6BC0;
        }

        QComboBox:hover {
            border-color: #CBD5E0;
        }

        QComboBox::drop-down {
            border: none;
            width: 24px;
        }

        QComboBox::down-arrow {
            image: none; /* Can be replaced with a custom icon if available */
            border-left: 5px solid transparent;
            border-right: 5px solid transparent;
            border-top: 5px solid #718096;
            margin-right: 8px;
        }
        
        QComboBox QAbstractItemView {
            border: 1px solid #E2E8F0;
            border-radius: 6px;
            background: #FFFFFF;
            selection-background-color: #EDF2F7;
            selection-color: #2D3748;
            outline: none;
            padding: 4px;
        }
    )";
}

/**
 * @brief Get table stylesheet
 */
inline QString get_table_stylesheet()
{
    return R"(
        QTableWidget {
            background-color: #FFFFFF;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
            gridline-color: transparent; /* Cleaner look without grid lines */
            selection-background-color: #EDF2F7;
            selection-color: #2D3748;
        }

        QTableWidget::item {
            padding: 10px 12px;
            border-bottom: 1px solid #EDF2F7;
            color: #4A5568;
        }

        QTableWidget::item:selected {
            background-color: #EDF2F7;
            color: #2D3748;
        }

        QHeaderView::section {
            background-color: #F8FAFC;
            color: #718096;
            padding: 12px;
            border: none;
            border-bottom: 2px solid #E2E8F0;
            font-weight: 600;
            font-size: 12px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        
        QHeaderView {
            background-color: transparent;
            border: none;
        }

        QTableCornerButton::section {
            background-color: #F8FAFC;
            border: none;
            border-bottom: 2px solid #E2E8F0;
        }
    )";
}

/**
 * @brief Get tab widget stylesheet
 */
inline QString get_tab_stylesheet()
{
    return R"(
        QTabWidget::pane {
            border: none;
            background: transparent;
        }

        QTabWidget::tab-bar {
            alignment: left;
        }

        QTabBar::tab {
            background: transparent;
            color: #718096;
            padding: 10px 20px;
            margin-right: 4px;
            border-bottom: 2px solid transparent;
            font-weight: 600;
            font-size: 14px;
        }

        QTabBar::tab:selected {
            color: #5C6BC0;
            border-bottom: 2px solid #5C6BC0;
        }

        QTabBar::tab:hover:!selected {
            color: #4A5568;
            border-bottom-color: #CBD5E0;
        }
    )";
}

/**
 * @brief Get progress bar stylesheet
 */
inline QString get_progress_stylesheet()
{
    return R"(
        QProgressBar {
            border: none;
            border-radius: 4px;
            background-color: #EDF2F7;
            text-align: center;
            color: transparent; /* Hide text, or set a contrasting color */
            min-height: 8px;
            max-height: 8px;
        }

        QProgressBar::chunk {
            background-color: #5C6BC0;
            border-radius: 4px;
        }
    )";
}

/**
 * @brief Get card-like container stylesheet
 */
inline QString get_card_stylesheet()
{
    return R"(
        QWidget {
            background-color: #FFFFFF;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
        }
    )";
}

} // namespace falcon::desktop