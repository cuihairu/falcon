/**
 * @file device_pages.hpp
 * @brief ui-next 设备域页面：跨设备 / NAS 与路由 / 隔空传送 / NAS Web
 *
 * 数据来自 LocalStore（本机持久化），配对码 / 传送码为本机闭环演示；
 * 真正的跨设备中继需后端，UI 明确标注边界，不冒充联网能力。
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include "proto_page.hpp"

#include <QStringList>
#include <QVector>

namespace falcon::desktop::proto {

/** 跨设备：5 项同步开关 + 我的设备 + 隔空传送入口 */
class CrossDevicePage : public ProtoPage
{
    Q_OBJECT
public:
    explicit CrossDevicePage(QWidget* parent = nullptr);

    /** 从 LocalStore 重建内容（同步开关 / 已配对设备） */
    void refresh();

signals:
    void show_code_requested();
    void pair_code_requested();
    void airdrop_requested();

private slots:
    void on_generate_code();
    void on_pair_clicked();
    void on_sync_toggled(bool checked);
    void on_remove_device();

private:
    void build_content();
    QString sync_key_for(QObject* sender) const;
};

/** NAS 与路由：节点卡列表 + 原生安装说明 */
class NasNodesPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit NasNodesPage(QWidget* parent = nullptr);

    void refresh();

signals:
    void add_node_requested();

private slots:
    void on_add_node_clicked();
    void on_remove_node();

private:
    void build_content();
};

/** 隔空传送：发送入口 + 接收方 + 最近传送 */
class AirdropPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit AirdropPage(QWidget* parent = nullptr);

    void refresh();

signals:
    void show_code_requested();
    void pair_code_requested();
    void share_records_requested();

private slots:
    void on_generate_code();
    void on_pair_clicked();
    void on_records_clicked();

private:
    void build_content();
};

/** NAS / 路由版 Web 管理（屏23）：任务 + 存储 + 配对最小集（本地预览） */
class NasWebPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit NasWebPage(QWidget* parent = nullptr);

    void refresh();

private:
    void build_content();
};

} // namespace falcon::desktop::proto
