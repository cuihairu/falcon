/**
 * @file social_pages.hpp
 * @brief ui-next 社交域页面：账号 / 好友 / 分享组 / 分享记录（真实 LocalStore 数据）
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include "proto_page.hpp"

namespace falcon::desktop::proto {

/** 账号：资料卡 + 统计 + 账号安全 + 演进路径 + 关系能力入口 */
class AccountPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit AccountPage(QWidget* parent = nullptr);

    void refresh();

signals:
    void edit_profile_requested();
    void friends_requested();
    void share_groups_requested();
    void share_records_requested();
    void login_requested();

private slots:
    void on_edit_profile();
    void on_friends();
    void on_share_groups();
    void on_share_records();
    void on_login();
    void on_logout();

private:
    void build_content();
};

/** 好友：请求处理 + 好友列表 + 添加好友 */
class FriendsPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit FriendsPage(QWidget* parent = nullptr);

    void refresh();

signals:
    void pair_code_requested();

private slots:
    void on_add_friend();
    void on_accept_request();
    void on_reject_request();
    void on_remove_friend();
    void on_transfer();

private:
    void build_content();
    bool header_button_created_ = false;
};

/** 分享组：组列表 + 新建 + 组内设备 */
class ShareGroupsPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit ShareGroupsPage(QWidget* parent = nullptr);

    void refresh();

private slots:
    void on_create_group();
    void on_remove_group();

private:
    void build_content();
    bool header_button_created_ = false;
};

/** 分享记录：全部记录表 + 清空 */
class ShareRecordsPage : public ProtoPage
{
    Q_OBJECT
public:
    explicit ShareRecordsPage(QWidget* parent = nullptr);

    void refresh();

private slots:
    void on_clear_records();

private:
    void build_content();
};

} // namespace falcon::desktop::proto
