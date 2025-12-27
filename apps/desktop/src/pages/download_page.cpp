/**
 * @file download_page.cpp
 * @brief 下载管理页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "download_page.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QInputDialog>

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
    main_layout->setContentsMargins(10, 10, 10, 10);
    main_layout->setSpacing(10);

    // 创建工具栏
    auto* toolbar = create_toolbar();
    main_layout->addWidget(toolbar);

    // 创建标签页
    auto* tab_widget = create_tab_widget();
    main_layout->addWidget(tab_widget);

    // 设置样式
    setStyleSheet(R"(
        QTabWidget::pane {
            border: 1px solid #ddd;
        }
        QTableWidget {
            gridline-color: #eee;
            selection-background-color: #0078d4;
            selection-color: white;
        }
        QTableWidget::item {
            padding: 8px;
        }
        QPushButton {
            padding: 6px 12px;
            border-radius: 4px;
            border: 1px solid #0078d4;
            background-color: #0078d4;
            color: white;
        }
        QPushButton:hover {
            background-color: #005a9e;
        }
        QPushButton:pressed {
            background-color: #004578;
        }
        QPushButton:disabled {
            background-color: #ccc;
            border-color: #bbb;
        }
    )");
}

QWidget* DownloadPage::create_toolbar()
{
    auto* toolbar = new QWidget(this);
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // 新建任务按钮
    new_task_button_ = new QPushButton("+ 新建任务", toolbar);
    connect(new_task_button_, &QPushButton::clicked, this, &DownloadPage::add_new_task);
    layout->addWidget(new_task_button_);

    layout->addStretch();

    // 控制按钮
    pause_button_ = new QPushButton("暂停", toolbar);
    pause_button_->setEnabled(false);
    layout->addWidget(pause_button_);

    resume_button_ = new QPushButton("继续", toolbar);
    resume_button_->setEnabled(false);
    layout->addWidget(resume_button_);

    cancel_button_ = new QPushButton("取消", toolbar);
    cancel_button_->setEnabled(false);
    layout->addWidget(cancel_button_);

    delete_button_ = new QPushButton("删除", toolbar);
    delete_button_->setEnabled(false);
    layout->addWidget(delete_button_);

    clean_button_ = new QPushButton("清理已完成", toolbar);
    layout->addWidget(clean_button_);

    return toolbar;
}

QWidget* DownloadPage::create_tab_widget()
{
    tab_widget_ = new QTabWidget(this);
    tab_widget_->setTabPosition(QTabWidget::North);
    tab_widget_->setTabShape(QTabWidget::Rounded);

    // 创建各个标签页
    create_downloading_tab();
    create_completed_tab();
    create_trash_tab();
    create_history_tab();

    return tab_widget_;
}

void DownloadPage::create_downloading_tab()
{
    downloading_table_ = new QTableWidget(this);
    downloading_table_->setColumnCount(7);
    downloading_table_->setHorizontalHeaderLabels({
        "文件名", "大小", "进度", "速度", "状态", "保存路径", "操作"
    });

    downloading_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloading_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    downloading_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloading_table_->horizontalHeader()->setStretchLastSection(false);

    // 设置列宽
    downloading_table_->setColumnWidth(0, 200);  // 文件名
    downloading_table_->setColumnWidth(1, 80);   // 大小
    downloading_table_->setColumnWidth(2, 120);  // 进度
    downloading_table_->setColumnWidth(3, 80);   // 速度
    downloading_table_->setColumnWidth(4, 80);   // 状态
    downloading_table_->setColumnWidth(5, 150);  // 保存路径
    downloading_table_->setColumnWidth(6, 100);  // 操作

    tab_widget_->addTab(downloading_table_, "正在下载");
}

void DownloadPage::create_completed_tab()
{
    completed_table_ = new QTableWidget(this);
    completed_table_->setColumnCount(6);
    completed_table_->setHorizontalHeaderLabels({
        "文件名", "大小", "完成时间", "保存路径", "文件类型", "操作"
    });

    completed_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    completed_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    completed_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    tab_widget_->addTab(completed_table_, "已完成");
}

void DownloadPage::create_trash_tab()
{
    trash_table_ = new QTableWidget(this);
    trash_table_->setColumnCount(5);
    trash_table_->setHorizontalHeaderLabels({
        "文件名", "大小", "删除时间", "原因", "操作"
    });

    trash_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    trash_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    trash_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    tab_widget_->addTab(trash_table_, "回收站");
}

void DownloadPage::create_history_tab()
{
    history_table_ = new QTableWidget(this);
    history_table_->setColumnCount(6);
    history_table_->setHorizontalHeaderLabels({
        "文件名", "大小", "开始时间", "完成时间", "状态", "操作"
    });

    history_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    history_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    history_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    tab_widget_->addTab(history_table_, "下载记录");
}

void DownloadPage::add_new_task()
{
    QString url = QInputDialog::getText(this, "新建下载任务",
                                       "请输入下载链接（支持 HTTP/HTTPS/Magnet）:",
                                       QLineEdit::Normal);

    if (!url.isEmpty()) {
        QString save_path = QFileDialog::getSaveFileName(
            this, "选择保存位置",
            QDir::homePath() + "/Downloads",
            "所有文件 (*.*)"
        );

        if (!save_path.isEmpty()) {
            // TODO: 调用 libfalcon 下载引擎
            // 暂时添加到表格作为示例
            int row = downloading_table_->rowCount();
            downloading_table_->insertRow(row);

            downloading_table_->setItem(row, 0, new QTableWidgetItem(QFileInfo(save_path).fileName()));
            downloading_table_->setItem(row, 1, new QTableWidgetItem("0 MB"));
            downloading_table_->setItem(row, 2, new QTableWidgetItem("0%"));
            downloading_table_->setItem(row, 3, new QTableWidgetItem("0 KB/s"));
            downloading_table_->setItem(row, 4, new QTableWidgetItem("等待中"));
            downloading_table_->setItem(row, 5, new QTableWidgetItem(save_path));
            downloading_table_->setItem(row, 6, new QTableWidgetItem("删除"));

            // 添加进度条
            auto* progress_bar = new QProgressBar(this);
            progress_bar->setRange(0, 100);
            progress_bar->setValue(0);
            downloading_table_->setCellWidget(row, 2, progress_bar);
        }
    }
}

void DownloadPage::update_task_status()
{
    // TODO: 定期更新下载状态
    // 从 libfalcon 获取实时进度
}

} // namespace falcon::desktop
