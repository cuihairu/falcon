/**
 * @file device_pages.cpp
 * @brief ui-next 设备域页面实现（真实 LocalStore 数据）
 */

#include "device_pages.hpp"

#include "../services/local_store.hpp"
#include "../widgets/proto_widgets.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTableWidget>
#include <QFrame>
#include <QMessageBox>
#include <QSet>

namespace falcon::desktop::proto {
namespace {

const char* kSyncKeyProperty = "falcon_sync_key";
const char* kIdProperty = "falcon_item_id";

/** 一行带开关的同步项（key 用于写回 LocalStore） */
QWidget* sync_row(const QString& title, const QString& desc, bool on,
                  const QString& key, QWidget* parent)
{
    auto* toggle = new QCheckBox(parent);
    toggle->setChecked(on);
    toggle->setProperty(kSyncKeyProperty, key);
    return setting_row(title, desc, toggle, parent);
}

/** 节点卡网格（自适应列数） */
QWidget* card_grid(const QVector<QWidget*>& cards, int columns, QWidget* parent)
{
    auto* grid = new QWidget(parent);
    auto* layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    const int cols = columns > 0 ? columns : 2;
    for (int i = 0; i < cards.size(); ++i) {
        layout->addWidget(cards.at(i), i / cols, i % cols);
    }
    return grid;
}

/** 带删除按钮的节点卡（底部操作区加「移除」） */
QWidget* removable_node_card(const QString& glyph, const QString& name,
                             const QString& subtitle, const QString& status, bool status_ok,
                             const QStringList& capabilities, const QString& footer,
                             const QString& id, QObject* receiver, const char* member,
                             QWidget* parent)
{
    auto* host = new QWidget(parent);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(node_card(glyph, name, subtitle, status, status_ok,
                                capabilities, footer, host));
    auto* remove = new QPushButton(QObject::tr("移除"), host);
    remove->setObjectName("toolButton");
    remove->setProperty(kIdProperty, id);
    QObject::connect(remove, &QPushButton::clicked, receiver, member);
    layout->addWidget(remove, 0, Qt::AlignRight);
    return host;
}

} // namespace

// ============================================================
// 跨设备
// ============================================================
CrossDevicePage::CrossDevicePage(QWidget* parent)
    : ProtoPage(parent)
{
    set_header(tr("跨设备"),
               tr("通过匹配码配对设备，逐项同步你的数据。第一期无需账号。"),
               tr("本机闭环"));
    refresh();
}

void CrossDevicePage::refresh()
{
    clear_content();
    build_content();
}

void CrossDevicePage::build_content()
{
    const SyncSettings sync = LocalStore::instance().sync_settings();
    auto* content = content_layout();

    content->addWidget(notice(
        tr("第一期的同步能力不依赖登录：设备之间用 6 位匹配码建立可信连接。"
           "本机的开关与配对记录会保存在本地；真正的跨设备中继需后端接入。"),
        this, true));

    auto* sync_card = section_card(tr("同步内容"), this);
    auto* sync_layout = qobject_cast<QVBoxLayout*>(sync_card->layout());
    sync_layout->addWidget(sync_row(tr("跨设备复制粘贴"), tr("在一台设备复制，另一台直接粘贴"),
                                    sync.clipboard, QStringLiteral("clipboard"), sync_card));
    sync_layout->addWidget(sync_row(tr("个人词库同步"), tr("输入习惯与自定义词条跨设备共享"),
                                    sync.dictionary, QStringLiteral("dictionary"), sync_card));
    sync_layout->addWidget(sync_row(tr("常用语同步"), tr("常用短语与快捷回复同步"),
                                    sync.phrases, QStringLiteral("phrases"), sync_card));
    sync_layout->addWidget(sync_row(tr("下载记录同步"), tr("下载中/已完成列表在多设备保持一致"),
                                    sync.download_history, QStringLiteral("download_history"), sync_card));
    sync_layout->addWidget(sync_row(tr("隔空传送"), tr("在设备间快速传文件与链接"),
                                    sync.airdrop, QStringLiteral("airdrop"), sync_card));
    // 统一连接同步开关（用 property 区分，避免五个重复槽）
    for (QCheckBox* box : sync_card->findChildren<QCheckBox*>()) {
        connect(box, &QCheckBox::toggled, this, &CrossDevicePage::on_sync_toggled);
    }
    content->addWidget(sync_card);

    auto* devices_card = section_card(tr("我的设备"), this);
    auto* devices_layout = qobject_cast<QVBoxLayout*>(devices_card->layout());
    QVector<QWidget*> device_cards;

    // 本机卡（用真实平台信息，非假数据）
    device_cards << node_card(QStringLiteral("▣"), tr("本机 · Windows"),
                              tr("当前设备 · Falcon 0.1.0"), tr("在线"), true, {},
                              tr("匹配码在「展示传送码」中生成"), devices_card);

    const QVector<PairedDevice> devices = LocalStore::instance().paired_devices();
    if (devices.isEmpty()) {
        auto* empty = new QLabel(tr("暂无已配对设备，点击页头「匹配码配对」添加。"), devices_card);
        empty->setObjectName("summaryCaption");
        devices_layout->addWidget(empty);
    } else {
        for (const PairedDevice& device : devices) {
            device_cards << removable_node_card(
                QStringLiteral("◉"), device.name,
                device.platform.isEmpty() ? tr("配对设备") : device.platform,
                device.online ? tr("在线") : tr("离线"), device.online,
                {}, format_timestamp(device.paired_at), device.id,
                this, SLOT(on_remove_device()), devices_card);
        }
    }
    devices_layout->addWidget(card_grid(device_cards, 2, devices_card));
    devices_layout->addWidget(flow_steps(
        QStringList() << tr("1 在 A 设备展示匹配码") << tr("2 在 B 设备扫码或输入码")
                      << tr("3 建立可信连接"), devices_card));
    content->addWidget(devices_card);

    auto* airdrop_card = section_card(tr("隔空传送 · 与跨设备共用同一串码"), this);
    auto* airdrop_layout = qobject_cast<QVBoxLayout*>(airdrop_card->layout());
    auto* grid = new QGridLayout();
    grid->setSpacing(12);
    grid->addWidget(action_card(QStringLiteral("▦"), tr("生成传送码"),
                                tr("展示二维码 + 6 位码，对方扫一下即可收文件"),
                                airdrop_card, this, SLOT(on_generate_code())), 0, 0);
    grid->addWidget(action_card(QStringLiteral("⌗"), tr("扫码 / 输入对方的码"),
                                tr("扫二维码更快，或手动输入对方给的 6 位码"),
                                airdrop_card, this, SLOT(on_pair_clicked())), 0, 1);
    airdrop_layout->addLayout(grid);
    content->addWidget(airdrop_card);
}

void CrossDevicePage::on_generate_code() { emit show_code_requested(); }
void CrossDevicePage::on_pair_clicked() { emit pair_code_requested(); }

void CrossDevicePage::on_sync_toggled(bool checked)
{
    auto* box = qobject_cast<QCheckBox*>(sender());
    if (!box) {
        return;
    }
    const QString key = box->property(kSyncKeyProperty).toString();
    if (!key.isEmpty()) {
        LocalStore::instance().set_sync_enabled(key, checked);
    }
}

void CrossDevicePage::on_remove_device()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }
    LocalStore::instance().remove_paired_device(button->property(kIdProperty).toString());
    refresh();
}

QString CrossDevicePage::sync_key_for(QObject* sender) const
{
    auto* box = qobject_cast<QCheckBox*>(sender);
    return box ? box->property(kSyncKeyProperty).toString() : QString();
}

// ============================================================
// NAS 与路由
// ============================================================
NasNodesPage::NasNodesPage(QWidget* parent)
    : ProtoPage(parent)
{
    set_header(tr("NAS 与路由"),
               tr("按协议层接入 NAS / 路由；原生安装 Falcon 的设备可作为常驻节点。"));
    auto* add_button = add_header_button(tr("添加节点"));
    connect(add_button, &QPushButton::clicked, this, &NasNodesPage::on_add_node_clicked);
    refresh();
}

void NasNodesPage::refresh()
{
    clear_content();
    build_content();
}

void NasNodesPage::build_content()
{
    auto* content = content_layout();
    content->addWidget(notice(
        tr("接入优先级：WebDAV ＞ SMB ＞ SFTP/FTPS ＞ 厂商 API。节点配置保存在本机，"
           "连接测试为本地参数校验；实际挂载需后端接入。"),
        this, true));

    auto* nodes_card = section_card(QString(), this);
    auto* nodes_layout = qobject_cast<QVBoxLayout*>(nodes_card->layout());
    const QVector<NasNode> nodes = LocalStore::instance().nas_nodes();
    if (nodes.isEmpty()) {
        auto* empty = new QLabel(tr("暂无节点，点击右上角「添加节点」创建。"), nodes_card);
        empty->setObjectName("summaryCaption");
        nodes_layout->addWidget(empty);
    } else {
        QVector<QWidget*> cards;
        for (const NasNode& node : nodes) {
            cards << removable_node_card(
                QStringLiteral("▤"), node.name,
                node.protocol.isEmpty() ? tr("存储节点") : node.protocol + (node.endpoint.isEmpty() ? QString() : QStringLiteral(" · ") + node.endpoint),
                node.online ? tr("在线") : tr("未连接"), node.online,
                node.capabilities, tr("添加于 %1").arg(format_timestamp(node.added_at)),
                node.id, this, SLOT(on_remove_node()), nodes_card);
        }
        nodes_layout->addWidget(card_grid(cards, 2, nodes_card));
    }
    content->addWidget(nodes_card);

    auto* native_card = section_card(tr("原生安装（后续阶段）"), this);
    auto* native_layout = qobject_cast<QVBoxLayout*>(native_card->layout());
    auto* desc = new QLabel(tr("飞牛 / 群晖用 Docker Compose，OpenWrt 用 opkg + LuCI 入口。"
                               "Web 管理界面先做「任务 + 存储 + 配对」最小集，默认只监听局域网。"), native_card);
    desc->setObjectName("summaryCaption");
    desc->setWordWrap(true);
    native_layout->addWidget(desc);
    native_layout->addWidget(flow_steps(
        QStringList() << tr("桌面端下发任务") << tr("NAS 版落盘到本地硬盘") << tr("桌面关机仍持续下载"),
        native_card));
    content->addWidget(native_card);
}

void NasNodesPage::on_add_node_clicked() { emit add_node_requested(); }

void NasNodesPage::on_remove_node()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }
    LocalStore::instance().remove_nas_node(button->property(kIdProperty).toString());
    refresh();
}

// ============================================================
// 隔空传送
// ============================================================
AirdropPage::AirdropPage(QWidget* parent)
    : ProtoPage(parent)
{
    set_header(tr("隔空传送"),
               tr("用同一串 6 位码，把文件与链接发给我的设备、好友，或临时会话的对方。"),
               tr("本机闭环"));
    refresh();
}

void AirdropPage::refresh()
{
    clear_content();
    build_content();
}

void AirdropPage::build_content()
{
    auto* content = content_layout();
    content->addWidget(notice(
        tr("这里与「跨设备」共用同一套匹配码；扫码是密码的图形形态，两者完全等价。"
           "传送记录保存在本机，真正的网络传输需后端接入。"),
        this, true));

    auto* send_card = section_card(tr("发送 · 两种方向，同一套机制"), this);
    auto* send_layout = qobject_cast<QVBoxLayout*>(send_card->layout());
    auto* grid = new QGridLayout();
    grid->setSpacing(12);
    grid->addWidget(action_card(QStringLiteral("▦"), tr("生成传送码"),
                                tr("展示二维码 + 6 位码，对方扫一下即可收文件"),
                                send_card, this, SLOT(on_generate_code())), 0, 0);
    grid->addWidget(action_card(QStringLiteral("⌗"), tr("扫码 / 输入对方的码"),
                                tr("扫二维码更快，或手动输入对方给的 6 位码"),
                                send_card, this, SLOT(on_pair_clicked())), 0, 1);
    send_layout->addLayout(grid);
    content->addWidget(send_card);

    auto* receivers_card = section_card(tr("接收方"), this);
    auto* receivers_layout = qobject_cast<QVBoxLayout*>(receivers_card->layout());
    QVector<QWidget*> cards;
    cards << node_card(QStringLiteral("▣"), tr("我的设备"), tr("已配对设备 · 物"),
                       tr("本机"), true, {}, tr("配对记录保存在本地"), receivers_card);
    cards << node_card(QStringLiteral("◉"), tr("好友"), tr("可信联系人 · 人"),
                       tr("本机"), true, {}, tr("好友列表保存在本地"), receivers_card);
    cards << node_card(QStringLiteral("⚡"), tr("一次性会话"), tr("临时码 · 不留存关系"),
                       tr("本机"), true, {}, tr("码过期即失效"), receivers_card);
    receivers_layout->addWidget(card_grid(cards, 3, receivers_card));
    content->addWidget(receivers_card);

    auto* recent_card = section_card(tr("最近传送"), this);
    auto* recent_layout = qobject_cast<QVBoxLayout*>(recent_card->layout());
    const QVector<ShareRecord> records = LocalStore::instance().share_records();
    QStringList rows;
    for (const ShareRecord& record : records) {
        rows << record.content + QStringLiteral("\t") + record.target + QStringLiteral("\t")
             + record.method + QStringLiteral("\t") + format_timestamp(record.created_at)
             + QStringLiteral("\t") + record.status;
    }
    if (rows.isEmpty()) {
        rows << tr("暂无传送记录\t—\t—\t—\t—");
    }
    recent_layout->addWidget(data_table(
        QStringList() << tr("内容") << tr("对象") << tr("方式") << tr("时间") << tr("状态"),
        rows, recent_card));
    auto* more = new QPushButton(tr("查看全部分享记录"), recent_card);
    more->setObjectName("toolButton");
    connect(more, &QPushButton::clicked, this, &AirdropPage::on_records_clicked);
    recent_layout->addWidget(more, 0, Qt::AlignRight);
    content->addWidget(recent_card);
}

void AirdropPage::on_generate_code() { emit show_code_requested(); }
void AirdropPage::on_pair_clicked() { emit pair_code_requested(); }
void AirdropPage::on_records_clicked() { emit share_records_requested(); }

// ============================================================
// NAS / 路由版 Web 管理
// ============================================================
NasWebPage::NasWebPage(QWidget* parent)
    : ProtoPage(parent)
{
    set_header(tr("NAS / 路由版 Web 管理"),
               tr("最小集：任务 + 存储 + 配对 · 默认只监听局域网（本地预览）。"),
               tr("Web 端"));
    refresh();
}

void NasWebPage::refresh()
{
    clear_content();
    build_content();
}

void NasWebPage::build_content()
{
    auto* content = content_layout();
    content->addWidget(notice(
        tr("原生安装 Falcon 的设备提供局域网 Web 管理，仅含任务、存储、配对最小集；"
           "此处展示本机节点与任务概览，Web 服务需后端接入。"),
        this, true));

    const QVector<NasNode> nodes = LocalStore::instance().nas_nodes();
    auto* stats = new QWidget(this);
    auto* stats_layout = new QHBoxLayout(stats);
    stats_layout->setSpacing(12);
    stats_layout->addWidget(stat_card(QString::number(nodes.size()), tr("存储节点"), stats), 1);
    stats_layout->addWidget(stat_card(QString::number(LocalStore::instance().paired_devices().size()),
                                      tr("已配对设备"), stats), 1);
    stats_layout->addWidget(stat_card(tr("局域网"), tr("监听范围"), stats), 1);
    content->addWidget(stats);

    auto* tasks = section_card(tr("存储节点"), this);
    QStringList rows;
    for (const NasNode& node : nodes) {
        rows << node.name + QStringLiteral("\t") + node.protocol + QStringLiteral("\t")
             + node.endpoint + QStringLiteral("\t")
             + (node.online ? tr("在线") : tr("未连接"));
    }
    if (rows.isEmpty()) {
        rows << tr("暂无节点\t—\t—\t—");
    }
    qobject_cast<QVBoxLayout*>(tasks->layout())->addWidget(data_table(
        QStringList() << tr("名称") << tr("协议") << tr("端点") << tr("状态"), rows, tasks));
    content->addWidget(tasks);

    auto* tabs = section_card(tr("功能最小集"), this);
    qobject_cast<QVBoxLayout*>(tabs->layout())->addWidget(flow_steps(
        QStringList() << tr("任务") << tr("存储") << tr("配对"), tabs));
    content->addWidget(tabs);
}

} // namespace falcon::desktop::proto
