/**
 * @file social_pages.cpp
 * @brief ui-next 社交域页面实现（真实 LocalStore 数据）
 */

#include "social_pages.hpp"

#include "../services/local_store.hpp"
#include "../widgets/proto_widgets.hpp"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVector>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>

namespace falcon::desktop::proto {
namespace {

const char* kIdProperty = "falcon_item_id";

/** 统计条：从 LocalStore 取真实数量 */
QWidget* profile_stats(QWidget* parent)
{
    auto* bar = new QWidget(parent);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(10);
    const LocalStore& store = LocalStore::instance();
    struct Item { QString value; QString caption; };
    const QVector<Item> items = {
        { QString::number(store.paired_devices().size()), tr("绑定设备") },
        { QString::number(store.friends().size()), tr("好友") },
        { QString::number(store.share_groups().size()), tr("分享组") },
        { QString::number(store.share_records().size()), tr("分享记录") },
    };
    for (const Item& item : items) {
        layout->addWidget(stat_card(item.value, item.caption, bar), 1);
    }
    return bar;
}

/** 资料行：头像字母 + 名字 + 说明 + 右侧控件 */
QWidget* person_row(const QString& glyph, const QString& name, const QString& desc,
                    QWidget* right, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(12);

    auto* avatar = new QLabel(glyph, row);
    avatar->setFixedSize(34, 34);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setObjectName("summaryValue");
    layout->addWidget(avatar);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* name_label = new QLabel(name, row);
    name_label->setObjectName("sectionLabel");
    text->addWidget(name_label);
    auto* desc_label = new QLabel(desc, row);
    desc_label->setObjectName("summaryCaption");
    text->addWidget(desc_label);
    layout->addLayout(text, 1);
    if (right) {
        layout->addWidget(right, 0, Qt::AlignRight);
    }
    return row;
}

/** 关系能力入口行 */
QWidget* link_row(const QString& title, const QString& desc,
                  const QString& button_text, QObject* receiver, const char* member,
                  QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* button = new QPushButton(button_text, row);
    button->setObjectName("toolButton");
    QObject::connect(button, &QPushButton::clicked, receiver, member);
    return setting_row(title, desc, button, parent);
}

/** 带操作按钮的资料行 */
QWidget* action_person_row(const QString& glyph, const QString& name, const QString& desc,
                           const QString& action_text, const QString& id,
                           QObject* receiver, const char* member, QWidget* parent)
{
    auto* button = new QPushButton(action_text, parent);
    button->setObjectName("toolButton");
    button->setProperty(kIdProperty, id);
    QObject::connect(button, &QPushButton::clicked, receiver, member);
    return person_row(glyph, name, desc, button, parent);
}

} // namespace

// ============================================================
// 账号
// ============================================================
AccountPage::AccountPage(QWidget* parent)
    : ProtoPage(parent)
{
    refresh();
}

void AccountPage::refresh()
{
    const LocalProfile profile = LocalStore::instance().profile();
    set_header(tr("账号"),
               tr("账号是「人」的域：身份、好友、分享组都归在这里，与「跨设备与节点」的物正交。"),
               profile.logged_in ? tr("已登录") : tr("本地模式"));
    clear_content();
    build_content();
}

void AccountPage::build_content()
{
    const LocalProfile profile = LocalStore::instance().profile();
    auto* content = content_layout();

    content->addWidget(notice(
        profile.logged_in
            ? tr("已登录 %1（本地会话）。设备与关系数据保存在本机；云端同步需后端接入。").arg(profile.phone)
            : tr("第一期不依赖账号：设备间靠匹配码建立可信连接。"
                 "当前为本地模式，资料保存在本机；云端同步需后端接入。"),
        this));

    auto* profile_card = section_card(QString(), this);
    auto* profile_layout = qobject_cast<QVBoxLayout*>(profile_card->layout());
    auto* edit = new QPushButton(tr("编辑资料"), profile_card);
    edit->setObjectName("toolButton");
    connect(edit, &QPushButton::clicked, this, &AccountPage::on_edit_profile);
    profile_layout->addWidget(person_row(QStringLiteral("F"), profile.nickname,
                                         tr("ID: %1").arg(profile.id), edit, profile_card));
    profile_layout->addWidget(profile_stats(profile_card));
    content->addWidget(profile_card);

    auto* security = section_card(tr("账号安全 · 登录方式与可信设备管理"), this);
    auto* security_layout = qobject_cast<QVBoxLayout*>(security->layout());
    auto* manage = new QPushButton(profile.logged_in ? tr("退出") : tr("登录"), security);
    manage->setObjectName("toolButton");
    connect(manage, &QPushButton::clicked, this,
            profile.logged_in ? &AccountPage::on_logout : &AccountPage::on_login);
    security_layout->addWidget(setting_row(tr("登录方式"),
                                           profile.logged_in ? tr("手机号 %1").arg(profile.phone) : tr("未登录（本地模式）"),
                                           manage, security));
    auto* view = new QPushButton(tr("查看"), security);
    view->setObjectName("toolButton");
    connect(view, &QPushButton::clicked, this, &AccountPage::on_friends);
    security_layout->addWidget(setting_row(tr("设备管理"),
                                           tr("%1 台设备已配对").arg(LocalStore::instance().paired_devices().size()),
                                           view, security));
    auto* two_factor = new QCheckBox(security);
    two_factor->setEnabled(false);
    security_layout->addWidget(setting_row(tr("两步验证"), tr("需账号后端接入后启用"), two_factor, security));
    content->addWidget(security);

    auto* path = section_card(tr("演进路径"), this);
    qobject_cast<QVBoxLayout*>(path->layout())->addWidget(flow_steps(
        QStringList() << tr("阶段 1 设备配对 · 无账号")
                      << tr("阶段 2 账号 + 好友")
                      << tr("阶段 3 分享组 + 云同步"), path));
    content->addWidget(path);

    auto* relations = section_card(tr("关系能力 · 本机数据可用"), this);
    auto* relations_layout = qobject_cast<QVBoxLayout*>(relations->layout());
    relations_layout->addWidget(link_row(tr("好友"), tr("用匹配码添加可信联系人，管理好友请求"),
                                         tr("查看"), this, SLOT(on_friends()), relations));
    relations_layout->addWidget(link_row(tr("分享组"), tr("把我的设备与好友的设备编入同一共享域"),
                                         tr("查看"), this, SLOT(on_share_groups()), relations));
    relations_layout->addWidget(link_row(tr("分享记录"), tr("追溯每一次隔空传送与分组分享"),
                                         tr("查看"), this, SLOT(on_share_records()), relations));
    content->addWidget(relations);
}

void AccountPage::on_edit_profile() { emit edit_profile_requested(); }
void AccountPage::on_friends() { emit friends_requested(); }
void AccountPage::on_share_groups() { emit share_groups_requested(); }
void AccountPage::on_share_records() { emit share_records_requested(); }
void AccountPage::on_login() { emit login_requested(); }

void AccountPage::on_logout()
{
    LocalStore::instance().logout();
    refresh();
}

// ============================================================
// 好友
// ============================================================
FriendsPage::FriendsPage(QWidget* parent)
    : ProtoPage(parent)
{
    refresh();
}

void FriendsPage::refresh()
{
    set_header(tr("好友"),
               tr("用匹配码添加可信联系人。好友是「人」，与「我的设备」分开管理。"),
               tr("本机"));
    clear_content();
    build_content();
}

void FriendsPage::build_content()
{
    LocalStore& store = LocalStore::instance();
    auto* content = content_layout();
    if (!header_button_created_) {
        auto* add = add_header_button(tr("添加好友"));
        connect(add, &QPushButton::clicked, this, &FriendsPage::on_add_friend);
        header_button_created_ = true;
    }

    content->addWidget(notice(
        tr("添加好友复用同一套 6 位匹配码；好友与请求保存在本机，"
           "真正的跨设备关系同步需后端接入。"),
        this));

    const QVector<FriendRequest> requests = store.friend_requests();
    auto* requests_card = section_card(tr("好友请求 · %1 条待处理").arg(requests.size()), this);
    auto* requests_layout = qobject_cast<QVBoxLayout*>(requests_card->layout());
    if (requests.isEmpty()) {
        auto* empty = new QLabel(tr("暂无待处理请求。"), requests_card);
        empty->setObjectName("summaryCaption");
        requests_layout->addWidget(empty);
    }
    for (const FriendRequest& request : requests) {
        auto* actions = new QWidget(requests_card);
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        auto* reject = new QPushButton(tr("忽略"), actions);
        reject->setProperty(kIdProperty, request.id);
        connect(reject, &QPushButton::clicked, this, &FriendsPage::on_reject_request);
        actions_layout->addWidget(reject);
        auto* accept = new QPushButton(tr("接受"), actions);
        accept->setObjectName("primaryButton");
        accept->setProperty(kIdProperty, request.id);
        connect(accept, &QPushButton::clicked, this, &FriendsPage::on_accept_request);
        actions_layout->addWidget(accept);
        requests_layout->addWidget(person_row(QStringLiteral("◉"), request.name,
                                              request.message, actions, requests_card));
    }
    content->addWidget(requests_card);

    const QVector<Friend> friends = store.friends();
    auto* friends_card = section_card(tr("我的好友 · %1 人").arg(friends.size()), this);
    auto* friends_layout = qobject_cast<QVBoxLayout*>(friends_card->layout());
    if (friends.isEmpty()) {
        auto* empty = new QLabel(tr("暂无好友，点击右上角「添加好友」。"), friends_card);
        empty->setObjectName("summaryCaption");
        friends_layout->addWidget(empty);
    }
    for (const Friend& friend_item : friends) {
        friends_layout->addWidget(action_person_row(
            QStringLiteral("F"), friend_item.name,
            tr("%1 台设备 · %2").arg(friend_item.device_count).arg(friend_item.note),
            tr("传送文件"), friend_item.id, this, SLOT(on_transfer()), friends_card));
        auto* remove = new QPushButton(tr("删除"), friends_card);
        remove->setObjectName("toolButton");
        remove->setProperty(kIdProperty, friend_item.id);
        connect(remove, &QPushButton::clicked, this, &FriendsPage::on_remove_friend);
        friends_layout->addWidget(remove, 0, Qt::AlignRight);
    }
    content->addWidget(friends_card);
}

void FriendsPage::on_add_friend() { emit pair_code_requested(); }

void FriendsPage::on_accept_request()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (button) {
        LocalStore::instance().resolve_friend_request(button->property(kIdProperty).toString(), true);
        refresh();
    }
}

void FriendsPage::on_reject_request()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (button) {
        LocalStore::instance().resolve_friend_request(button->property(kIdProperty).toString(), false);
        refresh();
    }
}

void FriendsPage::on_remove_friend()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }
    const QString id = button->property(kIdProperty).toString();
    if (QMessageBox::question(this, tr("删除好友"), tr("确定删除该好友？"))
        == QMessageBox::Yes) {
        LocalStore::instance().remove_friend(id);
        refresh();
    }
}

void FriendsPage::on_transfer()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }
    const QString friend_id = button->property(kIdProperty).toString();
    QString friend_name;
    for (const Friend& f : LocalStore::instance().friends()) {
        if (f.id == friend_id) {
            friend_name = f.name;
            break;
        }
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("选择要传送的文件"));
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    ShareRecord record;
    record.content = info.fileName();
    record.size = QLocale::c().formattedDataSize(info.size());
    record.target = friend_name;
    record.method = tr("好友直传");
    record.status = tr("本机演示");
    LocalStore::instance().add_share_record(record);
    QMessageBox::information(this, tr("传送文件"),
                             tr("已为 %1 生成传送记录（%2）。文件实际传输需后端接入。")
                                 .arg(friend_name, record.content));
}

// ============================================================
// 分享组
// ============================================================
ShareGroupsPage::ShareGroupsPage(QWidget* parent)
    : ProtoPage(parent)
{
    refresh();
}

void ShareGroupsPage::refresh()
{
    set_header(tr("分享组"),
               tr("把「我的设备」与「好友的设备」编入同一共享域——这是人与物的连接点。"),
               tr("本机"));
    clear_content();
    build_content();
}

void ShareGroupsPage::build_content()
{
    LocalStore& store = LocalStore::instance();
    if (!header_button_created_) {
        auto* add = add_header_button(tr("新建分享组"));
        connect(add, &QPushButton::clicked, this, &ShareGroupsPage::on_create_group);
        header_button_created_ = true;
    }
    auto* content = content_layout();
    content->addWidget(notice(
        tr("组内共享：文件、下载记录、词库。邀请成员用匹配码；每组有独立 key epoch，"
           "成员移除即轮换密钥。组信息保存在本机。"), this));

    const QVector<ShareGroup> groups = store.share_groups();
    auto* groups_card = section_card(tr("我的分享组 · %1").arg(groups.size()), this);
    auto* groups_layout = qobject_cast<QVBoxLayout*>(groups_card->layout());
    if (groups.isEmpty()) {
        auto* empty = new QLabel(tr("暂无分享组，点击右上角「新建分享组」。"), groups_card);
        empty->setObjectName("summaryCaption");
        groups_layout->addWidget(empty);
    }
    for (const ShareGroup& group : groups) {
        groups_layout->addWidget(stat_card(
            tr("%1 · %2 名成员 · %3 台设备").arg(group.name).arg(group.members.size()).arg(group.device_count),
            tr("创建于 %1").arg(format_timestamp(group.created_at)), groups_card));
        auto* remove = new QPushButton(tr("删除组"), groups_card);
        remove->setObjectName("toolButton");
        remove->setProperty(kIdProperty, group.id);
        connect(remove, &QPushButton::clicked, this, &ShareGroupsPage::on_remove_group);
        groups_layout->addWidget(remove, 0, Qt::AlignRight);
    }
    content->addWidget(groups_card);

    auto* devices = section_card(tr("组内设备"), this);
    auto* devices_layout = qobject_cast<QVBoxLayout*>(devices->layout());
    QVector<QWidget*> cards;
    for (const PairedDevice& device : store.paired_devices()) {
        cards << node_card(QStringLiteral("▣"), device.name, device.platform,
                           device.online ? tr("在线") : tr("离线"), device.online,
                           {}, tr("配对设备"), devices);
    }
    if (cards.isEmpty()) {
        auto* empty = new QLabel(tr("暂无配对设备。"), devices);
        empty->setObjectName("summaryCaption");
        devices_layout->addWidget(empty);
    } else {
        auto* grid = new QGridLayout();
        grid->setSpacing(12);
        for (int i = 0; i < cards.size(); ++i) {
            grid->addWidget(cards.at(i), i / 3, i % 3);
        }
        devices_layout->addLayout(grid);
    }
    content->addWidget(devices);
}

void ShareGroupsPage::on_create_group()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("新建分享组"), tr("组名称："),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    ShareGroup group;
    group.name = name.trimmed();
    group.members << LocalStore::instance().profile().nickname;
    group.device_count = LocalStore::instance().paired_devices().size();
    LocalStore::instance().add_share_group(group);
    refresh();
}

void ShareGroupsPage::on_remove_group()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }
    LocalStore::instance().remove_share_group(button->property(kIdProperty).toString());
    refresh();
}

// ============================================================
// 分享记录
// ============================================================
ShareRecordsPage::ShareRecordsPage(QWidget* parent)
    : ProtoPage(parent)
{
    refresh();
}

void ShareRecordsPage::refresh()
{
    set_header(tr("分享记录"),
               tr("追溯每一次隔空传送与分组分享，一次性码过期后记录仍保留。"),
               tr("本机"));
    clear_content();
    build_content();
}

void ShareRecordsPage::build_content()
{
    LocalStore& store = LocalStore::instance();
    auto* content = content_layout();
    const QVector<ShareRecord> records = store.share_records();

    auto* records_card = section_card(tr("全部记录 · %1 条").arg(records.size()), this);
    auto* records_layout = qobject_cast<QVBoxLayout*>(records_card->layout());
    QStringList rows;
    for (const ShareRecord& record : records) {
        rows << record.content + QStringLiteral("\t") + record.target + QStringLiteral("\t")
             + record.method + QStringLiteral("\t") + format_timestamp(record.created_at)
             + QStringLiteral("\t") + record.status;
    }
    if (rows.isEmpty()) {
        rows << tr("暂无分享记录\t—\t—\t—\t—");
    }
    records_layout->addWidget(data_table(
        QStringList() << tr("内容") << tr("对象") << tr("方式") << tr("时间") << tr("状态"),
        rows, records_card));
    auto* clear = new QPushButton(tr("清空记录"), records_card);
    clear->setObjectName("toolButton");
    connect(clear, &QPushButton::clicked, this, &ShareRecordsPage::on_clear_records);
    records_layout->addWidget(clear, 0, Qt::AlignRight);
    content->addWidget(records_card);

    content->addWidget(notice(
        tr("一次性码传送的记录只保留「发生过」与文件信息，不保留码本身；记录保存在本机。"),
        this));
}

void ShareRecordsPage::on_clear_records()
{
    if (QMessageBox::question(this, tr("清空记录"), tr("确定清空全部分享记录？"))
        == QMessageBox::Yes) {
        LocalStore::instance().clear_share_records();
        refresh();
    }
}

} // namespace falcon::desktop::proto
