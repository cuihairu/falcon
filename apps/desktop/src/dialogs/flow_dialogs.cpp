/**
 * @file flow_dialogs.cpp
 * @brief ui-next 六个弹层对话框实现（真实本地动作）
 */

#include "flow_dialogs.hpp"

#include "../services/local_store.hpp"
#include "../widgets/proto_widgets.hpp"
#include "../utils/icon_utils.hpp"
#include "../utils/qr_code.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTimer>
#include <QClipboard>
#include <QApplication>
#include <QFrame>
#include <QFont>
#include <QFileInfo>
#include <QPixmap>
#include <QList>
#include <QRandomGenerator>
#include <QMessageBox>

namespace falcon::desktop::proto {
namespace {

/** 弹层统一底部按钮行 */
QWidget* dialog_footer(QDialog* dlg, const QList<QPushButton*>& buttons)
{
    auto* row = new QWidget(dlg);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addStretch(1);
    for (auto* b : buttons) {
        layout->addWidget(b);
    }
    return row;
}

/** 表单分组小标题 */
QLabel* group_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName("sectionLabel");
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}

} // namespace

// ============================================================
// 文件详情
// ============================================================
TaskDetailDialog::TaskDetailDialog(const falcon::daemon::rpc::TaskSnapshot& snapshot,
                                   QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("文件详情"));
    resize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("文件详情"), this);
    title->setObjectName("heroTitle");
    layout->addWidget(title);

    const QString name = QFileInfo(QString::fromStdString(snapshot.output_path)).fileName();
    auto* sub = new QLabel(name, this);
    sub->setObjectName("heroDescription");
    sub->setWordWrap(true);
    layout->addWidget(sub);

    auto* grid = new QGridLayout();
    grid->setSpacing(14);
    auto add_cell = [this, grid](int row, int col, const QString& label, const QString& value) {
        grid->addWidget(info_cell(label, value, this), row, col);
    };
    add_cell(0, 0, tr("文件大小"), QString::number(snapshot.total_bytes) + tr(" B"));
    add_cell(0, 1, tr("已下载"), QString::number(snapshot.downloaded_bytes) + tr(" B"));
    add_cell(1, 0, tr("保存路径"), QString::fromStdString(snapshot.output_path));
    add_cell(1, 1, tr("下载链接"), QString::fromStdString(snapshot.url));
    add_cell(2, 0, tr("原始节点数"), tr("—（快照无此字段）"));
    add_cell(2, 1, tr("校验"), tr("—（快照无此字段）"));
    layout->addLayout(grid);

    auto* cloud = new QPushButton(tr("添加到云盘"), this);
    cloud->setObjectName("primaryButton");
    connect(cloud, &QPushButton::clicked, this, &TaskDetailDialog::add_to_cloud_requested);
    auto* close = new QPushButton(tr("完成"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(dialog_footer(this, QList<QPushButton*>() << cloud << close));
}

// ============================================================
// 展示传送码
// ============================================================
ShowCodeDialog::ShowCodeDialog(QWidget* parent)
    : QDialog(parent)
    , qr_label_(nullptr)
    , code_label_(nullptr)
    , countdown_label_(nullptr)
    , timer_(nullptr)
    , remaining_seconds_(120)
{
    setWindowTitle(tr("展示传送码"));
    resize(520, 470);
    regenerate();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("展示传送码"), this);
    title->setObjectName("heroTitle");
    layout->addWidget(title);
    auto* sub = new QLabel(tr("让对方扫二维码，或把 6 位码告诉他手动输入——两种方式连的是同一条加密通道。"), this);
    sub->setObjectName("heroDescription");
    sub->setWordWrap(true);
    layout->addWidget(sub);

    qr_label_ = new QLabel(this);
    qr_label_->setAlignment(Qt::AlignCenter);
    qr_label_->setObjectName("summaryCard");
    qr_label_->setMinimumHeight(160);
    qr_label_->setFrameShape(QFrame::Box);
    qr_label_->setToolTip(tr("扫码与下方 6 位码等价；这是标准二维码，可用手机相机扫描。"));
    layout->addWidget(qr_label_);

    code_label_ = new QLabel(code_, this);
    code_label_->setObjectName("summaryValue");
    code_label_->setAlignment(Qt::AlignCenter);
    QFont big = code_label_->font();
    big.setPointSizeF(big.pointSizeF() + 12);
    big.setBold(true);
    code_label_->setFont(big);
    layout->addWidget(code_label_);

    countdown_label_ = new QLabel(this);
    countdown_label_->setObjectName("sectionLabel");
    countdown_label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(countdown_label_);

    layout->addWidget(notice(tr("一次性 · 120 秒内有效，配对成功后立即作废；码与密钥不会离开本机。"), this));

    auto* copy = new QPushButton(tr("复制码"), this);
    connect(copy, &QPushButton::clicked, this, &ShowCodeDialog::copy_code);
    auto* refresh = new QPushButton(tr("刷新码"), this);
    connect(refresh, &QPushButton::clicked, this, &ShowCodeDialog::refresh_code);
    auto* done = new QPushButton(tr("完成"), this);
    done->setObjectName("primaryButton");
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(dialog_footer(this, QList<QPushButton*>() << copy << refresh << done));

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &ShowCodeDialog::tick_countdown);
    timer_->start();
    update_qr();
    tick_countdown();
}

void ShowCodeDialog::regenerate()
{
    code_ = LocalStore::instance().issue_code(QStringLiteral("airdrop"), 120);
    remaining_seconds_ = 120;
}

void ShowCodeDialog::update_qr()
{
    if (!qr_label_) {
        return;
    }
    const QImage image = make_qr_image(code_);
    if (!image.isNull()) {
        qr_label_->setPixmap(QPixmap::fromImage(image).scaled(
            158, 158, Qt::KeepAspectRatio, Qt::FastTransformation));
    } else {
        qr_label_->setText(code_);
    }
}

void ShowCodeDialog::refresh_code()
{
    regenerate();
    if (code_label_) code_label_->setText(code_);
    update_qr();
    tick_countdown();
}

void ShowCodeDialog::copy_code()
{
    QApplication::clipboard()->setText(code_);
}

void ShowCodeDialog::tick_countdown()
{
    if (remaining_seconds_ > 0) {
        --remaining_seconds_;
    }
    if (countdown_label_) {
        countdown_label_->setText(tr("%1 秒后失效").arg(remaining_seconds_));
    }
}

// ============================================================
// 匹配码配对
// ============================================================
PairCodeDialog::PairCodeDialog(QWidget* parent)
    : QDialog(parent)
    , scan_pane_(nullptr)
    , manual_pane_(nullptr)
    , code_edit_(nullptr)
    , scan_code_edit_(nullptr)
    , scan_tab_(nullptr)
    , manual_tab_(nullptr)
{
    setWindowTitle(tr("匹配码配对"));
    resize(480, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("连接对方设备"), this);
    title->setObjectName("heroTitle");
    layout->addWidget(title);
    auto* sub = new QLabel(tr("扫码更快，或手动输入对方展示的 6 位码。两者等价，任选其一。"), this);
    sub->setObjectName("heroDescription");
    sub->setWordWrap(true);
    layout->addWidget(sub);

    auto* tabs = new QHBoxLayout();
    tabs->setSpacing(6);
    scan_tab_ = new QPushButton(tr("扫码"), this);
    manual_tab_ = new QPushButton(tr("手动输入"), this);
    tabs->addWidget(scan_tab_);
    tabs->addWidget(manual_tab_);
    tabs->addStretch(1);
    layout->addLayout(tabs);

    scan_pane_ = new QWidget(this);
    auto* scan_layout = new QVBoxLayout(scan_pane_);
    auto* scan_hint = new QLabel(
        tr("用手机扫描对方展示的二维码后，把得到的 6 位码粘贴到下方；与手动输入完全等价。"),
        scan_pane_);
    scan_hint->setObjectName("heroDescription");
    scan_hint->setWordWrap(true);
    scan_layout->addWidget(scan_hint);

    scan_code_edit_ = new QLineEdit(scan_pane_);
    scan_code_edit_->setPlaceholderText(tr("粘贴扫码得到的 6 位码"));
    scan_code_edit_->setMaxLength(7);
    scan_layout->addWidget(scan_code_edit_);

    auto* paste_button = new QPushButton(tr("从剪贴板粘贴"), scan_pane_);
    connect(paste_button, &QPushButton::clicked, this, &PairCodeDialog::paste_from_clipboard);
    scan_layout->addWidget(paste_button);
    scan_layout->addStretch(1);
    layout->addWidget(scan_pane_);

    manual_pane_ = new QWidget(this);
    auto* manual_layout = new QVBoxLayout(manual_pane_);
    code_edit_ = new QLineEdit(manual_pane_);
    code_edit_->setPlaceholderText(tr("输入 6 位码"));
    code_edit_->setMaxLength(7);
    manual_layout->addWidget(code_edit_);
    auto* hint = new QLabel(tr("码在 120 秒内有效 · 输错 3 次将临时锁定"), manual_pane_);
    hint->setObjectName("summaryCaption");
    manual_layout->addWidget(hint);
    manual_layout->addStretch(1);
    layout->addWidget(manual_pane_);
    manual_pane_->hide();

    connect(scan_tab_, &QPushButton::clicked, this, &PairCodeDialog::set_scan_mode);
    connect(manual_tab_, &QPushButton::clicked, this, &PairCodeDialog::set_manual_mode);

    auto* cancel = new QPushButton(tr("取消"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* connect_button = new QPushButton(tr("连接"), this);
    connect_button->setObjectName("primaryButton");
    connect(connect_button, &QPushButton::clicked, this, &PairCodeDialog::emit_connect);
    layout->addWidget(dialog_footer(this, QList<QPushButton*>() << cancel << connect_button));
}

void PairCodeDialog::set_scan_mode()
{
    scan_pane_->show();
    manual_pane_->hide();
}

void PairCodeDialog::set_manual_mode()
{
    scan_pane_->hide();
    manual_pane_->show();
    code_edit_->setFocus();
}

void PairCodeDialog::paste_from_clipboard()
{
    const QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, tr("匹配码"), tr("剪贴板中没有内容。"));
        return;
    }
    if (scan_code_edit_) {
        scan_code_edit_->setText(text);
    }
    if (code_edit_) {
        code_edit_->setText(text);
    }
}

void PairCodeDialog::emit_connect()
{
    // 两个模式共用同一串码：取当前可见输入框
    QString code = (scan_pane_ && scan_pane_->isVisible())
                       ? scan_code_edit_->text().trimmed()
                       : code_edit_->text().trimmed();
    code.remove(QStringLiteral(" "));
    if (code.size() != 6) {
        QMessageBox::warning(this, tr("匹配码"), tr("请输入 6 位数字码。"));
        return;
    }
    if (!LocalStore::instance().verify_code(code)) {
        QMessageBox::warning(this, tr("匹配码"), tr("码无效或已过期，请重新生成后再试。"));
        return;
    }
    emit connect_requested(code);
    accept();
}

// ============================================================
// 添加 NAS 节点
// ============================================================
AddNodeDialog::AddNodeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("添加存储节点"));
    resize(560, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("添加存储节点"), this);
    title->setObjectName("heroTitle");
    layout->addWidget(title);
    auto* sub = new QLabel(tr("选择预设可自动填入参数，也可自定义；配置保存在本机。"), this);
    sub->setObjectName("heroDescription");
    layout->addWidget(sub);

    auto* presets = section_card(tr("预设"), this);
    auto* presets_layout = qobject_cast<QVBoxLayout*>(presets->layout());
    auto* preset_row = new QHBoxLayout();
    preset_row->setSpacing(6);
    for (const QString& text : QStringList() << tr("自定义") << tr("飞牛 NAS") << tr("OpenWrt") << tr("S3 兼容")) {
        auto* button = new QPushButton(text, presets);
        connect(button, &QPushButton::clicked, this, [this, text]() { apply_preset(text); });
        preset_row->addWidget(button);
    }
    preset_row->addStretch(1);
    presets_layout->addLayout(preset_row);
    layout->addWidget(presets);

    auto* params = section_card(tr("连接参数"), this);
    auto* params_layout = qobject_cast<QVBoxLayout*>(params->layout());
    auto* form = new QGridLayout();
    form->setSpacing(8);
    auto add_field = [this, form, params](int row, const QString& label, QLineEdit*& edit,
                                          const QString& placeholder) {
        form->addWidget(group_label(label, params), row, 0);
        edit = new QLineEdit(params);
        edit->setPlaceholderText(placeholder);
        form->addWidget(edit, row, 1);
    };
    add_field(0, tr("名称"), name_edit_, tr("例：书房 NAS"));
    add_field(1, tr("协议"), protocol_edit_, tr("WebDAV / SMB / SFTP / S3"));
    add_field(2, tr("Endpoint"), endpoint_edit_, tr("https://192.168.1.20:5006"));
    add_field(3, tr("Access Key"), access_key_edit_, tr("用户名 / AK"));
    add_field(4, tr("Secret Key"), secret_key_edit_, tr("密码 / SK"));
    add_field(5, tr("区域 / Bucket"), region_edit_, tr("区域, Bucket"));
    params_layout->addLayout(form);
    layout->addWidget(params);

    auto* test = new QPushButton(tr("测试连接"), this);
    connect(test, &QPushButton::clicked, this, &AddNodeDialog::emit_test);
    auto* save = new QPushButton(tr("保存配置"), this);
    save->setObjectName("primaryButton");
    connect(save, &QPushButton::clicked, this, &AddNodeDialog::emit_save);
    auto* cancel = new QPushButton(tr("取消"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(dialog_footer(this, QList<QPushButton*>() << cancel << test << save));
}

void AddNodeDialog::apply_preset(const QString& preset)
{
    if (preset == tr("飞牛 NAS")) {
        protocol_edit_->setText(QStringLiteral("WebDAV"));
        endpoint_edit_->setText(QStringLiteral("https://192.168.1.20:5006"));
        name_edit_->setText(QStringLiteral("飞牛 NAS"));
    } else if (preset == tr("OpenWrt")) {
        protocol_edit_->setText(QStringLiteral("Samba"));
        endpoint_edit_->setText(QStringLiteral("smb://192.168.1.1"));
        name_edit_->setText(QStringLiteral("OpenWrt 路由"));
    } else if (preset == tr("S3 兼容")) {
        protocol_edit_->setText(QStringLiteral("S3"));
        endpoint_edit_->setText(QStringLiteral("https://s3.example.com"));
        name_edit_->setText(QStringLiteral("S3 节点"));
    }
}

NodeDraft AddNodeDialog::draft() const
{
    NodeDraft d;
    d.name = name_edit_->text().trimmed();
    d.protocol = protocol_edit_->text().trimmed();
    d.endpoint = endpoint_edit_->text().trimmed();
    d.access_key = access_key_edit_->text().trimmed();
    d.secret_key = secret_key_edit_->text().trimmed();
    d.region = region_edit_->text().trimmed();
    return d;
}

void AddNodeDialog::emit_test()
{
    const NodeDraft d = draft();
    if (d.protocol.isEmpty() || d.endpoint.isEmpty()) {
        QMessageBox::warning(this, tr("测试连接"), tr("协议与 Endpoint 不能为空。"));
        return;
    }
    emit test_connection_requested(d);
}

void AddNodeDialog::emit_save()
{
    const NodeDraft d = draft();
    if (d.name.isEmpty() || d.protocol.isEmpty() || d.endpoint.isEmpty()) {
        QMessageBox::warning(this, tr("保存配置"), tr("名称、协议与 Endpoint 不能为空。"));
        return;
    }
    emit save_requested(d);
    accept();
}

// ============================================================
// 登录
// ============================================================
LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("欢迎登录 Falcon"));
    resize(420, 400);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("Falcon"), this);
    title->setObjectName("heroTitle");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto* sub = new QLabel(tr("本地会话演示：验证码在本机生成，登录后资料保存在本机。"), this);
    sub->setObjectName("heroDescription");
    sub->setAlignment(Qt::AlignCenter);
    sub->setWordWrap(true);
    layout->addWidget(sub);

    layout->addWidget(group_label(tr("手机号"), this));
    phone_edit_ = new QLineEdit(this);
    phone_edit_->setPlaceholderText(tr("请输入手机号"));
    layout->addWidget(phone_edit_);

    layout->addWidget(group_label(tr("验证码"), this));
    auto* code_row = new QHBoxLayout();
    code_edit_ = new QLineEdit(this);
    code_edit_->setPlaceholderText(tr("请输入 6 位验证码"));
    code_edit_->setMaxLength(7);
    code_row->addWidget(code_edit_, 1);
    auto* request = new QPushButton(tr("获取验证码"), this);
    connect(request, &QPushButton::clicked, this, &LoginDialog::request_code);
    code_row->addWidget(request);
    layout->addLayout(code_row);

    code_hint_ = new QLabel(this);
    code_hint_->setObjectName("summaryCaption");
    layout->addWidget(code_hint_);

    auto* login = new QPushButton(tr("登录 / 注册"), this);
    login->setObjectName("primaryButton");
    connect(login, &QPushButton::clicked, this, &LoginDialog::emit_login);
    layout->addWidget(login);
    layout->addWidget(notice(tr("云端账号服务需后端接入；此处建立的是本地会话。"), this));
}

void LoginDialog::request_code()
{
    if (phone_edit_->text().trimmed().size() < 6) {
        QMessageBox::warning(this, tr("获取验证码"), tr("请输入有效手机号（至少 6 位）。"));
        return;
    }
    const QString code = LocalStore::instance().issue_code(QStringLiteral("login"), 120);
    code_hint_->setText(tr("本地验证码：%1（120 秒内有效）").arg(code));
}

void LoginDialog::emit_login()
{
    const QString phone = phone_edit_->text().trimmed();
    const QString code = code_edit_->text().trimmed();
    if (!LocalStore::instance().login(phone, code)) {
        QMessageBox::warning(this, tr("登录"), tr("手机号无效或验证码错误/已过期。"));
        return;
    }
    emit login_succeeded(phone);
    accept();
}

// ============================================================
// 编辑资料
// ============================================================
EditProfileDialog::EditProfileDialog(QWidget* parent)
    : QDialog(parent)
    , nickname_edit_(nullptr)
{
    setWindowTitle(tr("编辑资料"));
    resize(420, 340);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("编辑资料"), this);
    title->setObjectName("heroTitle");
    layout->addWidget(title);
    auto* sub = new QLabel(tr("昵称在好友、分享组与分享记录中显示，保存在本机。"), this);
    sub->setObjectName("heroDescription");
    sub->setWordWrap(true);
    layout->addWidget(sub);

    auto* avatar = new QLabel(QStringLiteral("F"), this);
    avatar->setFixedSize(48, 48);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setObjectName("summaryValue");
    auto* avatar_row = new QHBoxLayout();
    avatar_row->addStretch(1);
    avatar_row->addWidget(avatar);
    avatar_row->addStretch(1);
    layout->addLayout(avatar_row);

    layout->addWidget(group_label(tr("昵称"), this));
    nickname_edit_ = new QLineEdit(this);
    nickname_edit_->setText(LocalStore::instance().profile().nickname);
    layout->addWidget(nickname_edit_);

    layout->addWidget(group_label(tr("Falcon ID"), this));
    auto* id = new QLineEdit(LocalStore::instance().profile().id, this);
    id->setReadOnly(true);
    layout->addWidget(id);
    layout->addWidget(new QLabel(tr("ID 由系统生成，不可修改"), this));

    auto* cancel = new QPushButton(tr("取消"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* save = new QPushButton(tr("保存"), this);
    save->setObjectName("primaryButton");
    connect(save, &QPushButton::clicked, this, &EditProfileDialog::emit_save);
    layout->addWidget(dialog_footer(this, QList<QPushButton*>() << cancel << save));
}

void EditProfileDialog::emit_save()
{
    const QString nickname = nickname_edit_->text().trimmed();
    if (nickname.isEmpty()) {
        QMessageBox::warning(this, tr("编辑资料"), tr("昵称不能为空。"));
        return;
    }
    LocalProfile profile = LocalStore::instance().profile();
    profile.nickname = nickname;
    LocalStore::instance().save_profile(profile);
    accept();
}

} // namespace falcon::desktop::proto
