/**
 * @file download_page.cpp
 * @brief Download Management Page Implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "download_page.hpp"
#include "../styles.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QInputDialog>
#include <QProgressBar>

namespace falcon::desktop {

DownloadPage::DownloadPage(QWidget* parent)
    : QWidget(parent)
    , tab_widget_(nullptr)
    , downloading_table_(nullptr)
    , completed_table_(nullptr)
    , trash_table_(nullptr)
    , history_table_(nullptr)
    , new_task_button_(nullptr)
    , pause_button_(nullptr)
    , resume_button_(nullptr)
    , cancel_button_(nullptr)
    , delete_button_(nullptr)
    , clean_button_(nullptr)
{
    setup_ui();
}

DownloadPage::~DownloadPage() = default;

void DownloadPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(32, 32, 32, 32);
    main_layout->setSpacing(24);

    // Header Area
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(16);

    // Page Title
    auto* title_label = new QLabel("Downloads", this);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 28px;
            font-weight: 700;
            color: #2D3748;
            border: none;
        }
    )");
    header_layout->addWidget(title_label);
    
    header_layout->addStretch();
    main_layout->addLayout(header_layout);

    // Toolbar
    auto* toolbar = create_toolbar();
    main_layout->addWidget(toolbar);

    // Content Area (Tabs)
    create_tab_widget();
    main_layout->addWidget(tab_widget_);
}

QWidget* DownloadPage::create_toolbar()
{
    auto* toolbar = new QWidget(this);
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // Primary Action: New Task
    new_task_button_ = new QPushButton("➕ New Task", toolbar);
    new_task_button_->setStyleSheet(get_button_stylesheet(true));
    new_task_button_->setCursor(Qt::PointingHandCursor);
    new_task_button_->setMinimumHeight(36);
    connect(new_task_button_, &QPushButton::clicked, this, &DownloadPage::add_new_task);
    layout->addWidget(new_task_button_);

    // Spacer
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(spacer);

    // Secondary Actions Group
    auto* actions_container = new QWidget(toolbar);
    auto* actions_layout = new QHBoxLayout(actions_container);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(4);

    auto create_action_btn = [this, actions_layout](QPushButton*& btn, const QString& text, const QString& tooltip) {
        btn = new QPushButton(text, this);
        btn->setEnabled(false); // Default disabled until selection
        btn->setStyleSheet(get_icon_button_stylesheet());
        btn->setToolTip(tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(36, 36);
        actions_layout->addWidget(btn);
    };

    create_action_btn(pause_button_, "⏸", "Pause");
    create_action_btn(resume_button_, "▶", "Resume");
    create_action_btn(cancel_button_, "✕", "Cancel");
    
    // Separator line
    auto* line = new QFrame(actions_container);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setStyleSheet("background-color: #E2E8F0; width: 1px;");
    line->setFixedHeight(20);
    actions_layout->addWidget(line);

    create_action_btn(delete_button_, "🗑", "Delete");

    // Clean button is always enabled usually
    clean_button_ = new QPushButton("🧹", this);
    clean_button_->setStyleSheet(get_icon_button_stylesheet());
    clean_button_->setToolTip("Clear Completed");
    clean_button_->setCursor(Qt::PointingHandCursor);
    clean_button_->setFixedSize(36, 36);
    actions_layout->addWidget(clean_button_);

    layout->addWidget(actions_container);

    return toolbar;
}

void DownloadPage::create_tab_widget()
{
    tab_widget_ = new QTabWidget(this);
    tab_widget_->setTabPosition(QTabWidget::North);
    tab_widget_->setDocumentMode(true); // Removes the border around the tab content
    tab_widget_->setStyleSheet(get_tab_stylesheet());

    // Create Tabs
    create_downloading_tab();
    create_completed_tab();
    create_trash_tab();
    create_history_tab();
}

void DownloadPage::create_downloading_tab()
{
    downloading_table_ = new QTableWidget(this);
    downloading_table_->setColumnCount(7);
    downloading_table_->setHorizontalHeaderLabels({
        "File Name", "Size", "Progress", "Speed", "Status", "Path", "Actions"
    });

    // Table settings
    downloading_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloading_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    downloading_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloading_table_->setShowGrid(false);
    downloading_table_->setAlternatingRowColors(false); // Clean look
    downloading_table_->verticalHeader()->setVisible(false);
    downloading_table_->horizontalHeader()->setStretchLastSection(true);
    downloading_table_->horizontalHeader()->setHighlightSections(false);
    downloading_table_->setStyleSheet(get_table_stylesheet());

    // Column widths
    downloading_table_->setColumnWidth(0, 300); // Name
    downloading_table_->setColumnWidth(1, 100); // Size
    downloading_table_->setColumnWidth(2, 200); // Progress
    downloading_table_->setColumnWidth(3, 100); // Speed
    downloading_table_->setColumnWidth(4, 100); // Status
    downloading_table_->setColumnWidth(5, 250); // Path
    // Last section stretches

    tab_widget_->addTab(downloading_table_, "Downloading");
}

void DownloadPage::create_completed_tab()
{
    completed_table_ = new QTableWidget(this);
    completed_table_->setColumnCount(6);
    completed_table_->setHorizontalHeaderLabels({
        "File Name", "Size", "Date", "Path", "Type", "Actions"
    });

    completed_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    completed_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    completed_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    completed_table_->setShowGrid(false);
    completed_table_->verticalHeader()->setVisible(false);
    completed_table_->horizontalHeader()->setStretchLastSection(true);
    completed_table_->setStyleSheet(get_table_stylesheet());

    tab_widget_->addTab(completed_table_, "Completed");
}

void DownloadPage::create_trash_tab()
{
    trash_table_ = new QTableWidget(this);
    trash_table_->setColumnCount(5);
    trash_table_->setHorizontalHeaderLabels({
        "File Name", "Size", "Deleted At", "Reason", "Actions"
    });

    trash_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    trash_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    trash_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trash_table_->setShowGrid(false);
    trash_table_->verticalHeader()->setVisible(false);
    trash_table_->horizontalHeader()->setStretchLastSection(true);
    trash_table_->setStyleSheet(get_table_stylesheet());

    tab_widget_->addTab(trash_table_, "Trash");
}

void DownloadPage::create_history_tab()
{
    history_table_ = new QTableWidget(this);
    history_table_->setColumnCount(6);
    history_table_->setHorizontalHeaderLabels({
        "File Name", "Size", "Started", "Finished", "Status", "Actions"
    });

    history_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    history_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    history_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history_table_->setShowGrid(false);
    history_table_->verticalHeader()->setVisible(false);
    history_table_->horizontalHeader()->setStretchLastSection(true);
    history_table_->setStyleSheet(get_table_stylesheet());

    tab_widget_->addTab(history_table_, "History");
}

void DownloadPage::add_new_task()
{
    QString url = QInputDialog::getText(this, "New Download Task",
                                       "Enter download URL (HTTP/HTTPS/Magnet):",
                                       QLineEdit::Normal);

    if (!url.isEmpty()) {
        QString save_path = QFileDialog::getSaveFileName(
            this, "Select Save Location",
            QDir::homePath() + "/Downloads",
            "All Files (*.*)"
        );

        if (!save_path.isEmpty()) {
            int row = downloading_table_->rowCount();
            downloading_table_->insertRow(row);

            // Helper to set item
            auto set_item = [&](int col, QString text) {
                auto* item = new QTableWidgetItem(text);
                item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                downloading_table_->setItem(row, col, item);
            };

            set_item(0, QFileInfo(save_path).fileName());
            set_item(1, "0 MB");
            // Col 2 is progress bar
            set_item(3, "0 KB/s");
            set_item(4, "Waiting");
            set_item(5, save_path);
            set_item(6, "...");

            // Add progress bar
            auto* progress_bar = new QProgressBar(this);
            progress_bar->setRange(0, 100);
            progress_bar->setValue(0);
            progress_bar->setStyleSheet(get_progress_stylesheet());
            downloading_table_->setCellWidget(row, 2, progress_bar);
        }
    }
}

void DownloadPage::update_task_status()
{
    // TODO: Update status from backend
}

} // namespace falcon::desktop