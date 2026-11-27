/**
 * @file proto_page.hpp
 * @brief ui-next 原型页面基类：统一页头 + 可滚动内容区
 *
 * 所有新增页面（跨设备 / NAS / 账号 / 好友 / 隔空传送 / 分享组 /
 * 分享记录 / NAS Web）继承此类，只需在构造里往 content_layout() 添加
 * proto_widgets 组件。页头徽标、滚动区行为统一，避免每个页面重复代码。
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include <QWidget>
#include <QHBoxLayout>

#include <QString>

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace falcon::desktop::proto {

class ProtoPage : public QWidget
{
    Q_OBJECT

public:
    explicit ProtoPage(QWidget* parent = nullptr);
    ~ProtoPage() override = default;

    /**
     * 初始化页头。子类构造中调用一次。
     * @param title 大标题
     * @param subtitle 副标题
     * @param badge 右上角状态徽标文字（空=不显示），如「后续阶段」「第一期」
     * @param badge_warning true=警示色徽标（sectionLabel 样式），false=普通
     */
    void set_header(const QString& title, const QString& subtitle,
                    const QString& badge = QString(), bool badge_warning = false);

    /** 页头右侧追加一个主按钮（如「添加节点」「添加好友」），返回按钮供连接 */
    QPushButton* add_header_button(const QString& text);

    /** 内容区布局：子类往其中 addWidget / addStretch */
    QVBoxLayout* content_layout() const { return content_layout_; }

    /** 清空内容区所有 widget / 布局（供数据变化后 refresh 重建；页头不受影响） */
    void clear_content();

private:
    void setup_ui();

    QVBoxLayout* content_layout_ = nullptr;   // 滚动内容
    QWidget* header_host_ = nullptr;          // 页头容器
    QVBoxLayout* header_layout_ = nullptr;    // 页头内部（标题副标题区）
    QWidget* badge_ = nullptr;                // 右上角徽标
    QLabel* badge_label_ = nullptr;
    QHBoxLayout* header_top_ = nullptr;       // 页头顶行：标题区 + 徽标/按钮
};

} // namespace falcon::desktop::proto
