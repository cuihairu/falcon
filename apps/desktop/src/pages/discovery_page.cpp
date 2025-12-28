/**
 * @file discovery_page.cpp
 * @brief 资源发现与搜索页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "discovery_page.hpp"
#include "../styles.hpp"

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
    auto* title_label = new QLabel("资源发现", this);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 24px;
            font-weight: 700;
            color: #323130;
        }
    )");
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
    search_type_combo_->setStyleSheet(get_combo_stylesheet());
    search_type_combo_->addItem("🧲 磁力链接", "magnet");
    search_type_combo_->addItem("🌐 HTTP资源", "http");
    search_type_combo_->addItem("☁️ 云盘资源", "cloud");
    search_type_combo_->addItem("📡 FTP资源", "ftp");
    layout->addWidget(search_type_combo_);

    // 搜索输入框
    search_input_ = new QLineEdit(search_bar);
    search_input_->setStyleSheet(get_input_stylesheet());
    search_input_->setPlaceholderText("输入搜索关键词...");
    search_input_->setMinimumWidth(400);
    layout->addWidget(search_input_, 1);

    // 搜索按钮
    search_button_ = new QPushButton("🔍 搜索", search_bar);
    search_button_->setStyleSheet(get_button_stylesheet(true));
    search_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(search_button_);

    // 清空按钮
    clear_button_ = new QPushButton("✖️ 清空", search_bar);
    clear_button_->setStyleSheet(get_button_stylesheet(false));
    clear_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(clear_button_);

    // 排序方式
    sort_combo_ = new QComboBox(search_bar);
    sort_combo_->setStyleSheet(get_combo_stylesheet());
    sort_combo_->addItem("按相关性", "relevance");
    sort_combo_->addItem("按大小", "size");
    sort_combo_->addItem("按日期", "date");
    sort_combo_->addItem("按种子数", "seeders");
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
    auto* category_label = new QLabel("分类:", filter_bar);
    category_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    category_filter_ = new QComboBox(filter_bar);
    category_filter_->setStyleSheet(get_combo_stylesheet());
    category_filter_->addItem("全部", "all");
    category_filter_->addItem("视频", "video");
    category_filter_->addItem("音频", "audio");
    category_filter_->addItem("文档", "document");
    category_filter_->addItem("软件", "software");
    category_filter_->addItem("图片", "image");
    layout->addWidget(category_label);
    layout->addWidget(category_filter_);

    // 大小过滤
    auto* size_label = new QLabel("大小:", filter_bar);
    size_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    layout->addWidget(size_label);

    min_size_edit_ = new QLineEdit(filter_bar);
    min_size_edit_->setStyleSheet(get_input_stylesheet());
    min_size_edit_->setPlaceholderText("最小");
    min_size_edit_->setMaximumWidth(80);
    layout->addWidget(min_size_edit_);

    auto* to_label = new QLabel("-", filter_bar);
    to_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    layout->addWidget(to_label);

    max_size_edit_ = new QLineEdit(filter_bar);
    max_size_edit_->setStyleSheet(get_input_stylesheet());
    max_size_edit_->setPlaceholderText("最大");
    max_size_edit_->setMaximumWidth(80);
    layout->addWidget(max_size_edit_);

    size_filter_ = new QComboBox(filter_bar);
    size_filter_->setStyleSheet(get_combo_stylesheet());
    size_filter_->addItem("MB", "mb");
    size_filter_->addItem("GB", "gb");
    layout->addWidget(size_filter_);

    layout->addStretch();

    return filter_bar;
}

void DiscoveryPage::create_results_table()
{
    results_table_ = new QTableWidget(this);
    results_table_->setColumnCount(7);
    results_table_->setHorizontalHeaderLabels({
        "标题", "大小", "来源", "类型", "种子数", "下载中", "操作"
    });

    results_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_table_->horizontalHeader()->setStretchLastSection(false);
    results_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    results_table_->setAlternatingRowColors(true);
    results_table_->setStyleSheet(get_table_stylesheet());

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

    status_label_ = new QLabel("就绪", status_bar);
    status_label_->setStyleSheet(R"(
        QLabel {
            padding: 8px;
            color: #605e5c;
            font-size: 13px;
        }
    )");
    layout->addWidget(status_label_);

    layout->addStretch();

    result_count_label_ = new QLabel("", status_bar);
    result_count_label_->setStyleSheet(R"(
        QLabel {
            padding: 8px;
            color: #605e5c;
            font-size: 13px;
        }
    )");
    layout->addWidget(result_count_label_);

    return status_bar;
}

void DiscoveryPage::perform_search()
{
    QString keyword = search_input_->text().trimmed();
    if (keyword.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入搜索关键词");
        return;
    }

    settings_.search_type = search_type_combo_->currentData().toString();
    settings_.category = category_filter_->currentData().toString();
    settings_.sort_by = sort_combo_->currentData().toString();

    // 清空当前结果
    results_table_->setRowCount(0);
    current_results_.clear();

    status_label_->setText("正在搜索...");
    status_label_->setStyleSheet(R"(
        QLabel {
            padding: 8px;
            color: #0078d4;
            font-size: 13px;
        }
    )");

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

void DiscoveryPage::search_magnet_links(const QString& keyword)
{
    // TODO: 集成实际的磁力链接搜索API
    // 这里添加示例数据

    QList<SearchResultItem> results;
    results.append({
        "示例电影 2025 BluRay 1080p",
        "magnet:?xt=urn:btih:example1",
        "4.2 GB",
        "示例站点1",
        "视频",
        "2025-12-27",
        1523,
        456
    });
    results.append({
        "示例软件包 v2.0",
        "magnet:?xt=urn:btih:example2",
        "850 MB",
        "示例站点2",
        "软件",
        "2025-12-26",
        892,
        234
    });

    display_results(results);
}

void DiscoveryPage::search_http_resources(const QString& keyword)
{
    // TODO: 集成实际的HTTP资源搜索
    QList<SearchResultItem> results;
    results.append({
        "示例文档.pdf",
        "https://example.com/doc1.pdf",
        "2.5 MB",
        "示例下载站",
        "文档",
        "2025-12-25",
        0,
        0
    });

    display_results(results);
}

void DiscoveryPage::search_cloud_resources(const QString& keyword)
{
    // TODO: 集成网盘资源搜索
    QList<SearchResultItem> results;
    results.append({
        "示例资源包.zip",
        "https://pan.example.com/s/xxx",
        "1.2 GB",
        "百度网盘",
        "压缩包",
        "2025-12-24",
        0,
        0
    });

    display_results(results);
}

void DiscoveryPage::search_ftp_resources(const QString& keyword)
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

        auto* download_btn = new QPushButton("下载", operation_widget);
        download_btn->setStyleSheet(get_button_stylesheet(false));
        download_btn->setCursor(Qt::PointingHandCursor);
        auto* copy_btn = new QPushButton("复制", operation_widget);
        copy_btn->setStyleSheet(get_button_stylesheet(false));
        copy_btn->setCursor(Qt::PointingHandCursor);

        op_layout->addWidget(download_btn);
        op_layout->addWidget(copy_btn);
        op_layout->addStretch();

        results_table_->setCellWidget(row, 6, operation_widget);

        // 连接按钮信号（使用行号标识）
        connect(download_btn, &QPushButton::clicked, this, [this, row]() {
            if (row < current_results_.size()) {
                // TODO: 添加到下载任务
                QMessageBox::information(this, "下载",
                    QString("开始下载: %1\n链接: %2").arg(current_results_[row].title).arg(current_results_[row].url));
            }
        });

        connect(copy_btn, &QPushButton::clicked, this, [this, row]() {
            if (row < current_results_.size()) {
                QApplication::clipboard()->setText(current_results_[row].url);
                status_label_->setText("链接已复制到剪贴板");
            }
        });
    }

    status_label_->setText(QString("搜索完成，找到 %1 个结果").arg(results.size()));
    status_label_->setStyleSheet(R"(
        QLabel {
            padding: 8px;
            color: #107c10;
            font-size: 13px;
        }
    )");
    result_count_label_->setText(QString("共 %1 个结果").arg(results.size()));
}

void DiscoveryPage::clear_search()
{
    search_input_->clear();
    results_table_->setRowCount(0);
    current_results_.clear();
    status_label_->setText("就绪");
    result_count_label_->clear();
}

void DiscoveryPage::download_selected()
{
    auto selected_rows = results_table_->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择要下载的资源");
        return;
    }

    // TODO: 批量添加到下载任务
    QMessageBox::information(this, "下载", QString("已添加 %1 个任务到下载队列").arg(selected_rows.size()));
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
        status_label_->setText("链接已复制到剪贴板");
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

    QString details = QString(
        "标题: %1\n"
        "大小: %2\n"
        "来源: %3\n"
        "类型: %4\n"
        "发布日期: %5\n"
        "链接: %6"
    ).arg(item.title).arg(item.size).arg(item.source).arg(item.type).arg(item.date).arg(item.url);

    if (item.seeders > 0) {
        details += QString("\n种子数: %1\n下载中: %2").arg(format_number(item.seeders)).arg(format_number(item.leechers));
    }

    QMessageBox::information(this, "资源详情", details);
}

void DiscoveryPage::show_context_menu(const QPoint& pos)
{
    QMenu menu(this);

    auto* download_action = menu.addAction("📥 下载");
    auto* copy_action = menu.addAction("📋 复制链接");
    auto* open_action = menu.addAction("🌐 在浏览器中打开");
    menu.addSeparator();
    auto* queue_action = menu.addAction("📝 添加到下载队列");

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
        QMessageBox::warning(this, "提示", "请先选择要添加的资源");
        return;
    }

    // TODO: 添加到下载队列（不立即开始）
    QMessageBox::information(this, "添加到队列",
        QString("已添加 %1 个任务到下载队列").arg(selected_rows.size()));
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
