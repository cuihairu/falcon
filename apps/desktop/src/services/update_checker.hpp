/**
 * @file update_checker.hpp
 * @brief GitHub Releases 检查更新服务（无自更新，提供发布页链接）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace falcon::desktop {

/**
 * @brief 检查 GitHub Releases 最新版本
 *
 * 请求 `https://api.github.com/repos/cuihairu/falcon/releases/latest`，
 * 解析 tag_name（剥前导 v 后按语义化版本与编译期 FALCON_VERSION_STRING
 * 比较），有新版时给出 release 页链接——不自动下载安装（aria2 同姿态，
 * 由用户前往发布页取包）。
 */
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);
    ~UpdateChecker() override;

    /** 异步发起检查（进行中重入忽略）；结果经信号返回 */
    void check_for_updates();

    bool checking() const { return checking_; }

    /** 编译期版本号（<falcon/version.hpp> 单一事实源） */
    static QString current_version();

    /**
     * tag（"v1.2.3"）与当前版本比较，仅当严格更新时为真。
     * 公开静态纯函数（无网络依赖）：版本比较语义是检查更新唯一的
     * 可离线测试面，公开供单测直调（request_refresh 提升同先例）。
     */
    static bool is_newer_tag_version(const QString& tag);

signals:
    /** 有更新：latest_version 已剥 v 前缀 */
    void update_available(const QString& latest_version, const QString& release_url);
    void up_to_date();
    void check_failed(const QString& reason);

private:
    QNetworkAccessManager* nam_;
    bool checking_;
};

} // namespace falcon::desktop
