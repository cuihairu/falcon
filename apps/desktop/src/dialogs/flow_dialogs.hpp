/**
 * @file flow_dialogs.hpp
 * @brief ui-next 六个弹层对话框（真实本地动作）
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include <QDialog>

#include <rpc/aria2_snapshots.hpp>

#include <QString>
#include <QStringList>

namespace falcon::desktop::proto {

/** 文件详情（屏21）：读 TaskSnapshot 真实字段；无数据源显示「—」 */
class TaskDetailDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TaskDetailDialog(const falcon::daemon::rpc::TaskSnapshot& snapshot,
                              QWidget* parent = nullptr);

signals:
    void add_to_cloud_requested();
};

/** 展示传送码（屏19）：6 位码 + 120s 倒计时 + 复制/刷新（本地码库闭环） */
class ShowCodeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ShowCodeDialog(QWidget* parent = nullptr);

private slots:
    void refresh_code();
    void copy_code();
    void tick_countdown();

private:
    void regenerate();

    void update_qr();

    class QLabel* qr_label_;
    class QLabel* code_label_;
    class QLabel* countdown_label_;
    class QTimer* timer_;
    int remaining_seconds_ = 120;
    QString code_;
};

/** 匹配码配对（屏18）：扫码/手输切换 + 6 位本地校验 */
class PairCodeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PairCodeDialog(QWidget* parent = nullptr);

signals:
    /** code 已通过本地校验（未过期），由 MainWindow 落库为配对设备/好友 */
    void connect_requested(const QString& code);

private slots:
    void set_scan_mode();
    void set_manual_mode();
    void emit_connect();
    void paste_from_clipboard();

private:
    class QWidget* scan_pane_;
    class QWidget* manual_pane_;
    class QLineEdit* code_edit_;
    class QLineEdit* scan_code_edit_;
    class QPushButton* scan_tab_;
    class QPushButton* manual_tab_;
};

/** 添加 NAS 节点表单数据 */
struct NodeDraft {
    QString name;
    QString protocol;
    QString endpoint;
    QString access_key;
    QString secret_key;
    QString region;
};

/** 添加 NAS 节点（屏20）：预设 + 协议参数（本地保存 + 参数校验） */
class AddNodeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddNodeDialog(QWidget* parent = nullptr);

signals:
    void test_connection_requested(const NodeDraft& draft);
    void save_requested(const NodeDraft& draft);

private slots:
    void apply_preset(const QString& preset);
    void emit_test();
    void emit_save();

private:
    class QLineEdit* name_edit_ = nullptr;
    class QLineEdit* protocol_edit_ = nullptr;
    class QLineEdit* endpoint_edit_ = nullptr;
    class QLineEdit* access_key_edit_ = nullptr;
    class QLineEdit* secret_key_edit_ = nullptr;
    class QLineEdit* region_edit_ = nullptr;

    NodeDraft draft() const;
};

/** 登录（本地会话）：手机号 + 本地验证码闭环 */
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);

signals:
    void login_succeeded(const QString& phone);

private slots:
    void request_code();
    void emit_login();

private:
    class QLineEdit* phone_edit_ = nullptr;
    class QLineEdit* code_edit_ = nullptr;
    class QLabel* code_hint_ = nullptr;
};

/** 编辑资料（屏22）：昵称本地持久化 */
class EditProfileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit EditProfileDialog(QWidget* parent = nullptr);

private slots:
    void emit_save();

private:
    class QLineEdit* nickname_edit_ = nullptr;
};

} // namespace falcon::desktop::proto
