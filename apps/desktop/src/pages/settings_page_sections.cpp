/**
 * @file settings_page_sections.cpp
 * @brief Settings page implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "settings_page.hpp"

#include "../dialogs/search_engine_dialog.hpp"
#include "../services/search_engine_catalog.hpp"
#include "../services/update_checker.hpp"
#include "../utils/theme_manager.hpp"

#include <QFontMetrics>
#include <QMessageBox>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QLabel>
#include <QHeaderView>
#include <QScrollArea>
#include <QFrame>
#include <QStyle>

namespace falcon::desktop {

QWidget* SettingsPage::create_completion_action_section_widget()
{
    auto* group = new QGroupBox(tr("下载完成后"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // Action selection
    auto* action_layout = new QHBoxLayout();
    action_layout->setSpacing(12);

    auto* action_label = new QLabel(tr("完成后动作:"), this);
    action_layout->addWidget(action_label);

    completion_action_combo_ = new QComboBox(this);
    completion_action_combo_->addItem(tr("无操作"));
    completion_action_combo_->addItem(tr("打开文件"));
    completion_action_combo_->addItem(tr("打开文件夹"));
    completion_action_combo_->addItem(tr("仅显示通知"));
    action_layout->addWidget(completion_action_combo_);

    action_layout->addStretch();
    layout->addLayout(action_layout);

    // B18: 已完成任务双击行为（默认打开文件，可切换为打开所在文件夹）
    auto* dbl_layout = new QHBoxLayout();
    dbl_layout->setSpacing(12);

    auto* dbl_label = new QLabel(tr("已完成双击:"), this);
    dbl_layout->addWidget(dbl_label);

    completed_double_click_combo_ = new QComboBox(this);
    completed_double_click_combo_->addItem(tr("打开文件（默认）"));
    completed_double_click_combo_->addItem(tr("打开所在文件夹"));
    dbl_layout->addWidget(completed_double_click_combo_);

    dbl_layout->addStretch();
    layout->addLayout(dbl_layout);

    // 说明文字
    auto* desc_label = new QLabel(
        tr("选择下载完成后的动作；多文件任务仅显示通知。「已完成双击」决定在下载页双击已完成任务时打开文件还是所在文件夹，改动立即生效。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // B18: 双击行为变更即时上抛（MainWindow 落 QSettings；下载页双击时
    // 即时读取，无需向下载页回写）
    connect(completed_double_click_combo_, &QComboBox::currentIndexChanged,
            this, [this](int index) { emit completed_double_click_changed(index); });

    return group;
}

QWidget* SettingsPage::create_connection_section_widget()
{
    auto* group = new QGroupBox(tr("连接"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default connections
    auto* conn_label = new QLabel(tr("默认连接数:"), this);
    default_connections_spin_ = new QSpinBox(this);
    default_connections_spin_->setRange(1, 16);
    default_connections_spin_->setValue(4);
    layout->addRow(conn_label, default_connections_spin_);

    // Connection timeout
    auto* timeout_label = new QLabel(tr("连接超时:"), this);
    connection_timeout_spin_ = new QSpinBox(this);
    connection_timeout_spin_->setRange(5, 120);
    connection_timeout_spin_->setValue(30);
    connection_timeout_spin_->setSuffix(" s");
    layout->addRow(timeout_label, connection_timeout_spin_);

    // Retry count
    auto* retry_label = new QLabel(tr("重试次数:"), this);
    retry_count_spin_ = new QSpinBox(this);
    retry_count_spin_->setRange(0, 10);
    retry_count_spin_->setValue(3);
    layout->addRow(retry_label, retry_count_spin_);

    // Daemon RPC 模式：经 aria2 兼容 JSON-RPC 连接 falcon-daemon。
    // 后端在应用启动时创建，切换需重启应用生效。
    auto* daemon_label = new QLabel(tr("Daemon 模式:"), this);
    daemon_enabled_checkbox_ = new QCheckBox(
        tr("连接 falcon-daemon（重启后生效）"), this);
    layout->addRow(daemon_label, daemon_enabled_checkbox_);

    auto* daemon_url_label = new QLabel(tr("Daemon RPC URL:"), this);
    daemon_url_edit_ = new QLineEdit(this);
    daemon_url_edit_->setPlaceholderText("http://127.0.0.1:6800/jsonrpc");
    layout->addRow(daemon_url_label, daemon_url_edit_);

    auto* daemon_secret_label = new QLabel(tr("Daemon RPC 密钥:"), this);
    daemon_secret_edit_ = new QLineEdit(this);
    daemon_secret_edit_->setEchoMode(QLineEdit::Password);
    daemon_secret_edit_->setPlaceholderText(tr("token 密钥（可选）"));
    layout->addRow(daemon_secret_label, daemon_secret_edit_);

    return group;
}

QWidget* SettingsPage::create_notification_section_widget()
{
    auto* group = new QGroupBox(tr("通知"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    notifications_checkbox_ = new QCheckBox(tr("启用通知"), this);
    notifications_checkbox_->setChecked(true);
    layout->addWidget(notifications_checkbox_);

    sound_notification_checkbox_ = new QCheckBox(tr("提示音"), this);
    layout->addWidget(sound_notification_checkbox_);

    return group;
}

QWidget* SettingsPage::create_float_widget_section_widget()
{
    auto* group = new QGroupBox(tr("悬浮速度窗"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    float_enabled_checkbox_ = new QCheckBox(
        tr("显示悬浮速度窗（始终置顶，可拖动到任意位置）"), this);
    float_enabled_checkbox_->setChecked(true);
    // 勾选即时生效（沿剪切板监听范式）：不经「应用」按钮立即显隐；
    // 持久化与显隐由 MainWindow 的 toggled 处理器收口（B21③）
    connect(float_enabled_checkbox_, &QCheckBox::toggled, this,
            [this](bool checked) { emit float_widget_toggled(checked); });
    layout->addWidget(float_enabled_checkbox_);

    float_show_active_checkbox_ = new QCheckBox(tr("显示活跃任务数"), this);
    float_show_active_checkbox_->setChecked(true);
    layout->addWidget(float_show_active_checkbox_);

    float_show_progress_checkbox_ = new QCheckBox(tr("显示总进度"), this);
    float_show_progress_checkbox_->setChecked(true);
    layout->addWidget(float_show_progress_checkbox_);

    auto* size_row = new QHBoxLayout();
    size_row->addWidget(new QLabel(tr("大小:"), this));
    float_size_combo_ = new QComboBox(this);
    float_size_combo_->addItem(tr("小"));
    float_size_combo_->addItem(tr("中"));
    float_size_combo_->addItem(tr("大"));
    float_size_combo_->setCurrentIndex(0); // B21①: 默认紧密档 = 小
    size_row->addWidget(float_size_combo_);
    size_row->addStretch();
    layout->addLayout(size_row);

    auto* opacity_row = new QHBoxLayout();
    opacity_row->addWidget(new QLabel(tr("透明度:"), this));
    float_opacity_spin_ = new QSpinBox(this);
    float_opacity_spin_->setRange(30, 100);
    float_opacity_spin_->setSuffix(tr(" %"));
    float_opacity_spin_->setValue(90);
    opacity_row->addWidget(float_opacity_spin_);
    opacity_row->addStretch();
    layout->addLayout(opacity_row);

    float_click_through_checkbox_ = new QCheckBox(
        tr("鼠标穿透（开启后点击会穿过悬浮窗，需回到此处关闭）"), this);
    layout->addWidget(float_click_through_checkbox_);

    return group;
}

QWidget* SettingsPage::create_about_section_widget()
{
    auto* group = new QGroupBox(tr("关于与更新"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // 版本行：当前版本 + 检查按钮
    auto* version_layout = new QHBoxLayout();
    version_layout->setSpacing(12);

    version_layout->addWidget(new QLabel(tr("当前版本:"), this));

    auto* version_label = new QLabel(UpdateChecker::current_version(), this);
    version_layout->addWidget(version_label);

    version_layout->addStretch();

    check_updates_button_ = new QPushButton(tr("检查更新"), this);
    check_updates_button_->setCursor(Qt::PointingHandCursor);
    check_updates_button_->setObjectName("toolButton");
    version_layout->addWidget(check_updates_button_);

    layout->addLayout(version_layout);

    // 检查结果（链接跳 GitHub 发布页——无自更新，aria2 同姿态）
    update_result_label_ = new QLabel(tr("尚未检查更新。"), this);
    update_result_label_->setObjectName("cardInfoLabel");
    update_result_label_->setWordWrap(true);
    update_result_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    update_result_label_->setOpenExternalLinks(true);
    layout->addWidget(update_result_label_);

    check_updates_on_startup_checkbox_ = new QCheckBox(tr("启动时自动检查更新"), this);
    check_updates_on_startup_checkbox_->setChecked(true);
    layout->addWidget(check_updates_on_startup_checkbox_);

    auto* desc_label = new QLabel(
        tr("发现新版本时提供 GitHub 发布页链接，应用不会自动下载或安装更新。"), this);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // 页面内自持一个检查器（MainWindow 另持一个做启动静默检查）
    update_checker_ = new UpdateChecker(this);
    connect(check_updates_button_, &QPushButton::clicked, this, [this]() {
        if (update_checker_->checking()) {
            return;
        }
        check_updates_button_->setEnabled(false);
        check_updates_button_->setText(tr("正在检查…"));
        update_result_label_->setText(tr("正在连接 GitHub Releases…"));
        update_checker_->check_for_updates();
    });
    connect(update_checker_, &UpdateChecker::update_available, this,
            [this](const QString& latest, const QString& url) {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(
            tr("发现新版本 %1，可<a href=\"%2\">前往发布页下载</a>。").arg(latest, url));
    });
    connect(update_checker_, &UpdateChecker::up_to_date, this, [this]() {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(tr("当前已是最新版本。"));
    });
    connect(update_checker_, &UpdateChecker::check_failed, this,
            [this](const QString& reason) {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(tr("检查失败：%1").arg(reason));
    });

    return group;
}

QLayout* SettingsPage::create_action_buttons_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);
    layout->setContentsMargins(0, 20, 0, 0);

    layout->addStretch();

    reset_button_ = new QPushButton(tr("重置"), this);
    reset_button_->setCursor(Qt::PointingHandCursor);
    reset_button_->setMinimumWidth(100);
    reset_button_->setObjectName("toolButton");
    connect(reset_button_, &QPushButton::clicked, this, &SettingsPage::reset_to_defaults);
    layout->addWidget(reset_button_);

    apply_button_ = new QPushButton(tr("应用"), this);
    apply_button_->setCursor(Qt::PointingHandCursor);
    apply_button_->setMinimumWidth(100);
    apply_button_->setObjectName("primaryButton");
    connect(apply_button_, &QPushButton::clicked, this, &SettingsPage::apply_settings);
    layout->addWidget(apply_button_);

    return layout;
}

QWidget* SettingsPage::create_appearance_section_widget()
{
    auto* group = new QGroupBox(tr("外观"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // 主题设置行（三级切换：主题家族 / 亮暗模式 / 跟随系统，选择即生效并记住）
    auto* theme_layout = new QHBoxLayout();
    theme_layout->setSpacing(12);

    auto* theme_label = new QLabel(tr("主题:"), this);
    theme_layout->addWidget(theme_label);

    // 主题家族：整体色相方向（强调色与中性底色随家族切换，设计语言不变）
    theme_family_combo_ = new QComboBox(this);
    theme_family_combo_->addItem(ThemeManager::family_name(ThemeFamily::Warm));
    theme_family_combo_->addItem(ThemeManager::family_name(ThemeFamily::Cold));
    theme_family_combo_->addItem(ThemeManager::family_name(ThemeFamily::Graphite));
    theme_family_combo_->setCursor(Qt::PointingHandCursor);
    connect(theme_family_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { emit theme_family_changed(theme_family_combo_->currentIndex()); });
    theme_layout->addWidget(theme_family_combo_);

    // 亮暗模式：浅色 / 深色 / 跟随系统
    theme_mode_combo_ = new QComboBox(this);
    theme_mode_combo_->addItem(ThemeManager::mode_name(ThemeMode::Light));
    theme_mode_combo_->addItem(ThemeManager::mode_name(ThemeMode::Dark));
    theme_mode_combo_->addItem(ThemeManager::mode_name(ThemeMode::System));
    theme_mode_combo_->setCursor(Qt::PointingHandCursor);
    connect(theme_mode_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { emit theme_mode_changed(theme_mode_combo_->currentIndex()); });
    theme_layout->addWidget(theme_mode_combo_);

    theme_layout->addStretch();

    // 当前实际生效的亮暗（跟随系统时按系统亮暗解析后显示）
    current_theme_label_ = new QLabel(tr("浅色"), this);
    theme_layout->addWidget(current_theme_label_);

    layout->addLayout(theme_layout);

    // 说明文字
    auto* desc_label = new QLabel(tr("选择主题家族与亮暗模式；跟随系统时按系统亮暗自动切换。切换后立即生效并记住。"),
                                 this);
    desc_label->setObjectName("cardInfoLabel");
    desc_label->setWordWrap(true);
    layout->addWidget(desc_label);

    // 任务列表视图（默认卡片；与顶栏/下载页内的切换按钮共用同一份记忆，
    // 手动切换后此处同步显示，首次/无记录即「卡片视图（默认）」）
    auto* view_layout = new QHBoxLayout();
    view_layout->setSpacing(12);

    auto* view_label = new QLabel(tr("任务列表视图:"), this);
    view_layout->addWidget(view_label);

    task_view_combo_ = new QComboBox(this);
    task_view_combo_->addItem(tr("卡片视图（默认）"), true);
    task_view_combo_->addItem(tr("列表视图"), false);
    task_view_combo_->setCursor(Qt::PointingHandCursor);
    connect(task_view_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { emit task_view_grid_changed(is_task_view_grid()); });
    view_layout->addWidget(task_view_combo_);

    view_layout->addStretch();
    layout->addLayout(view_layout);

    auto* view_desc = new QLabel(tr("下载页任务的显示方式；切换后立即生效并记住你的选择。"), this);
    view_desc->setObjectName("cardInfoLabel");
    view_desc->setWordWrap(true);
    layout->addWidget(view_desc);

    return group;
}

void SettingsPage::set_task_view_grid(bool grid_view)
{
    if (!task_view_combo_) {
        return;
    }
    const int want = task_view_combo_->findData(grid_view);
    if (want < 0 || task_view_combo_->currentIndex() == want) {
        return;
    }
    // 程序化回写不反向触发 task_view_grid_changed（避免设置页→下载页→设置页回环）
    QSignalBlocker blocker(task_view_combo_);
    task_view_combo_->setCurrentIndex(want);
}

bool SettingsPage::is_task_view_grid() const
{
    if (!task_view_combo_) {
        return true;  // 默认卡片视图
    }
    return task_view_combo_->currentData().toBool();
}

} // namespace falcon::desktop
