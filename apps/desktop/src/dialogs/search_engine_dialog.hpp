/**
 * @file search_engine_dialog.hpp
 * @brief 搜索引擎添加/编辑表单对话框（engines.json 条目的结构化编辑）
 * @author Falcon Team
 * @date 2026-10-07
 */

#pragma once

#include "../services/search_engine_catalog.hpp"

#include <QDialog>

#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace falcon::desktop {

/**
 * @brief 单个搜索引擎的添加/编辑表单
 *
 * 表单字段与 drives ResourceSearchManager 的加载语义一一对应：
 *   - 关键词经 params 中留空的 q/search/keyword 键自动填入
 *   - response_format=html 时必须提供 selectors.item（否则解析恒零结果）
 * OK 前做内联校验（必填/URL 格式/重名/参数行格式/正则合法性），
 * 校验失败显示错误标签并拒绝关闭。
 */
class SearchEngineDialog : public QDialog
{
    Q_OBJECT

public:
    /// existing_names 为重名校验集合（编辑时应排除自身旧名）；
    /// initial 为空 CatalogEngine 时即添加形态
    SearchEngineDialog(const QString& title, const CatalogEngine& initial,
                       const QStringList& existing_names,
                       QWidget* parent = nullptr);

    /// 校验通过后（accept）取表单值
    CatalogEngine engine() const;

private:
    bool validate(QString* error) const;

    QLabel* error_label_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* base_url_edit_ = nullptr;
    QLineEdit* search_path_edit_ = nullptr;
    QComboBox* format_combo_ = nullptr;
    QSpinBox* delay_spin_ = nullptr;
    QCheckBox* enabled_check_ = nullptr;
    QPlainTextEdit* params_edit_ = nullptr;
    QLineEdit* selector_item_ = nullptr;
    QLineEdit* selector_title_ = nullptr;
    QLineEdit* selector_url_ = nullptr;
    QLineEdit* selector_size_ = nullptr;
    QLineEdit* selector_seeds_ = nullptr;

    QStringList existing_names_;
};

} // namespace falcon::desktop
