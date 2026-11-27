/**
 * @file proto_widgets.hpp
 * @brief ui-next 原型的通用 UI 组件（供 16 个新增页面复用）
 *
 * 这些组件严格复用现有 QSS objectName（sectionLabel / summaryCard /
 * summaryValue / summaryCaption / navTab / primaryButton / separatorLine）
 * 与主题图标，保证换肤自动生效、风格与既有页面一致，不需要新增样式规则。
 *
 * 组件全部是纯布局拼装，不持有业务状态；数据由调用方以 QString 传入。
 *
 * @author Falcon Team
 * @date 2026-11-27
 */

#pragma once

#include <QWidget>

#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace falcon::desktop::proto {

/** 页面标题区：大标题 + 副标题（复用 heroTitle / heroDescription） */
QWidget* page_header(const QString& title, const QString& subtitle, QWidget* parent);

/**
 * 提示条：左侧信息图标 + 一段文字（对应原型 .notice）。
 * @param accent true=强调色（暖橙顶点），false=中性信息
 */
QWidget* notice(const QString& text, QWidget* parent, bool accent = false);

/**
 * 卡片区块：白底圆角容器 + 区块标题（对应原型 .section-card）。
 * 返回卡片 widget，调用方把自己的内容 addWidget 进 returned->layout()。
 */
QWidget* section_card(const QString& title, QWidget* parent, int margin = 16);

/** 统计卡：大数字 + 说明（复用 summaryCard / summaryValue / summaryCaption） */
QWidget* stat_card(const QString& value, const QString& caption, QWidget* parent);

/**
 * 开关式导航入口卡：图标 + 标题 + 副标题，可点击（对应原型 .airdrop-card）。
 * @param clicked 接收者槽
 */
QWidget* action_card(const QString& glyph, const QString& title,
                     const QString& subtitle, QWidget* parent,
                     QObject* receiver, const char* member);

/** 步骤流程条：1 → 2 → 3（对应原型 .flow） */
QWidget* flow_steps(const QStringList& steps, QWidget* parent);

/** 只读信息行：字段名 + 值（对应原型 .detail-grid 的单格） */
QWidget* info_cell(const QString& label, const QString& value, QWidget* parent);

/** 设置行：标题 + 说明 + 右侧控件（对应原型 .setting-row） */
QWidget* setting_row(const QString& label, const QString& description,
                     QWidget* control, QWidget* parent);

/**
 * 节点卡：图标 + 名称 + 副标题 + 状态标签 + 能力标签 + 底部操作。
 * 用于「我的设备」「NAS 节点」「接收方」等卡片。
 * @param status 状态文字（在线/离线/第一期…）
 * @param status_ok true=绿色强调，false=灰色
 * @param capabilities 能力标签列表（如 WebDAV/SMB3）
 * @param footer 底部说明文字
 */
QWidget* node_card(const QString& glyph, const QString& name,
                   const QString& subtitle, const QString& status, bool status_ok,
                   const QStringList& capabilities, const QString& footer,
                   QWidget* parent);

/**
 * 数据表格：表头 + 行数据（对应原型 .tbl）。
 * @param rows 每行以 \t 分隔单元格
 */
QTableWidget* data_table(const QStringList& headers, const QStringList& rows,
                         QWidget* parent);

} // namespace falcon::desktop::proto
