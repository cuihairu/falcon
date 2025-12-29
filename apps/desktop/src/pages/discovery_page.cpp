/**
 * @file discovery_page.cpp
 * @brief 资源发现与搜索页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "discovery_page.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QMenu>
#include <QProgressBar>
#include <QApplication>
#include <QStyle>

namespace falcon::desktop {

DiscoveryPage::DiscoveryPage(QWidget* parent)
    : QWidget(parent)
    , search_bar_(nullptr)
    , filter_bar_(nullptr)
{
    setup_ui();
}

DiscoveryPage::~DiscoveryPage() = default;

void DiscoveryPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(16);

    // 页面标题
    auto* title_label = new QLabel(tr("Discovery"), this);
    auto title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    main_layout->addWidget(title_label);

    // 创建搜索栏
    search_bar_ = create_search_bar();
    main_layout->addWidget(search_bar_);

    // 创建过滤栏
    filter_bar_ = create_filter_bar();
    main_layout->addWidget(filter_bar_);

    // 创建结果表格
    create_results_table();
    main_layout->addWidget(results_table_);

    // 创建状态栏
    auto* status_bar = create_status_bar();
    main_layout->addWidget(status_bar);
}

QWidget* DiscoveryPage::create_search_bar()
{
    auto* search_bar = new QWidget(this);
    auto* layout = new QHBoxLayout(search_bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // 搜索类型选择
    search_type_combo_ = new QComboBox(search_bar);
    search_type_combo_->addItem(tr("Magnet"), "magnet");
    search_type_combo_->addItem(tr("HTTP"), "http");
    search_type_combo_->addItem(tr("Cloud"), "cloud");
    search_type_combo_->addItem(tr("FTP"), "ftp");
    layout->addWidget(search_type_combo_);

    // 搜索输入框
    search_input_ = new QLineEdit(search_bar);
    search_input_->setPlaceholderText(tr("Enter keywords..."));
    search_input_->setMinimumWidth(400);
    layout->addWidget(search_input_, 1);

    // 搜索按钮
    search_button_ = new QPushButton(tr("Search"), search_bar);
    search_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    search_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(search_button_);

    // 清空按钮
    clear_button_ = new QPushButton(tr("Clear"), search_bar);
    clear_button_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clear_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(clear_button_);

    // 排序方式
    sort_combo_ = new QComboBox(search_bar);
    sort_combo_->addItem(tr("Relevance"), "relevance");
    sort_combo_->addItem(tr("Size"), "size");
    sort_combo_->addItem(tr("Date"), "date");
    sort_combo_->addItem(tr("Seeders"), "seeders");
    layout->addWidget(sort_combo_);

    // 连接信号
    connect(search_button_, &QPushButton::clicked, this, &DiscoveryPage::perform_search);
    connect(clear_button_, &QPushButton::clicked, this, &DiscoveryPage::clear_search);
    connect(search_input_, &QLineEdit::returnPressed, this, &DiscoveryPage::perform_search);
    connect(search_type_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiscoveryPage::on_search_type_changed);

    return search_bar;
}

QWidget* DiscoveryPage::create_filter_bar()
{
    auto* filter_bar = new QWidget(this);
    auto* layout = new QHBoxLayout(filter_bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // 分类过滤
    auto* category_label = new QLabel(tr("Category:"), filter_bar);
    category_filter_ = new QComboBox(filter_bar);
    category_filter_->addItem(tr("All"), "all");
    category_filter_->addItem(tr("Video"), "video");
    category_filter_->addItem(tr("Audio"), "audio");
    category_filter_->addItem(tr("Document"), "document");
    category_filter_->addItem(tr("Software"), "software");
    category_filter_->addItem(tr("Image"), "image");
    layout->addWidget(category_label);
    layout->addWidget(category_filter_);

    // 大小过滤
    auto* size_label = new QLabel(tr("Size:"), filter_bar);
    layout->addWidget(size_label);

    min_size_edit_ = new QLineEdit(filter_bar);
    min_size_edit_->setPlaceholderText(tr("Min"));
    min_size_edit_->setMaximumWidth(80);
    layout->addWidget(min_size_edit_);

    auto* to_label = new QLabel("-", filter_bar);
    layout->addWidget(to_label);

    max_size_edit_ = new QLineEdit(filter_bar);
    max_size_edit_->setPlaceholderText(tr("Max"));
    max_size_edit_->setMaximumWidth(80);
    layout->addWidget(max_size_edit_);

    size_filter_ = new QComboBox(filter_bar);
    size_filter_->addItem(tr("MB"), "mb");
    size_filter_->addItem(tr("GB"), "gb");
    layout->addWidget(size_filter_);

    layout->addStretch();

    return filter_bar;
}

void DiscoveryPage::create_results_table()
{
    results_table_ = new QTableWidget(this);
    results_table_->setColumnCount(7);
    results_table_->setHorizontalHeaderLabels({
        tr("Title"),
        tr("Size"),
        tr("Source"),
        tr("Type"),
        tr("Seeders"),
        tr("Leechers"),
        tr("Actions")
    });

    results_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_table_->horizontalHeader()->setStretchLastSection(false);
    results_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    results_table_->setAlternatingRowColors(true);

    // 设置列宽
    results_table_->setColumnWidth(0, 350);  // 标题
    results_table_->setColumnWidth(1, 100);  // 大小
    results_table_->setColumnWidth(2, 120);  // 来源
    results_table_->setColumnWidth(3, 80);   // 类型
    results_table_->setColumnWidth(4, 80);   // 种子数
    results_table_->setColumnWidth(5, 80);   // 下载中
    results_table_->setColumnWidth(6, 120);  // 操作

    // 连接信号
    connect(results_table_, &QTableWidget::cellDoubleClicked, this, &DiscoveryPage::show_item_details);
    connect(results_table_, &QTableWidget::customContextMenuRequested, this, &DiscoveryPage::show_context_menu);
}

QWidget* DiscoveryPage::create_status_bar()
{
    auto* status_bar = new QWidget(this);
    auto* layout = new QHBoxLayout(status_bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    status_label_ = new QLabel(tr("Ready"), status_bar);
    layout->addWidget(status_label_);

    layout->addStretch();

    result_count_label_ = new QLabel("", status_bar);
    layout->addWidget(result_count_label_);

    return status_bar;
}

void DiscoveryPage::perform_search()
{
    QString keyword = search_input_->text().trimmed();
    if (keyword.isEmpty()) {
        QMessageBox::warning(this, tr("Notice"), tr("Please enter keywords."));
        return;
    }

    settings_.search_type = search_type_combo_->currentData().toString();
    settings_.category = category_filter_->currentData().toString();
    settings_.sort_by = sort_combo_->currentData().toString();

    // 清空当前结果
    results_table_->setRowCount(0);
    current_results_.clear();

    status_label_->setText(tr("Searching..."));

    // 根据搜索类型执行不同的搜索
    if (settings_.search_type == "magnet") {
        search_magnet_links(keyword);
    } else if (settings_.search_type == "http") {
        search_http_resources(keyword);
    } else if (settings_.search_type == "cloud") {
        search_cloud_resources(keyword);
    } else if (settings_.search_type == "ftp") {
        search_ftp_resources(keyword);
    }
}

void DiscoveryPage::search_magnet_links(const QString& /*keyword*/)
{
    // TODO: 集成实际的磁力链接搜索API
    // 这里添加示例数据

    QList<SearchResultItem> results;
    results.append({
        tr("Sample Movie 2025 BluRay 1080p"),
        "magnet:?xt=urn:btih:example1",
        "4.2 GB",
        tr("Sample Site 1"),
        tr("Video"),
        "2025-12-27",
        1523,
        456
    });
    results.append({
        tr("Sample Package v2.0"),
        "magnet:?xt=urn:btih:example2",
        "850 MB",
        tr("Sample Site 2"),
        tr("Software"),
        "2025-12-26",
        892,
        234
    });

    display_results(results);
}

void DiscoveryPage::search_http_resources(const QString& /*keyword*/)
{
    // TODO: 集成实际的HTTP资源搜索
    QList<SearchResultItem> results;
    results.append({
        tr("Sample Document.pdf"),
        "https://example.com/doc1.pdf",
        "2.5 MB",
        tr("Sample Host"),
        tr("Document"),
        "2025-12-25",
        0,
        0
    });

    display_results(results);
}

void DiscoveryPage::search_cloud_resources(const QString& /*keyword*/)
{
    // TODO: 集成网盘资源搜索
    QList<SearchResultItem> results;
    results.append({
        tr("Sample Archive.zip"),
        "https://pan.example.com/s/xxx",
        "1.2 GB",
        tr("Cloud Drive"),
        tr("Archive"),
        "2025-12-24",
        0,
        0
    });

    display_results(results);
}

void DiscoveryPage::search_ftp_resources(const QString& /*keyword*/)
{
    // TODO: 集成FTP资源搜索
    QList<SearchResultItem> results;

    display_results(results);
}

void DiscoveryPage::display_results(const QList<SearchResultItem>& results)
{
    current_results_ = results;

    for (const auto& item : results) {
        int row = results_table_->rowCount();
        results_table_->insertRow(row);

        results_table_->setItem(row, 0, new QTableWidgetItem(item.title));
        results_table_->setItem(row, 1, new QTableWidgetItem(item.size));
        results_table_->setItem(row, 2, new QTableWidgetItem(item.source));
        results_table_->setItem(row, 3, new QTableWidgetItem(item.type));

        if (item.seeders > 0) {
            results_table_->setItem(row, 4, new QTableWidgetItem(format_number(item.seeders)));
            results_table_->setItem(row, 5, new QTableWidgetItem(format_number(item.leechers)));
        } else {
            results_table_->setItem(row, 4, new QTableWidgetItem("-"));
            results_table_->setItem(row, 5, new QTableWidgetItem("-"));
        }

        // 操作列添加按钮
        auto* operation_widget = new QWidget(this);
        auto* op_layout = new QHBoxLayout(operation_widget);
        op_layout->setContentsMargins(5, 2, 5, 2);
        op_layout->setSpacing(5);

        auto* download_btn = new QPushButton(tr("Download"), operation_widget);
        download_btn->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
        download_btn->setCursor(Qt::PointingHandCursor);
        auto* copy_btn = new QPushButton(tr("Copy"), operation_widget);
        copy_btn->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        copy_btn->setCursor(Qt::PointingHandCursor);

        op_layout->addWidget(download_btn);
        op_layout->addWidget(copy_btn);
        op_layout->addStretch();

        results_table_->setCellWidget(row, 6, operation_widget);

        // 连接按钮信号（使用行号标识）
        connect(download_btn, &QPushButton::clicked, this, [this, row]() {
            if (row < current_results_.size()) {
                // TODO: 添加到下载任务
                QMessageBox::information(this, tr("Download"),
                    tr("Download: %1\nURL: %2").arg(current_results_[row].title, current_results_[row].url));
            }
        });

        connect(copy_btn, &QPushButton::clicked, this, [this, row]() {
            if (row < current_results_.size()) {
                QApplication::clipboard()->setText(current_results_[row].url);
                status_label_->setText(tr("Copied to clipboard."));
            }
        });
    }

    status_label_->setText(tr("Done. %1 result(s).").arg(results.size()));
    result_count_label_->setText(tr("%1 result(s).").arg(results.size()));
}

void DiscoveryPage::clear_search()
{
    search_input_->clear();
    results_table_->setRowCount(0);
    current_results_.clear();
    status_label_->setText(tr("Ready"));
    result_count_label_->clear();
}

void DiscoveryPage::download_selected()
{
    auto selected_rows = results_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        QMessageBox::warning(this, tr("Notice"), tr("Select items first."));
        return;
    }

    // TODO: 批量添加到下载任务
    QMessageBox::information(this, tr("Download"), tr("Added %1 task(s) to queue.").arg(selected_rows.size()));
}

void DiscoveryPage::copy_link()
{
    auto selected_rows = results_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return;
    }

    int row = selected_rows.first().row();
    if (row < current_results_.size()) {
        QApplication::clipboard()->setText(current_results_[row].url);
        status_label_->setText(tr("Copied to clipboard."));
    }
}

void DiscoveryPage::open_link()
{
    auto selected_rows = results_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return;
    }

    int row = selected_rows.first().row();
    if (row < current_results_.size()) {
        QDesktopServices::openUrl(QUrl(current_results_[row].url));
    }
}

void DiscoveryPage::on_search_type_changed(int index)
{
    // 根据搜索类型调整界面
    QString type = search_type_combo_->itemData(index).toString();

    if (type == "magnet") {
        // 显示种子数相关列
        results_table_->showColumn(4);
        results_table_->showColumn(5);
    } else {
        // 隐藏种子数相关列
        results_table_->hideColumn(4);
        results_table_->hideColumn(5);
    }
}

void DiscoveryPage::show_item_details(int row)
{
    if (row >= current_results_.size()) {
        return;
    }

    const auto& item = current_results_[row];

    QString details = tr(
        "Title: %1\n"
        "Size: %2\n"
        "Source: %3\n"
        "Type: %4\n"
        "Date: %5\n"
        "URL: %6"
    ).arg(item.title)
     .arg(item.size)
     .arg(item.source)
     .arg(item.type)
     .arg(item.date)
     .arg(item.url);

    if (item.seeders > 0) {
        details += QString("\n%1: %2\n%3: %4")
            .arg(tr("Seeders"), format_number(item.seeders), tr("Leechers"), format_number(item.leechers));
    }

    QMessageBox::information(this, tr("Details"), details);
}

void DiscoveryPage::show_context_menu(const QPoint& pos)
{
    QMenu menu(this);

    auto* download_action = menu.addAction(style()->standardIcon(QStyle::SP_ArrowDown), tr("Download"));
    auto* copy_action = menu.addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Copy URL"));
    auto* open_action = menu.addAction(style()->standardIcon(QStyle::SP_DirLinkIcon), tr("Open in Browser"));
    menu.addSeparator();
    auto* queue_action = menu.addAction(style()->standardIcon(QStyle::SP_FileDialogListView), tr("Add to Queue"));

    QAction* action = menu.exec(results_table_->mapToGlobal(pos));

    if (action == download_action) {
        download_selected();
    } else if (action == copy_action) {
        copy_link();
    } else if (action == open_action) {
        open_link();
    } else if (action == queue_action) {
        add_to_download_queue();
    }
}

void DiscoveryPage::add_to_download_queue()
{
    auto selected_rows = results_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        QMessageBox::warning(this, tr("Notice"), tr("Select items first."));
        return;
    }

    // TODO: 添加到下载队列（不立即开始）
    QMessageBox::information(this, tr("Queue"),
        tr("Added %1 task(s) to queue.").arg(selected_rows.size()));
}

QString DiscoveryPage::format_number(int num) const
{
    if (num >= 1000000) {
        return QString("%1M").arg(num / 1000000.0, 0, 'f', 1);
    } else if (num >= 1000) {
        return QString("%1K").arg(num / 1000.0, 0, 'f', 1);
    }
    return QString::number(num);
}

} // namespace falcon::desktop
