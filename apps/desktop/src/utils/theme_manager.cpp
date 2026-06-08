/**
 * @file theme_manager.cpp
 * @brief 主题管理器实现
 * @author Falcon Team
 * @date 2026-04-22
 */

#include "theme_manager.hpp"
#include <QApplication>
#include <QFile>
#include <QTextStream>

namespace falcon::desktop {

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
    , current_theme_(ThemeType::Dark)
{
}

ThemeManager::~ThemeManager() = default;

void ThemeManager::set_theme(ThemeType theme)
{
    const bool changed = current_theme_ != theme;
    current_theme_ = theme;
    apply_stylesheet();
    if (changed) {
        emit theme_changed(theme);
    }
}

void ThemeManager::toggle_theme()
{
    set_theme(current_theme_ == ThemeType::Light ? ThemeType::Dark : ThemeType::Light);
}

QString ThemeManager::get_stylesheet() const
{
    return current_theme_ == ThemeType::Light ? load_light_theme() : load_dark_theme();
}

void ThemeManager::apply_stylesheet()
{
    qApp->setStyleSheet(get_stylesheet());
}

QString ThemeManager::theme_name(ThemeType theme)
{
    switch (theme) {
        case ThemeType::Light:
            return tr("亮色");
        case ThemeType::Dark:
            return tr("暗色");
    }
    return QString();
}

QString ThemeManager::load_light_theme() const
{
    return R"(
/* ========== 亮色主题 ========== */

/* 全局设置 */
QWidget {
    background-color: #f3f6fb;
    color: #1f2937;
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 9pt;
}

/* 主窗口 */
QMainWindow {
    background-color: #e9eef6;
}

/* 中心部件（用于无边框透明窗口的背景） */
#centralWidget {
    background-color: #e9eef6;
}

/* 标题栏 */
#topBar {
    background-color: #fcfdff;
    border-bottom: 1px solid #d9e2ef;
}

#brandMark {
    background-color: #1d4ed8;
    color: #ffffff;
    border-radius: 10px;
    font-size: 10pt;
    font-weight: bold;
    padding: 4px 8px;
}

#brandTitle {
    color: #111827;
    font-size: 12pt;
    font-weight: bold;
}

#brandSubtitle {
    color: #6b7280;
    font-size: 8pt;
}

#searchEdit {
    background-color: #eef3f9;
    color: #111827;
    border: 1px solid #d6deea;
    border-radius: 18px;
    padding: 8px 14px;
}

#searchEdit:focus {
    background-color: #ffffff;
    border-color: #60a5fa;
}

#titleMetaBadge {
    background-color: #e8f1ff;
    color: #2563eb;
    border: 1px solid #c9dcff;
    border-radius: 11px;
    padding: 3px 10px;
    font-size: 8pt;
    font-weight: bold;
}

#pageSurface {
    background-color: #f8fbff;
    border: 1px solid #dbe5f1;
    border-radius: 20px;
}

#downloadHero {
    background-color: #fdfefe;
    border: 1px solid #dbe5f1;
    border-radius: 18px;
}

#heroEyebrow {
    color: #2563eb;
    font-size: 8pt;
    font-weight: bold;
}

#heroTitle {
    color: #0f172a;
    font-size: 18pt;
    font-weight: bold;
}

#heroDescription {
    color: #64748b;
    font-size: 9pt;
}

#summaryCard {
    background-color: #ffffff;
    border: 1px solid #dde6f2;
    border-radius: 16px;
}

#summaryValue {
    color: #0f172a;
    font-size: 16pt;
    font-weight: bold;
}

#summaryCaption {
    color: #64748b;
    font-size: 8pt;
}

QPushButton {
    background-color: #f4f7fb;
    color: #1f2937;
    border: 1px solid #d6deea;
    border-radius: 10px;
    padding: 6px 12px;
    min-width: 60px;
}

QPushButton:hover {
    background-color: #edf3fb;
    border-color: #bfcee2;
}

QPushButton:pressed {
    background-color: #e5edf8;
}

QPushButton#primaryButton {
    background-color: #2563eb;
    color: #ffffff;
    border: none;
    font-weight: bold;
}

QPushButton#primaryButton:hover {
    background-color: #1d4ed8;
}

QPushButton#secondaryButton {
    background-color: #ffffff;
    color: #1d4ed8;
    border: 1px solid #bfd5ff;
    font-weight: bold;
}

QPushButton#toolButton {
    background-color: #f4f7fb;
    color: #334155;
    border: 1px solid #d6deea;
    border-radius: 10px;
}

QPushButton#toolButton:hover {
    background-color: #ffffff;
    border-color: #bfd5ff;
}

QPushButton#navTab {
    background-color: transparent;
    color: #475569;
    border: none;
    border-radius: 12px;
    padding: 10px 12px;
    text-align: left;
    font-weight: 500;
}

QPushButton#navTab:hover {
    background-color: #edf3fb;
}

QPushButton#navTab:checked {
    background-color: #e6f0ff;
    color: #1d4ed8;
    font-weight: bold;
}

#sectionLabel {
    color: #94a3b8;
    font-size: 8pt;
    font-weight: bold;
    text-transform: uppercase;
}

#sideBar {
    background-color: #f7faff;
    border-right: 1px solid #dbe5f1;
}

#sideBarFooter {
    background-color: #ffffff;
    border: 1px solid #dde6f2;
    border-radius: 16px;
}

#sidebarStatValue {
    color: #111827;
    font-size: 14pt;
    font-weight: bold;
}

#sidebarStatLabel {
    color: #64748b;
    font-size: 8pt;
}

#separatorLine {
    background-color: #e2e8f0;
    min-height: 1px;
}

#downloadToolbar {
    background-color: transparent;
}

#taskTable {
    background-color: #ffffff;
    alternate-background-color: #f8fbff;
    border: 1px solid #dbe5f1;
    border-radius: 16px;
    gridline-color: transparent;
    selection-background-color: #e9f2ff;
    selection-color: #0f172a;
}

#taskTable::item {
    padding: 10px 8px;
    border-bottom: 1px solid #edf2f7;
}

#taskTable::item:selected {
    background-color: #e9f2ff;
}

QHeaderView::section {
    background-color: #f8fbff;
    color: #64748b;
    padding: 10px 12px;
    border: none;
    border-bottom: 1px solid #dbe5f1;
    font-weight: bold;
}

#taskProgressBar, #cardProgressBar {
    border: none;
    border-radius: 5px;
    background-color: #e6edf5;
    text-align: center;
    color: #334155;
    min-height: 10px;
}

#taskProgressBar::chunk, #cardProgressBar::chunk {
    background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #2563eb, stop:1 #38bdf8);
    border-radius: 5px;
}

#rowActionButton {
    background-color: transparent;
    border: none;
    color: #475569;
    border-radius: 10px;
    min-width: 28px;
    padding: 4px 6px;
}

#rowActionButton:hover {
    background-color: #edf3fb;
    color: #1d4ed8;
}

#taskCard {
    background-color: #ffffff;
    border: 1px solid #dbe5f1;
    border-radius: 18px;
}

#taskCard:hover {
    border-color: #bfd5ff;
}

#cardFileName {
    color: #0f172a;
}

#cardInfoLabel {
    color: #64748b;
    font-size: 8pt;
}

#emptyStateTitle {
    color: #0f172a;
    font-size: 14pt;
    font-weight: bold;
}

#emptyStateBody {
    color: #64748b;
    font-size: 9pt;
}

QLineEdit, QTextEdit, QPlainTextEdit {
    background-color: #ffffff;
    color: #1f2937;
    border: 1px solid #d6deea;
    border-radius: 10px;
    padding: 6px;
}

QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
    border-color: #60a5fa;
}

QComboBox {
    background-color: #ffffff;
    color: #1f2937;
    border: 1px solid #d6deea;
    border-radius: 10px;
    padding: 4px 8px;
    min-width: 80px;
}

QComboBox:hover {
    border-color: #bfcee2;
}

QComboBox::drop-down {
    border: none;
    padding-right: 16px;
}

QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 4px solid #64748b;
    width: 0;
    height: 0;
}

QComboBox QAbstractItemView {
    background-color: #ffffff;
    border: 1px solid #d6deea;
    selection-background-color: #e9f2ff;
    selection-color: #1f2937;
}

QCheckBox {
    spacing: 6px;
}

QCheckBox::indicator {
    width: 18px;
    height: 18px;
    border: 2px solid #cbd5e1;
    border-radius: 4px;
    background-color: #ffffff;
}

QCheckBox::indicator:hover {
    border-color: #2563eb;
}

QCheckBox::indicator:checked {
    background-color: #2563eb;
    border-color: #2563eb;
    image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTgiIGhlaWdodD0iMTgiIHZpZXdCb3g9IjAgMCAxOCAxOCIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj48cGF0aCBkPSJNNyAxMC4ybC0xLjQtMS40LDMuNi0zLjYgNS41IDUuNS0xLjQgMS40LTQuMS00LjF6IiBmaWxsPSIjZmZmIi8+PC9zdmc+);
}

QGroupBox {
    border: 1px solid #dbe5f1;
    border-radius: 16px;
    margin-top: 12px;
    padding: 14px;
    background-color: #ffffff;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 14px;
    padding: 0 6px;
    color: #64748b;
    font-weight: bold;
}

#statusBar {
    background-color: #fcfdff;
    border-top: 1px solid #d9e2ef;
}

#statusBarButton {
    background-color: #f4f7fb;
    color: #334155;
    border: 1px solid #d6deea;
    border-radius: 10px;
    padding: 5px 12px;
    min-width: 0;
}

#statusBarButton:hover {
    background-color: #ffffff;
    border-color: #bfd5ff;
}

#statusBarLabel {
    color: #64748b;
    font-size: 8pt;
}

#detectionBadge[active="true"] {
    background-color: #ef4444;
    color: #ffffff;
    border-radius: 8px;
    font-size: 10px;
    font-weight: bold;
}

QMenu {
    background-color: #ffffff;
    border: 1px solid #d6deea;
    border-radius: 12px;
    padding: 4px;
}

QMenu::item {
    padding: 8px 24px;
    border-radius: 8px;
}

QMenu::item:selected {
    background-color: #e9f2ff;
}

QMenu::separator {
    height: 1px;
    background-color: #e2e8f0;
    margin: 4px 8px;
}
)";
}

QString ThemeManager::load_dark_theme() const
{
    return R"(
/* ========== 暗色主题 ========== */

/* 全局设置 */
QWidget {
    background-color: #1e1e1e;
    color: #e0e0e0;
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 9pt;
}

/* 主窗口 */
QMainWindow {
    background-color: #121212;
}

/* 中心部件（用于无边框透明窗口的背景） */
#centralWidget {
    background-color: #121212;
}

/* 标题栏 */
#topBar {
    background-color: #1e1e1e;
    border-bottom: 1px solid #3a3a3a;
}

#titleLabel {
    color: #e0e0e0;
    font-size: 11pt;
    font-weight: bold;
}

/* 按钮样式 */
QPushButton {
    background-color: #2d2d2d;
    color: #e0e0e0;
    border: 1px solid #404040;
    border-radius: 4px;
    padding: 4px 12px;
    min-width: 60px;
}

QPushButton:hover {
    background-color: #3a3a3a;
    border-color: #505050;
}

QPushButton:pressed {
    background-color: #454545;
}

QPushButton#primaryButton {
    background-color: #0078d4;
    color: #ffffff;
    border: none;
}

QPushButton#primaryButton:hover {
    background-color: #1a86d9;
}

QPushButton#toolButton {
    background-color: transparent;
    border: none;
    border-radius: 4px;
}

QPushButton#toolButton:hover {
    background-color: rgba(255, 255, 255, 0.08);
}

QPushButton#rowActionButton {
    background-color: transparent;
    border: none;
    border-radius: 12px;
    font-size: 14px;
}

QPushButton#rowActionButton:hover {
    background-color: rgba(255, 255, 255, 0.1);
}

QPushButton#cardActionButton {
    background-color: #2d2d2d;
    border: 1px solid #404040;
    border-radius: 4px;
    font-size: 9pt;
}

QPushButton#cardActionButton:hover {
    background-color: #3a3a3a;
}

QPushButton#cardActionButton:disabled {
    background-color: #252525;
    color: #606060;
}

/* 表格样式 */
QTableWidget {
    background-color: #1e1e1e;
    alternate-background-color: #252525;
    gridline-color: #3a3a3a;
    border: none;
}

QTableWidget::item {
    padding: 8px;
    border-bottom: 1px solid #3a3a3a;
}

QTableWidget::item:selected {
    background-color: #264f78;
    color: #e0e0e0;
}

QTableView::section:horizontal {
    background-color: #252525;
    color: #a0a0a0;
    padding: 8px;
    border: none;
    border-right: 1px solid #3a3a3a;
    border-bottom: 1px solid #3a3a3a;
    font-weight: bold;
}

QTableView::section:horizontal:first {
    border-top-left-radius: 4px;
}

QTableView::section:horizontal:last {
    border-top-right-radius: 4px;
    border-right: none;
}

/* 进度条 */
QProgressBar {
    border: 1px solid #404040;
    border-radius: 4px;
    background-color: #2d2d2d;
    text-align: center;
}

QProgressBar::chunk {
    background-color: #0078d4;
    border-radius: 3px;
}

#taskProgressBar, #cardProgressBar {
    border: none;
    background-color: #3a3a3a;
}

#taskProgressBar::chunk, #cardProgressBar::chunk {
    background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #0078d4, stop:1 #00bcf2);
}

/* 任务卡片 */
#taskCard {
    background-color: #252525;
    border: 1px solid #3a3a3a;
    border-radius: 8px;
}

#taskCard:hover {
    background-color: #2a2a2a;
    border-color: #404040;
}

#cardFileName {
    color: #e0e0e0;
}

#cardInfoLabel {
    color: #a0a0a0;
    font-size: 8pt;
}

/* 滚动区域 */
QScrollArea {
    border: none;
    background-color: transparent;
}

QScrollBar:vertical {
    background-color: #252525;
    width: 12px;
    border-radius: 6px;
}

QScrollBar::handle:vertical {
    background-color: #505050;
    border-radius: 6px;
    min-height: 30px;
}

QScrollBar::handle:vertical:hover {
    background-color: #606060;
}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}

QScrollBar:horizontal {
    background-color: #252525;
    height: 12px;
    border-radius: 6px;
}

QScrollBar::handle:horizontal {
    background-color: #505050;
    border-radius: 6px;
    min-width: 30px;
}

QScrollBar::handle:horizontal:hover {
    background-color: #606060;
}

QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0px;
}

/* 输入框 */
QLineEdit, QTextEdit, QPlainTextEdit {
    background-color: #2d2d2d;
    color: #e0e0e0;
    border: 1px solid #404040;
    border-radius: 4px;
    padding: 6px;
}

QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {
    border-color: #0078d4;
}

QLineEdit:hover, QTextEdit:hover, QPlainTextEdit:hover {
    border-color: #505050;
}

/* 下拉框 */
QComboBox {
    background-color: #2d2d2d;
    color: #e0e0e0;
    border: 1px solid #404040;
    border-radius: 4px;
    padding: 4px 8px;
    min-width: 80px;
}

QComboBox:hover {
    border-color: #505050;
}

QComboBox::drop-down {
    border: none;
    padding-right: 16px;
}

QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 4px solid #a0a0a0;
    width: 0;
    height: 0;
}

QComboBox QAbstractItemView {
    background-color: #2d2d2d;
    border: 1px solid #404040;
    selection-background-color: #264f78;
    selection-color: #e0e0e0;
}

/* 复选框 */
QCheckBox {
    spacing: 6px;
    color: #e0e0e0;
}

QCheckBox::indicator {
    width: 18px;
    height: 18px;
    border: 2px solid #505050;
    border-radius: 3px;
    background-color: #2d2d2d;
}

QCheckBox::indicator:hover {
    border-color: #0078d4;
}

QCheckBox::indicator:checked {
    background-color: #0078d4;
    border-color: #0078d4;
    image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTgiIGhlaWdodD0iMTgiIHZpZXdCb3g9IjAgMCAxOCAxOCIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj48cGF0aCBkPSJNNyAxMC4ybC0xLjQtMS40LDMuNi0zLjYgNS41IDUuNS0xLjQgMS40LTQuMS00LjF6IiBmaWxsPSIjZmZmIi8+PC9zdmc+);
}

/* 标签页 */
QTabWidget::pane {
    border: 1px solid #3a3a3a;
    background-color: #1e1e1e;
}

QTabBar::tab {
    background-color: #252525;
    color: #a0a0a0;
    padding: 8px 16px;
    border: 1px solid #3a3a3a;
    border-bottom: none;
    margin-right: 2px;
}

QTabBar::tab:selected {
    background-color: #1e1e1e;
    color: #e0e0e0;
}

QTabBar::tab:hover {
    background-color: #2d2d2d;
}

/* 分组框 */
QGroupBox {
    border: 1px solid #3a3a3a;
    border-radius: 4px;
    margin-top: 12px;
    padding: 12px;
    color: #e0e0e0;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 4px;
    color: #a0a0a0;
}

/* 侧边栏 */
#sideBar {
    background-color: #1a1a1a;
    border-right: 1px solid #3a3a3a;
}

/* 状态栏 */
#statusBar {
    background-color: #1a1a1a;
    border-top: 1px solid #3a3a3a;
}

#statusBarButton {
    background-color: #252525;
    color: #d1d5db;
    border: 1px solid #3a3a3a;
    border-radius: 10px;
    padding: 5px 12px;
    min-width: 0;
}

#statusBarButton:hover {
    background-color: #2d2d2d;
    border-color: #505050;
}

#statusBarLabel {
    color: #9ca3af;
    font-size: 8pt;
}

#detectionBadge[active="true"] {
    background-color: #dc2626;
    color: #ffffff;
    border-radius: 8px;
    font-size: 10px;
    font-weight: bold;
}

/* 菜单 */
QMenu {
    background-color: #2d2d2d;
    border: 1px solid #404040;
    border-radius: 4px;
    padding: 4px;
}

QMenu::item {
    padding: 6px 24px;
    border-radius: 4px;
    color: #e0e0e0;
}

QMenu::item:selected {
    background-color: #264f78;
}

QMenu::separator {
    height: 1px;
    background-color: #3a3a3a;
    margin: 4px 8px;
}

/* 对话框 */
QDialog {
    background-color: #1e1e1e;
}

/* 标签 */
QLabel {
    color: #e0e0e0;
}

#headerLabel {
    color: #e0e0e0;
}

/* 滑块 */
QSlider::groove:horizontal {
    height: 4px;
    background-color: #3a3a3a;
    border-radius: 2px;
}

QSlider::handle:horizontal {
    width: 16px;
    height: 16px;
    background-color: #e0e0e0;
    border: 2px solid #0078d4;
    border-radius: 8px;
    margin: -6px 0;
}

QSlider::handle:horizontal:hover {
    background-color: #0078d4;
}

/* SpinBox */
QSpinBox, QDoubleSpinBox {
    background-color: #2d2d2d;
    border: 1px solid #404040;
    border-radius: 4px;
    padding: 4px;
    color: #e0e0e0;
}

QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: #0078d4;
}

QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background-color: #252525;
    border: none;
    width: 16px;
}

QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background-color: #3a3a3a;
}
)";
}

} // namespace falcon::desktop
