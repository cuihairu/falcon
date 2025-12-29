/**
 * @file download_page.cpp
 * @brief Download Management Page Implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "download_page.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QInputDialog>
#include <QProgressBar>
#include <QStyle>
#include <falcon/event_listener.hpp>

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
    , refresh_timer_(nullptr)
{
    setup_ui();

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(500);
    connect(refresh_timer_, &QTimer::timeout, this, &DownloadPage::refresh_engine_tasks);
    refresh_timer_->start();
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
    auto* title_label = new QLabel(tr("Downloads"), this);
    auto title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
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
    layout->setSpacing(8);

    // Primary Action: New Task
    new_task_button_ = new QPushButton(tr("New Task"), toolbar);
    new_task_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
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

    auto create_action_btn = [this, actions_layout](QPushButton*& btn, QStyle::StandardPixmap icon, const QString& tooltip) {
        btn = new QPushButton(this);
        btn->setEnabled(false); // Default disabled until selection
        btn->setIcon(style()->standardIcon(icon));
        btn->setFlat(true);
        btn->setToolTip(tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(32, 32);
        actions_layout->addWidget(btn);
    };

    create_action_btn(pause_button_, QStyle::SP_MediaPause, tr("Pause"));
    create_action_btn(resume_button_, QStyle::SP_MediaPlay, tr("Resume"));
    create_action_btn(cancel_button_, QStyle::SP_BrowserStop, tr("Cancel"));
    
    // Separator line
    auto* line = new QFrame(actions_container);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setFixedHeight(20);
    actions_layout->addWidget(line);

    create_action_btn(delete_button_, QStyle::SP_TrashIcon, tr("Delete"));

    // Clean button is always enabled usually
    clean_button_ = new QPushButton(this);
    clean_button_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clean_button_->setFlat(true);
    clean_button_->setToolTip(tr("Clear Completed"));
    clean_button_->setCursor(Qt::PointingHandCursor);
    clean_button_->setFixedSize(32, 32);
    actions_layout->addWidget(clean_button_);

    layout->addWidget(actions_container);

    return toolbar;
}

void DownloadPage::create_tab_widget()
{
    tab_widget_ = new QTabWidget(this);
    tab_widget_->setTabPosition(QTabWidget::North);
#ifdef Q_OS_MAC
    tab_widget_->setDocumentMode(true);
#else
    tab_widget_->setDocumentMode(false);
#endif

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
        tr("File Name"),
        tr("Size"),
        tr("Progress"),
        tr("Speed"),
        tr("Status"),
        tr("Save Path"),
        tr("Actions")
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

    // Column widths
    downloading_table_->setColumnWidth(0, 300); // Name
    downloading_table_->setColumnWidth(1, 100); // Size
    downloading_table_->setColumnWidth(2, 200); // Progress
    downloading_table_->setColumnWidth(3, 100); // Speed
    downloading_table_->setColumnWidth(4, 100); // Status
    downloading_table_->setColumnWidth(5, 250); // Path
    // Last section stretches

    tab_widget_->addTab(downloading_table_, tr("Downloading"));
}

void DownloadPage::create_completed_tab()
{
    completed_table_ = new QTableWidget(this);
    completed_table_->setColumnCount(6);
    completed_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Completed At"),
        tr("Save Path"),
        tr("Type"),
        tr("Actions")
    });

    completed_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    completed_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    completed_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    completed_table_->setShowGrid(false);
    completed_table_->verticalHeader()->setVisible(false);
    completed_table_->horizontalHeader()->setStretchLastSection(true);

    tab_widget_->addTab(completed_table_, tr("Completed"));
}

void DownloadPage::create_trash_tab()
{
    trash_table_ = new QTableWidget(this);
    trash_table_->setColumnCount(5);
    trash_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Deleted At"),
        tr("Reason"),
        tr("Actions")
    });

    trash_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    trash_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    trash_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trash_table_->setShowGrid(false);
    trash_table_->verticalHeader()->setVisible(false);
    trash_table_->horizontalHeader()->setStretchLastSection(true);

    tab_widget_->addTab(trash_table_, tr("Trash"));
}

void DownloadPage::create_history_tab()
{
    history_table_ = new QTableWidget(this);
    history_table_->setColumnCount(6);
    history_table_->setHorizontalHeaderLabels({
        tr("File Name"),
        tr("Size"),
        tr("Started At"),
        tr("Finished At"),
        tr("Status"),
        tr("Actions")
    });

    history_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    history_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    history_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history_table_->setShowGrid(false);
    history_table_->verticalHeader()->setVisible(false);
    history_table_->horizontalHeader()->setStretchLastSection(true);

    tab_widget_->addTab(history_table_, tr("History"));
}

void DownloadPage::add_new_task()
{
    QString url = QInputDialog::getText(this, tr("New Download Task"),
                                       tr("Enter download URL (HTTP/HTTPS/Magnet):"),
                                       QLineEdit::Normal);

    if (!url.isEmpty()) {
        QString save_path = QFileDialog::getSaveFileName(
            this, tr("Select Save Location"),
            QDir::homePath() + "/Downloads",
            tr("All Files (*.*)")
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
            set_item(4, tr("Waiting"));
            set_item(5, save_path);
            set_item(6, "...");

            // Add progress bar
            auto* progress_bar = new QProgressBar(this);
            progress_bar->setRange(0, 100);
            progress_bar->setValue(0);
            progress_bar->setTextVisible(false);
            downloading_table_->setCellWidget(row, 2, progress_bar);
        }
    }
}

void DownloadPage::update_task_status()
{
    // TODO: Update status from backend
}

QString DownloadPage::format_bytes(uint64_t bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QString::number(bytes) + " B";
    }
    return QString("%1 %2").arg(size, 0, 'f', 1).arg(units[unit]);
}

QString DownloadPage::format_speed(uint64_t bytes_per_second)
{
    if (bytes_per_second == 0) {
        return "0 B/s";
    }
    return format_bytes(bytes_per_second) + "/s";
}

void DownloadPage::add_engine_task(const falcon::DownloadTask::Ptr& task)
{
    if (!task) {
        return;
    }

    const qulonglong id = static_cast<qulonglong>(task->id());
    if (row_by_task_id_.contains(id)) {
        return;
    }

    engine_tasks_.push_back(task);

    const int row = downloading_table_->rowCount();
    downloading_table_->insertRow(row);
    row_by_task_id_.insert(id, row);

    const QString filename = !task->options().output_filename.empty()
        ? QString::fromStdString(task->options().output_filename)
        : QString::fromStdString(task->file_info().filename);
    const QString save_dir = QString::fromStdString(task->options().output_directory);

    auto set_item = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        downloading_table_->setItem(row, col, item);
    };

    set_item(0, filename.isEmpty() ? tr("(unknown)") : filename);
    set_item(1, "-");
    set_item(3, "0 B/s");
    set_item(4, QString::fromUtf8(falcon::to_string(task->status())));
    set_item(5, save_dir);
    set_item(6, "...");

    auto* progress_bar = new QProgressBar(this);
    progress_bar->setRange(0, 100);
    progress_bar->setValue(0);
    progress_bar->setTextVisible(false);
    downloading_table_->setCellWidget(row, 2, progress_bar);
}

void DownloadPage::refresh_engine_tasks()
{
    for (const auto& task : engine_tasks_) {
        if (!task) {
            continue;
        }

        const qulonglong id = static_cast<qulonglong>(task->id());
        auto it = row_by_task_id_.find(id);
        if (it == row_by_task_id_.end()) {
            continue;
        }

        const int row = it.value();

        const uint64_t total = static_cast<uint64_t>(task->total_bytes());
        const uint64_t speed = static_cast<uint64_t>(task->speed());
        const int pct = static_cast<int>(task->progress() * 100.0f);

        if (auto* size_item = downloading_table_->item(row, 1)) {
            size_item->setText(total > 0 ? format_bytes(total) : "-");
        }
        if (auto* speed_item = downloading_table_->item(row, 3)) {
            speed_item->setText(format_speed(speed));
        }
        if (auto* status_item = downloading_table_->item(row, 4)) {
            status_item->setText(QString::fromUtf8(falcon::to_string(task->status())));
        }
        if (auto* widget = downloading_table_->cellWidget(row, 2)) {
            if (auto* bar = qobject_cast<QProgressBar*>(widget)) {
                bar->setValue(std::max(0, std::min(100, pct)));
            }
        }
    }
}

} // namespace falcon::desktop
