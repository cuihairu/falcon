/**
 * @file search_engine_dialog.cpp
 * @brief 搜索引擎添加/编辑表单对话框实现
 * @author Falcon Team
 * @date 2026-10-07
 */

#include "search_engine_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <regex>
#include <vector>

namespace falcon::desktop {

namespace {

/// "key=value" 行解析（value 可为空——留空的 q/search/keyword 由
/// drives 侧自动填入关键词）；格式非法返回 false
bool parse_params_text(const QString& text,
                       std::map<std::string, std::string>& out,
                       QString* error)
{
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0) {
            if (error) {
                *error = QObject::tr("查询参数第 %1 行缺少「键=值」格式： %2")
                             .arg(i + 1)
                             .arg(line);
            }
            return false;
        }
        out[line.left(eq).trimmed().toStdString()] =
            line.mid(eq + 1).trimmed().toStdString();
    }
    return true;
}

/// 选择器正则合法性预检（与 drives parse_html_by_selectors 同
/// ECMAScript 方言——这里挡住的错误不必等搜索失败才发现）
bool check_regex(const QString& pattern, QString* error, const char* field)
{
    if (pattern.isEmpty()) {
        return true; // 空选择器合法（drives 侧跳过）
    }
    try {
        std::regex re(pattern.toStdString(), std::regex::ECMAScript);
        (void)re;
        return true;
    } catch (const std::regex_error& e) {
        *error = QObject::tr("%1 正则无效: %2")
                     .arg(QString::fromUtf8(field),
                          QString::fromUtf8(e.what()));
        return false;
    }
}

} // namespace

SearchEngineDialog::SearchEngineDialog(const QString& title,
                                       const CatalogEngine& initial,
                                       const QStringList& existing_names,
                                       QWidget* parent)
    : QDialog(parent)
    , existing_names_(existing_names)
{
    setWindowTitle(title);
    setModal(true);
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    // 内联错误区（校验失败显示，隐藏时不占位）
    error_label_ = new QLabel(this);
    error_label_->setWordWrap(true);
    error_label_->setObjectName("errorLabel");
    error_label_->setStyleSheet("color: #c42b1c;");
    error_label_->hide();
    layout->addWidget(error_label_);

    auto* form = new QFormLayout;
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignRight);

    name_edit_ = new QLineEdit(QString::fromStdString(initial.name), this);
    name_edit_->setPlaceholderText(tr("例: 我的资源站"));
    name_edit_->setToolTip(tr("引擎唯一名；发现页结果按此名显示来源"));
    form->addRow(tr("名称:"), name_edit_);

    base_url_edit_ = new QLineEdit(QString::fromStdString(initial.base_url), this);
    base_url_edit_->setPlaceholderText(tr("https://example.com"));
    base_url_edit_->setToolTip(tr("站点根地址，必须以 http:// 或 https:// 开头"));
    form->addRow(tr("站点地址:"), base_url_edit_);

    search_path_edit_ =
        new QLineEdit(QString::fromStdString(initial.search_path), this);
    search_path_edit_->setPlaceholderText(tr("/search"));
    search_path_edit_->setToolTip(
        tr("搜索路径，与站点地址拼接；关键词经下方查询参数传入"));
    form->addRow(tr("搜索路径:"), search_path_edit_);

    format_combo_ = new QComboBox(this);
    format_combo_->addItem("html");
    format_combo_->addItem("json");
    format_combo_->setCurrentText(
        initial.response_format.empty() ? "html"
                                        : QString::fromStdString(initial.response_format));
    format_combo_->setToolTip(tr("html 按正则选择器解析结果页，json 按标准结果数组解析"));
    form->addRow(tr("返回格式:"), format_combo_);

    delay_spin_ = new QSpinBox(this);
    delay_spin_->setRange(0, 60000);
    delay_spin_->setSuffix(tr(" ms"));
    delay_spin_->setValue(initial.delay_ms);
    delay_spin_->setToolTip(tr("与上一引擎请求的最小间隔（防站点封禁），0 为不限"));
    form->addRow(tr("请求间隔:"), delay_spin_);

    enabled_check_ = new QCheckBox(tr("添加后立即启用"), this);
    enabled_check_->setChecked(initial.name.empty() ? true : initial.enabled);
    form->addRow(QString(), enabled_check_);

    layout->addLayout(form);

    // 查询参数
    auto* params_label = new QLabel(tr("查询参数（每行 键=值；值留空的 q/search/keyword 自动填入搜索关键词）:"), this);
    params_label->setWordWrap(true);
    layout->addWidget(params_label);
    params_edit_ = new QPlainTextEdit(this);
    params_edit_->setPlaceholderText(tr("q=\ntype=video"));
    QString params_text;
    for (const auto& [k, v] : initial.params) {
        if (!params_text.isEmpty()) {
            params_text += '\n';
        }
        params_text += QString::fromStdString(k) + "=" +
                       QString::fromStdString(v);
    }
    params_edit_->setPlainText(params_text);
    params_edit_->setFixedHeight(64);
    layout->addWidget(params_edit_);

    // HTML 选择器
    auto* selector_hint = new QLabel(
        tr("HTML 结果选择器（仅 html 格式生效；每条结果先按 item 匹配，其余字段在 item 内取捕获组 1）:"),
        this);
    selector_hint->setWordWrap(true);
    layout->addWidget(selector_hint);

    auto* selector_form = new QFormLayout;
    selector_form->setSpacing(8);
    selector_form->setLabelAlignment(Qt::AlignRight);

    auto make_selector_row = [this, selector_form](const QString& label_key,
                                                   QLineEdit** edit,
                                                   const QString& placeholder,
                                                   const std::string& initial_value) {
        *edit = new QLineEdit(QString::fromStdString(initial_value), this);
        (*edit)->setPlaceholderText(placeholder);
        selector_form->addRow(label_key, *edit);
    };
    make_selector_row(tr("item:"), &selector_item_,
                      tr("<a class=\"result\" href=\"([^\"]+)\""),
                      initial.selectors.count("item")
                          ? initial.selectors.at("item") : std::string{});
    make_selector_row(tr("title:"), &selector_title_,
                      tr(">\\s*([^<]+?)\\s*<"),
                      initial.selectors.count("title")
                          ? initial.selectors.at("title") : std::string{});
    make_selector_row(tr("url:"), &selector_url_,
                      tr("href=\"([^\"]+)\""),
                      initial.selectors.count("url")
                          ? initial.selectors.at("url") : std::string{});
    make_selector_row(tr("size:"), &selector_size_,
                      tr("([\\d.]+)\\s*[GM]B"),
                      initial.selectors.count("size")
                          ? initial.selectors.at("size") : std::string{});
    make_selector_row(tr("seeds:"), &selector_seeds_,
                      tr("(\\d+)"),
                      initial.selectors.count("seeds")
                          ? initial.selectors.at("seeds") : std::string{});
    layout->addLayout(selector_form);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QString error;
        if (!validate(&error)) {
            error_label_->setText(error);
            error_label_->show();
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool SearchEngineDialog::validate(QString* error) const
{
    const QString name = name_edit_->text().trimmed();
    const QString base_url = base_url_edit_->text().trimmed();

    if (name.isEmpty()) {
        *error = tr("名称不能为空。");
        return false;
    }
    if (existing_names_.contains(name, Qt::CaseSensitive)) {
        *error = tr("名称「%1」已存在，请换一个。").arg(name);
        return false;
    }
    if (!base_url.startsWith("http://") && !base_url.startsWith("https://")) {
        *error = tr("站点地址必须以 http:// 或 https:// 开头。");
        return false;
    }

    std::map<std::string, std::string> params;
    if (!parse_params_text(params_edit_->toPlainText(), params, error)) {
        return false;
    }

    const bool is_html = format_combo_->currentText() == "html";
    if (is_html && selector_item_->text().trimmed().isEmpty()) {
        *error = tr("html 格式必须填写 item 选择器（否则解析不到任何结果）,"
                   "或将返回格式切换为 json。");
        return false;
    }
    if (!check_regex(selector_item_->text(), error, "item")
        || !check_regex(selector_title_->text(), error, "title")
        || !check_regex(selector_url_->text(), error, "url")
        || !check_regex(selector_size_->text(), error, "size")
        || !check_regex(selector_seeds_->text(), error, "seeds")) {
        return false;
    }
    return true;
}

CatalogEngine SearchEngineDialog::engine() const
{
    CatalogEngine out;
    out.name = name_edit_->text().trimmed().toStdString();
    out.base_url = base_url_edit_->text().trimmed().toStdString();
    out.search_path = search_path_edit_->text().trimmed().toStdString();
    out.response_format = format_combo_->currentText().toStdString();
    out.delay_ms = delay_spin_->value();
    out.enabled = enabled_check_->isChecked();
    parse_params_text(params_edit_->toPlainText(), out.params, nullptr);
    const auto put = [&out](const char* key, const QLineEdit* edit) {
        const QString text = edit->text().trimmed();
        if (!text.isEmpty()) {
            out.selectors[key] = text.toStdString();
        }
    };
    put("item", selector_item_);
    put("title", selector_title_);
    put("url", selector_url_);
    put("size", selector_size_);
    put("seeds", selector_seeds_);
    return out;
}

} // namespace falcon::desktop
