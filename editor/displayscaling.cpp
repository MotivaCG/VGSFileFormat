#include "displayscaling.h"
#include <QAbstractButton>
#include <QDockWidget>
#include <QEvent>
#include <QLayout>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QVector>
#include <algorithm>

double compactDisplayScale(const QSize &logicalScreenSize) {
    const int shortEdge=std::min(logicalScreenSize.width(),logicalScreenSize.height());
    return shortEdge>0 ? std::clamp(shortEdge/1440.0,0.875,1.0) : 1.0;
}
namespace {
class CompactControls final : public QObject {
    struct WidgetSize {QPointer<QWidget> widget;QSize minimum,maximum,icon;bool fixedWidth=false,fixedHeight=false,minimumWidth=false;};
    struct LayoutSize {QPointer<QLayout> layout;QMargins margins;int spacing;};
    QWidget *window_;bool enabled_,connected_=false;double applied_=-1;
    QVector<WidgetSize> widgets_;QVector<LayoutSize> layouts_;
public:
    CompactControls(QWidget *window,bool enabled):QObject(window),window_(window),enabled_(enabled) {
        for (auto *widget:window_->findChildren<QWidget *>()) {
            const auto name=widget->objectName();
            if (name=="viewCube" || name=="viewportDisplayControls" || name=="modifierTree" || name=="transformKeyTable") continue;
            if (widget->parentWidget() && widget->parentWidget()->objectName()=="viewportDisplayControls") continue;
            WidgetSize size;size.widget=widget;size.minimum=widget->minimumSize();size.maximum=widget->maximumSize();
            size.fixedWidth=size.minimum.width()==size.maximum.width();size.fixedHeight=size.minimum.height()==size.maximum.height();
            size.minimumWidth=qobject_cast<QDockWidget *>(widget)!=nullptr;
            if (auto *button=qobject_cast<QAbstractButton *>(widget)) size.icon=button->iconSize();
            if (size.fixedWidth || size.fixedHeight || size.minimumWidth || size.icon.isValid()) widgets_.append(size);
        }
        for (auto *layout:window_->findChildren<QLayout *>()) {
            // The view cube draws and hits its own coordinates; keep its geometry.
            if (layout->parentWidget() && layout->parentWidget()->objectName()=="viewportDisplayControls") continue;
            layouts_.append({layout,layout->contentsMargins(),layout->spacing()});
        }
        window_->installEventFilter(this);
    }
    bool eventFilter(QObject *object,QEvent *event) override {
        if (object==window_ && (event->type()==QEvent::Show || event->type()==QEvent::ScreenChangeInternal)) QTimer::singleShot(0,this,[this] {apply();});
        return QObject::eventFilter(object,event);
    }
    void apply() {
        if (!connected_ && window_->windowHandle()) {connect(window_->windowHandle(),&QWindow::screenChanged,this,[this](QScreen *) {apply();});connected_=true;}
        auto *screen=window_->screen();const double scale=enabled_ && screen ? compactDisplayScale(screen->size()) : 1.0;
        if (applied_==scale) return;applied_=scale;window_->setProperty("compactControls",scale<1.0);
        window_->style()->unpolish(window_);window_->style()->polish(window_);
        for (auto *widget:window_->findChildren<QWidget *>()) {widget->style()->unpolish(widget);widget->style()->polish(widget);}
        auto pixels=[&](int value) {return qRound(value*scale);};
        for (const auto &size:widgets_) if (auto *widget=size.widget.data()) {
            if (size.fixedWidth) widget->setFixedWidth(pixels(size.minimum.width()));
            if (size.fixedHeight) widget->setFixedHeight(std::max(pixels(size.minimum.height()),widget->minimumSizeHint().height()));
            if (size.minimumWidth) widget->setMinimumWidth(pixels(size.minimum.width()));
            if (size.icon.isValid()) if (auto *button=qobject_cast<QAbstractButton *>(widget)) button->setIconSize({pixels(size.icon.width()),pixels(size.icon.height())});
        }
        for (const auto &size:layouts_) if (auto *layout=size.layout.data()) {
            layout->setContentsMargins(pixels(size.margins.left()),pixels(size.margins.top()),pixels(size.margins.right()),pixels(size.margins.bottom()));
            if (size.spacing>=0) layout->setSpacing(pixels(size.spacing));
        }
        if (window_->layout()) window_->layout()->activate();window_->update();
    }
};
}
void installCompactControls(QWidget *window,bool enabled) {new CompactControls(window,enabled);}
