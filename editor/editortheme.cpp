#include "editortheme.h"
#include <QApplication>
#include <QPalette>
#include <QComboBox>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <QEvent>
#include <QHelpEvent>
#include <QLabel>
#include <QToolTip>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>

namespace {
class ThemeInteractionStyler final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *object,QEvent *event) override {
        if (event->type()==QEvent::Polish) if (auto *combo=qobject_cast<QComboBox *>(object)) {
            auto *view=combo->view();
            // The default combo delegate paints menu items and bypasses view
            // item hover rules. A styled delegate honours the application theme.
            if (!qobject_cast<QStyledItemDelegate *>(view->itemDelegate())) view->setItemDelegate(new QStyledItemDelegate(view));
            view->setMouseTracking(true);view->viewport()->setMouseTracking(true);
        }
        if (event->type()==QEvent::ToolTip) if (auto *widget=qobject_cast<QWidget *>(object);widget && widget->isEnabled() && !widget->toolTip().isEmpty()) {
            const auto *help=static_cast<QHelpEvent *>(event);
            QToolTip::showText(help->globalPos(),widget->toolTip(),widget,widget->rect(),widget->toolTipDuration());
            // Keep long explanations compact without clipping their text.
            for (auto *window:QApplication::topLevelWidgets()) if (window->windowType()==Qt::ToolTip)
                if (auto *label=qobject_cast<QLabel *>(window)) {
                    label->setWordWrap(true);const int width=std::min(380,label->sizeHint().width());
                    label->resize(width,label->heightForWidth(width));
                    auto *screen=QGuiApplication::screenAt(help->globalPos());if (!screen) screen=widget->screen();
                    if (screen) {
                        const auto bounds=screen->availableGeometry();QPoint position=help->globalPos()+QPoint(12,20);
                        if (position.y()+label->height()>bounds.bottom()-4) position.setY(help->globalPos().y()-label->height()-8);
                        position.setX(std::clamp(position.x(),bounds.left()+4,std::max(bounds.left()+4,bounds.right()-label->width()-4)));
                        position.setY(std::clamp(position.y(),bounds.top()+4,std::max(bounds.top()+4,bounds.bottom()-label->height()-4)));label->move(position);
                    }
                }
            return true;
        }
        return QObject::eventFilter(object,event);
    }
};
}

void EditorTheme::install() {
    qApp->setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window,window());palette.setColor(QPalette::WindowText,text());
    palette.setColor(QPalette::Base,field());palette.setColor(QPalette::AlternateBase,panel());
    palette.setColor(QPalette::Text,text());palette.setColor(QPalette::Button,panel());palette.setColor(QPalette::ButtonText,text());
    palette.setColor(QPalette::ToolTipBase,panel());palette.setColor(QPalette::ToolTipText,text());
    palette.setColor(QPalette::Highlight,QColor("#2e6d4e"));palette.setColor(QPalette::HighlightedText,Qt::white);palette.setColor(QPalette::Link,accent());
    for (auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText}) palette.setColor(QPalette::Disabled,role,disabledText());
    palette.setColor(QPalette::Disabled,QPalette::Base,window());palette.setColor(QPalette::Disabled,QPalette::Button,window());qApp->setPalette(palette);
    // Retain this editor's red headings, green interaction accents and modifier
    // colours while adopting Layama's cool surfaces and recessed, flat controls.
    qApp->setStyleSheet(QStringLiteral(R"QSS(
QWidget {font-family: 'Segoe UI'; font-size: 10pt; color: %TEXT%;}
QMainWindow, QDialog {background: %WINDOW%;}
QWidget:disabled {color: %DISABLED%;}
QDockWidget::title {background: %PANEL%; color: %MUTED%; padding: 7px; border-bottom: 1px solid %PANEL_BORDER%;}
QGroupBox {background: %PANEL%; border: 1px solid %PANEL_BORDER%; border-radius: 4px; margin-top: 16px; padding: 12px 4px 4px 4px;}
QGroupBox::title {subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %MUTED%;}
QGroupBox:disabled {background: %WINDOW%;}
QLabel#assetTitle {font-size: 14pt; font-weight: 600;}
QLabel#sectionTitle {color: %ACCENT%; font-size: 12pt; font-weight: 600;}
QWidget#timeline {background: %WINDOW%; border-top: 1px solid %PANEL_BORDER%;}
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox {
    background: %FIELD%; border: 1px solid %FIELD_BORDER%; border-radius: 3px; padding: 4px 7px;
    selection-background-color: #2e6d4e; selection-color: white;
}
QSpinBox, QDoubleSpinBox {padding-right: 18px; min-height: 20px;}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {border-color: %HOVER%;}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {border-color: %ACCENT%;}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {border-color: %PANEL_BORDER%; color: %DISABLED%;}
QSpinBox::up-button, QDoubleSpinBox::up-button {subcontrol-origin: padding; subcontrol-position: top right; width: 14px; background: transparent; border: none;}
QSpinBox::down-button, QDoubleSpinBox::down-button {subcontrol-origin: padding; subcontrol-position: bottom right; width: 14px; background: transparent; border: none;}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {image: url(:/icons/spin_up.png); width: 7px; height: 7px;}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {image: url(:/icons/spin_down.png); width: 7px; height: 7px;}
QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off {image: url(:/icons/spin_up_disabled.png);}
QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off {image: url(:/icons/spin_down_disabled.png);}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {background: %PANEL%;}
QComboBox {background: %FIELD%; border: 1px solid %FIELD_BORDER%; border-radius: 3px; padding: 4px 22px 4px 8px;}
QComboBox:hover {border-color: %HOVER%;} QComboBox:focus, QComboBox:on {border-color: %ACCENT%;}
QComboBox:disabled {border-color: %PANEL_BORDER%; color: %DISABLED%;}
QComboBox::drop-down {subcontrol-origin: padding; subcontrol-position: center right; width: 18px; border: none; background: transparent;}
QComboBox::down-arrow {image: url(:/icons/combo_arrow.svg); width: 14px; height: 10px;}
QComboBox::down-arrow:disabled {image: url(:/icons/combo_arrow_disabled.svg);}
QComboBox QAbstractItemView {background: %FIELD%; border: 1px solid %FIELD_BORDER%; outline: none; selection-background-color: #333a40; selection-color: %TEXT%;}
QComboBox QAbstractItemView::item {padding: 4px 8px; border-radius: 2px;}
QComboBox QAbstractItemView::item:selected {background: %SELECTION%; color: %TEXT%;}
QComboBox QAbstractItemView::item:hover {background: %ITEM_HOVER%; color: %TEXT%;}
QPushButton, QToolButton {background: %FIELD%; border: 1px solid %FIELD_BORDER%; border-radius: 3px; color: %TEXT%;}
QPushButton {padding: 7px 10px;} QToolButton {padding: 3px;}
QPushButton:hover {background: %SECTION%; border: 1px solid %HOVER%; border-radius: 3px; padding: 7px 10px; color: %TEXT%;}
QToolButton:hover {background: %SECTION%; border: 1px solid %HOVER%; border-radius: 3px; padding: 3px; color: %TEXT%;}
QPushButton:pressed, QToolButton:pressed {background: %WINDOW%;}
QPushButton:checked, QToolButton:checked {background: #2e6d4e; border-color: #41b018;}
QToolButton[neutralToggle="true"]:checked {background: %SELECTION%; border-color: %HOVER%;}
QToolButton[neutralToggle="true"]:checked:hover {background: %ITEM_HOVER%;}
QPushButton:disabled, QToolButton:disabled {background: %WINDOW%; border-color: %PANEL_BORDER%; color: %DISABLED%;}
QToolButton[neutralToggle="true"]:disabled {background: %WINDOW%; border-color: %PANEL_BORDER%; color: %DISABLED%;}
QWidget#timelinePlaybackControls QPushButton {padding: 3px;}
QCheckBox {spacing: 5px; background: transparent;}
QMenuBar {background: %WINDOW%;} QMenuBar::item {background: transparent; padding: 3px 7px;}
QMenuBar::item:selected {background: %PANEL%;}
QMenu {background: %PANEL%; border: 1px solid %FIELD_BORDER%; padding: 3px;}
QMenu::item {padding: 5px 26px;} QMenu::item:selected {background: #333a40;}
QMenu::separator {height: 1px; background: %PANEL_BORDER%; margin: 4px 6px;}
QAbstractItemView {background: %FIELD%; alternate-background-color: %PANEL%; border: 1px solid %PANEL_BORDER%; selection-background-color: #333a40; selection-color: %TEXT%;}
QHeaderView::section {background: %SECTION%; color: %MUTED%; border: none; border-bottom: 1px solid %PANEL_BORDER%; padding: 4px 7px;}
QTableWidget {gridline-color: %PANEL_BORDER%;}
QScrollArea {border: none; background: transparent;}
QScrollBar:vertical {background: %WINDOW%; width: 10px; margin: 0;} QScrollBar:horizontal {background: %WINDOW%; height: 10px; margin: 0;}
QScrollBar::handle {background: %FIELD_BORDER%; border-radius: 3px; min-height: 24px; min-width: 24px;}
QScrollBar::handle:hover {background: %HOVER%;}
QScrollBar::add-line, QScrollBar::sub-line {height: 0; width: 0; border: none;}
QScrollBar::add-page, QScrollBar::sub-page {background: none;}
QToolTip {background: %PANEL%; color: %TEXT%; border: 1px solid %HOVER%; border-radius: 4px; padding: 6px 8px;}
QStatusBar {background: %WINDOW%; color: %MUTED%;}
/* Compact geometry on smaller screens; fonts and native DPI stay unchanged. */
QWidget[compactControls="true"] QGroupBox {margin-top: 14px; padding: 10px 4px 4px 4px;}
QWidget[compactControls="true"] QPushButton, QWidget[compactControls="true"] QPushButton:hover {padding: 6px 9px;}
QWidget[compactControls="true"] QToolButton, QWidget[compactControls="true"] QToolButton:hover {padding: 2px;}
QWidget[compactControls="true"] QLineEdit, QWidget[compactControls="true"] QSpinBox, QWidget[compactControls="true"] QDoubleSpinBox {padding: 3px 6px;}
QWidget[compactControls="true"] QSpinBox, QWidget[compactControls="true"] QDoubleSpinBox {padding-right: 16px; min-height: 18px;}
QWidget[compactControls="true"] QComboBox {padding: 3px 20px 3px 7px;}
)QSS").replace("%TEXT%",text().name()).replace("%WINDOW%",window().name()).replace("%PANEL%",panel().name())
        .replace("%PANEL_BORDER%",panelBorder().name()).replace("%FIELD%",field().name()).replace("%FIELD_BORDER%",fieldBorder().name())
        .replace("%HOVER%",hoverBorder().name()).replace("%MUTED%",mutedText().name()).replace("%DISABLED%",disabledText().name()).replace("%ACCENT%",accent().name())
        .replace("%SECTION%",sectionHeader().name()).replace("%SELECTION%",selection().name()).replace("%ITEM_HOVER%",itemHover().name()));
    qApp->installEventFilter(new ThemeInteractionStyler(qApp));
}
