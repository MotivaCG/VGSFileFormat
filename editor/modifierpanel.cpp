#include "modifierpanel.h"
#include "editortheme.h"
#include <QTreeWidget>
#include <QToolButton>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QSignalBlocker>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPersistentModelIndex>
#include <functional>
#include <QIcon>
#include <algorithm>

namespace {
constexpr int ModifierRole=Qt::UserRole+1;
// The keys drawn on a modifier's timeline: its transform keys, or an animated crop's.
QVariantList keyFrames(const Modifier &m) {
    QVariantList frames;
    if (m.type==ModifierType::Crop) {if (m.cropAnimation.animated) for (const auto &key:m.cropAnimation.keys) frames.append(key.frame);}
    else for (const auto &key:m.animation.keys) frames.append(key.frame);
    return frames;
}
QJsonArray visualState(const Project &project) {
    QJsonArray items;for (const auto &m:project.modifiers) items.append(QJsonObject{{"id",m.id},{"name",m.name},{"active",m.active()},{"type",int(m.type)},{"shape",int(m.crop.shape)},{"keys",QJsonArray::fromVariantList(keyFrames(m))}});return items;
}
QString typeName(const Modifier &modifier) {
    switch (modifier.type) {
    case ModifierType::Crop: return ModifierPanel::tr(modifier.crop.shape==CropShape::Cylinder ? "Crop cylinder" : "Crop box");
    case ModifierType::RemoveGreen: return ModifierPanel::tr("Remove green");
    case ModifierType::AnimateTransform: return ModifierPanel::tr("Animate transform");
    case ModifierType::PurgeIsolated: return ModifierPanel::tr("Purge Isolated");
    case ModifierType::Walk: return ModifierPanel::tr("Walk");
    case ModifierType::BakeAntialiasing: return ModifierPanel::tr("Bake anti-aliasing");
    }
    return {};
}
// What each kind of modifier does, for its tooltips: the type list, the add menu and the rows.
QString typeDescription(ModifierType type) {
    switch (type) {
    case ModifierType::Crop: return ModifierPanel::tr("Keeps the splats inside a cylinder or box, or removes them in Remove mode.\nSeveral Keep crops add up, and Remove wins where they overlap.\nWhen animated, it follows keys on the timeline.");
    case ModifierType::RemoveGreen: return ModifierPanel::tr("Removes splats whose base colour is green-screen spill:\nsaturated colours within a hue range around green.");
    case ModifierType::AnimateTransform: return ModifierPanel::tr("Moves, rotates and scales the whole capture over time with keys.\nVGS/PGS exports store the movement as motion samples. MINT cannot hold it.");
    case ModifierType::PurgeIsolated: return ModifierPanel::tr("Removes stray splats: those whose Nth nearest neighbour\nis farther than a multiple of the frame's median distance.");
    case ModifierType::Walk: return ModifierPanel::tr("Marks the capture as walking at a speed along +Z.\nThe preview slides the floor under it.\nExports write the speed to the header and leave the data in place.");
    case ModifierType::BakeAntialiasing: return ModifierPanel::tr("Prepares a capture trained with anti-aliasing (every Gracia .mint)\nfor renderers that do not compensate for it.\nThin splats grow to about a pixel at a chosen viewing distance and fade by as much,\nso they no longer draw as solid lines.");
    }
    return {};
}
QColor modifierColour(ModifierType type) {
    switch (type) {
    case ModifierType::Crop: return {240,60,90};
    case ModifierType::RemoveGreen: return {85,185,105};
    case ModifierType::AnimateTransform: return {67,147,214};
    case ModifierType::PurgeIsolated: return {219,181,76};
    case ModifierType::Walk: return {160,110,214};
    case ModifierType::BakeAntialiasing: return {185,133,114}; // #B98572
    }
    return {75,80,86};
}
QIcon whiteIcon(const QIcon &source,const QColor &colour=Qt::white) {
    QIcon icon;for (auto mode:{QIcon::Normal,QIcon::Active,QIcon::Selected,QIcon::Disabled}) {
        auto pixmap=source.pixmap(24,24);QPainter painter(&pixmap);painter.setCompositionMode(QPainter::CompositionMode_SourceIn);painter.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#606060") : colour);painter.end();icon.addPixmap(pixmap,mode);
    }return icon;
}
class CoverageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(int)> seek;
    std::function<void()> textMoved;
    double viewFirst=0,viewLast=0; // the frames the bars show
    double frameX(const QRect &bar,double frame) const {return bar.left()+bar.width()*(frame-viewFirst)/std::max(1e-9,viewLast-viewFirst);}
    bool inView(double frame) const {return frame>=viewFirst-1e-6 && frame<=viewLast+1e-6;}
    mutable int textOffset=-1; // unknown until a row has been painted
    bool editorEvent(QEvent *event,QAbstractItemModel *model,const QStyleOptionViewItem &option,const QModelIndex &index) override {
        if (index.column()==0) {
            if (!(option.state & QStyle::State_Enabled) || !(index.flags() & Qt::ItemIsUserCheckable)) return false;
            auto toggle=[&] {return model->setData(index,index.data(Qt::CheckStateRole).toInt()==Qt::Checked ? Qt::Unchecked : Qt::Checked,Qt::CheckStateRole);};
            if (event->type()==QEvent::KeyPress) {
                const auto *key=static_cast<QKeyEvent *>(event);
                if (key->key()==Qt::Key_Space || key->key()==Qt::Key_Select) return toggle();
            }
            if (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseButtonRelease || event->type()==QEvent::MouseButtonDblClick) {
                const auto *mouse=static_cast<QMouseEvent *>(event);
                if (mouse->button()!=Qt::LeftButton) return false;
                QStyleOptionViewItem copy(option);initStyleOption(&copy,index);
                const auto *style=option.widget ? option.widget->style() : QApplication::style();
                const auto iconRect=style->subElementRect(QStyle::SE_ItemViewItemDecoration,&copy,option.widget).adjusted(-3,-3,3,3);
                if (event->type()==QEvent::MouseButtonPress) {pressedEye_=iconRect.contains(mouse->position().toPoint()) ? QPersistentModelIndex(index) : QPersistentModelIndex();return pressedEye_.isValid();}
                if (event->type()==QEvent::MouseButtonDblClick) return iconRect.contains(mouse->position().toPoint());
                const bool clicked=pressedEye_==index && iconRect.contains(mouse->position().toPoint());pressedEye_=QPersistentModelIndex();
                if (clicked) return toggle();
            }
            return false;
        }
        if (index.column()==2 && event->type()==QEvent::MouseButtonRelease && (option.state & QStyle::State_Enabled)) {
            const auto *mouse=static_cast<QMouseEvent *>(event);if (mouse->button()==Qt::LeftButton) {
                const auto keys=index.data(Qt::UserRole+4).toList();const auto rect=barRect(option.rect);
                for (const auto &key:keys) if (inView(key.toInt()) && std::abs(mouse->position().x()-frameX(rect,key.toInt()))<7) {if (seek) seek(key.toInt());return true;}
            }
        }
        return QStyledItemDelegate::editorEvent(event,model,option,index);
    }
    // Where a name's text starts in a row, as paint() lays it out with the eye beside it.
    QRect textRect(QStyleOptionViewItem option,const QModelIndex &index) const {
        initStyleOption(&option,index);const auto *style=option.widget ? option.widget->style() : QApplication::style();
        // Qt draws the text inside that rect, past a focus-frame margin of its own.
        const int margin=style->pixelMetric(QStyle::PM_FocusFrameHMargin,&option,option.widget)+1;
        return style->subElementRect(QStyle::SE_ItemViewItemText,&option,option.widget).adjusted(margin,0,-margin,0);
    }
    // Thin, centred range bar; keyframe hits use the same horizontal extent.
    static QRect barRect(const QRect &cell) {const int height=13;return {cell.left()+8,cell.center().y()-height/2,cell.width()-16,height};}
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(std::max(30,size.height()));return size;
    }
    QWidget *createEditor(QWidget *parent,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        return index.column()==0 ? QStyledItemDelegate::createEditor(parent,option,index) : nullptr;
    }
    void paint(QPainter *painter,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        if (index.column()==0) {
            // Where a name's text really starts within its row, as painted: the timeline's
            // name lines up with it.
            const int offset=textRect(option,index).left()-option.rect.left();
            if (offset!=textOffset) {textOffset=offset;if (textMoved) textMoved();}
        }
        if (index.column()!=2) {QStyledItemDelegate::paint(painter,option,index);return;}
        QStyleOptionViewItem copy(option);initStyleOption(&copy,index);copy.text.clear();
        const QWidget *widget=option.widget;(widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem,&copy,painter,widget);
        const bool active=index.data(Qt::UserRole+2).toBool() && (option.state & QStyle::State_Enabled);const auto type=ModifierType(index.data(Qt::UserRole+3).toInt());
        painter->save();const auto rect=barRect(option.rect);painter->setPen(Qt::NoPen);
        painter->setBrush(!active ? QColor(75,80,86) : modifierColour(type));painter->drawRoundedRect(rect,3,3);
        const int current=index.data(Qt::UserRole+6).toInt();
        painter->setRenderHint(QPainter::Antialiasing,true);
        // The current time, read only: the timeline's playhead carried down through every bar.
        if (inView(current)) {const double x=frameX(rect,current);
            painter->setPen(QPen(QColor(0,0,0,90),3));painter->drawLine(QPointF(x,rect.top()-3),QPointF(x,rect.bottom()+4));
            painter->setPen(QPen(Qt::white,2));painter->drawLine(QPointF(x,rect.top()-3),QPointF(x,rect.bottom()+4));}
        for (const auto &key:index.data(Qt::UserRole+4).toList()) {
            const int frame=key.toInt();if (!inView(frame)) continue;const double x=frameX(rect,frame),y=rect.center().y();
            painter->setPen(QPen(active ? QColor("#325777") : QColor("#555b61"),1));painter->setBrush(active ? frame==current ? Qt::white : QColor("#dcebf7") : QColor("#888888"));painter->drawPolygon(QPolygonF{{x,y-5},{x+4,y},{x,y+5},{x-4,y}});
        }
        painter->restore();
    }
protected:
    void initStyleOption(QStyleOptionViewItem *option,const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option,index);
        if (index.column()!=0) return;
        option->features &= ~QStyleOptionViewItem::HasCheckIndicator;
        option->features |= QStyleOptionViewItem::HasDecoration;
        option->icon=index.data(Qt::CheckStateRole).toInt()==Qt::Checked ? eye_ : eyeOff_;
        option->decorationSize={20,20};option->decorationAlignment=Qt::AlignCenter;
    }
private:
    QIcon eye_=whiteIcon(QIcon(":/icons/eye.png")),eyeOff_=whiteIcon(QIcon(":/icons/eye_off.png"),QColor("#888888"));
    QPersistentModelIndex pressedEye_;
};
}
ModifierPanel::ModifierPanel(QWidget *parent):QWidget(parent) {
    setObjectName("modifierPanel");auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,6,0,0);layout->setSpacing(5);
    auto *toolbar=new QHBoxLayout;auto *title=new QLabel(tr("MODIFIERS"));title->setObjectName("sectionTitle");toolbar->addWidget(title);toolbar->addStretch();
    auto button=[&](const QString &text,const QString &id,const QString &tip,auto action) {
        auto *b=new QToolButton;b->setText(text);b->setObjectName(id);b->setToolTip(tip);toolbar->addWidget(b);connect(b,&QToolButton::clicked,this,action);return b;
    };
    type_=new QComboBox;type_->setObjectName("newModifierType");type_->addItem(tr("Crop"),0);type_->addItem(tr("Remove green points"),2);
    type_->addItem(tr("Animate transform"),3);type_->addItem(tr("Purge Isolated"),4);type_->addItem(tr("Walk"),5);type_->addItem(tr("Bake anti-aliasing"),6);toolbar->addWidget(type_);
    {   // Each kind says what it does, in the list and in the context menu's Add modifier.
        const ModifierType kinds[]={ModifierType::Crop,ModifierType::RemoveGreen,ModifierType::AnimateTransform,ModifierType::PurgeIsolated,ModifierType::Walk,ModifierType::BakeAntialiasing};
        for (int i=0;i<type_->count();++i) type_->setItemData(i,typeDescription(kinds[i]),Qt::ToolTipRole);
    }
    type_->setToolTip(tr("Choose a modifier to add.\nHover a kind to see what it does."));
    addModifier_=button(tr("+ Modifier"),"addModifier",tr("Add the selected modifier type to the stack. Modifiers affect the full capture timeline."),[this] {addModifier();});
    duplicate_=button(tr("Duplicate"),"duplicateModifier",tr("Duplicate the selected modifier."),[this] {duplicateSelection();});
    up_=button(tr("Up"),"moveModifierUp",tr("Move the selected modifier up."),[this] {moveSelection(-1);});
    down_=button(tr("Down"),"moveModifierDown",tr("Move the selected modifier down."),[this] {moveSelection(1);});
    remove_=button(tr("Remove"),"removeModifier",tr("Remove the selected modifier. Source capture data is preserved."),[this] {removeSelection();});
    auto iconButton=[&](QToolButton *button,const QIcon &icon,const QString &name) {button->setText({});button->setIcon(whiteIcon(icon));button->setIconSize({20,20});button->setToolButtonStyle(Qt::ToolButtonIconOnly);button->setFixedSize(30,30);button->setAccessibleName(name);};
    iconButton(addModifier_,QIcon::fromTheme(QIcon::ThemeIcon::ListAdd,style()->standardIcon(QStyle::SP_FileDialogNewFolder)),tr("Add modifier"));
    iconButton(duplicate_,QIcon::fromTheme(QIcon::ThemeIcon::EditCopy,style()->standardIcon(QStyle::SP_FileIcon)),tr("Duplicate modifier"));
    iconButton(up_,QIcon::fromTheme(QIcon::ThemeIcon::GoUp,style()->standardIcon(QStyle::SP_ArrowUp)),tr("Move modifier up"));iconButton(down_,QIcon::fromTheme(QIcon::ThemeIcon::GoDown,style()->standardIcon(QStyle::SP_ArrowDown)),tr("Move modifier down"));
    iconButton(remove_,QIcon::fromTheme(QIcon::ThemeIcon::EditDelete,style()->standardIcon(QStyle::SP_TrashIcon)),tr("Remove modifier"));
    layout->addLayout(toolbar);
    tree_=new QTreeWidget;tree_->setObjectName("modifierTree");tree_->setColumnCount(3);tree_->setHeaderLabels({tr("Modifier"),tr("Type"),QString()});
    tree_->setColumnHidden(1,true);
    tree_->setUniformRowHeights(true);tree_->setRootIsDecorated(false);tree_->setSelectionMode(QAbstractItemView::SingleSelection);tree_->setMinimumHeight(90);tree_->setMaximumHeight(190);
    tree_->setAlternatingRowColors(true);
    auto palette=tree_->palette();palette.setColor(QPalette::Base,EditorTheme::field());palette.setColor(QPalette::AlternateBase,EditorTheme::panel());
    palette.setColor(QPalette::Highlight,QColor("#333a40"));palette.setColor(QPalette::HighlightedText,EditorTheme::text());tree_->setPalette(palette);
    tree_->setStyleSheet("QTreeWidget {border-radius: 4px;} QTreeWidget::item {padding: 2px 5px; border-bottom: 1px solid #23262a;}"
        "QTreeWidget::item:selected {background: #333a40; color: #e7eaeb;} QTreeWidget::item:hover:!selected {background: #1b1e21;}");
    tree_->header()->setSectionResizeMode(0,QHeaderView::Fixed);tree_->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);tree_->header()->setSectionResizeMode(2,QHeaderView::Stretch);
    tree_->viewport()->installEventFilter(this);
    connect(tree_->header(),&QHeaderView::sectionResized,this,&ModifierPanel::trackMoved);
    connect(tree_->header(),&QHeaderView::geometriesChanged,this,&ModifierPanel::trackMoved);
    auto *delegate=new CoverageDelegate(tree_);delegate->seek=[this](int frame) {emit seekFrame(frame);};
    // Measured while painting: refit and realign afterwards, not in the middle of a paint.
    delegate->textMoved=[this] {QMetaObject::invokeMethod(this,[this] {fitNames();emit trackMoved();},Qt::QueuedConnection);};tree_->setItemDelegate(delegate);tree_->setToolTip(tr("Click the eye to enable or disable a modifier.\nSelect a modifier to edit its properties in Tools.\nClick a keyframe diamond to seek.\nCtrl+wheel zooms the timeline, Shift+wheel or a middle drag pans it."));layout->addWidget(tree_);
    connect(tree_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem *item) {
        if (updating_ || !item) return;project_.selectedModifier=item->data(0,ModifierRole).toString();emit selectionChanged();
    });
    connect(tree_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem *item,int column) {
        if (updating_ || column!=0) return;
        for (auto &m:project_.modifiers) if (m.id==item->data(0,ModifierRole).toString()) {
            m.name=item->text(0).trimmed().left(120);if (m.name.isEmpty()) m.name="Modifier";
            m.enabled=item->checkState(0)==Qt::Checked;if (m.type==ModifierType::Crop) m.crop.enabled=m.enabled;
        }
        emit stackChanged();
    });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_,&QTreeWidget::customContextMenuRequested,this,[this](const QPoint &position) {
        auto *item=tree_->itemAt(position);if (item) {tree_->setCurrentItem(item);item=tree_->currentItem();}
        QMenu menu(this);auto *add=menu.addMenu(tr("Add modifier"));add->setToolTipsVisible(true);
        for (int type=0;type<type_->count();++type) add->addAction(type_->itemText(type),this,[this,type] {type_->setCurrentIndex(type);addModifier();})->setToolTip(type_->itemData(type,Qt::ToolTipRole).toString());
        if (item) {
            menu.addSeparator();const bool enabled=item->checkState(0)==Qt::Checked;
            menu.addAction(enabled ? tr("Disable temporarily") : tr("Enable"),this,[this,enabled] {if (auto *current=tree_->currentItem()) current->setCheckState(0,enabled ? Qt::Unchecked : Qt::Checked);});
            menu.addAction(tr("Rename"),this,[this] {if (auto *current=tree_->currentItem()) tree_->editItem(current,0);});
            menu.addAction(tr("Duplicate"),this,[this] {duplicateSelection();});
            menu.addAction(tr("Move up"),this,[this] {moveSelection(-1);});menu.addAction(tr("Move down"),this,[this] {moveSelection(1);});
            menu.addSeparator();menu.addAction(tr("Remove"),this,[this] {removeSelection();});
        }
        menu.exec(tree_->viewport()->mapToGlobal(position));
    });
    rebuild();
}
void ModifierPanel::setProject(const Project &project) {
    const bool changed=renderedModifiers_!=visualState(project);const bool selected=project_.selectedModifier!=project.selectedModifier;
    project_=project;
    if (changed) {
        bool sameRows=tree_->topLevelItemCount()==project_.modifiers.size();
        if (sameRows) for (int i=0;i<tree_->topLevelItemCount();++i) sameRows &= tree_->topLevelItem(i)->data(0,ModifierRole).toString()==project_.modifiers[i].id;
        if (!sameRows) rebuild();
        else {
            // itemChanged can be emitted from inside QTreeWidgetItem::setData.
            // Keep these items alive while updating activation, name or appearance.
            updating_=true;QSignalBlocker blocker(tree_);
            for (int i=0;i<tree_->topLevelItemCount();++i) {
                auto *item=tree_->topLevelItem(i);const auto &m=project_.modifiers[i];
                item->setText(0,m.name);item->setText(1,typeName(m));item->setToolTip(0,typeName(m)+"\n"+typeDescription(m.type));item->setToolTip(2,item->toolTip(0));
                item->setCheckState(0,m.active() ? Qt::Checked : Qt::Unchecked);item->setData(2,Qt::UserRole+2,m.active());item->setData(2,Qt::UserRole+3,int(m.type));
                item->setData(2,Qt::UserRole+4,keyFrames(m));
            }
            renderedModifiers_=visualState(project_);updating_=false;fitNames();tree_->viewport()->update();
        }
    }
    if (selected) {
        updating_=true;for (int i=0;i<tree_->topLevelItemCount();++i) if (tree_->topLevelItem(i)->data(0,ModifierRole).toString()==project_.selectedModifier) tree_->setCurrentItem(tree_->topLevelItem(i));updating_=false;
    }
}
void ModifierPanel::rebuild() {
    updating_=true;tree_->clear();QTreeWidgetItem *selected=nullptr;
    for (const auto &m:project_.modifiers) {
        const QString type=typeName(m);
        auto *item=new QTreeWidgetItem(tree_,{m.name,type,QString()});item->setFlags(item->flags()|Qt::ItemIsEditable|Qt::ItemIsUserCheckable);item->setCheckState(0,m.active() ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(0,type+"\n"+typeDescription(m.type));
        item->setData(2,Qt::UserRole+4,keyFrames(m));item->setData(2,Qt::UserRole+5,maximum_);item->setData(2,Qt::UserRole+6,frame_);
        item->setData(0,ModifierRole,m.id);item->setData(2,Qt::UserRole+2,m.active());item->setData(2,Qt::UserRole+3,int(m.type));item->setToolTip(2,item->toolTip(0));
        if (project_.selectedModifier==m.id) selected=item;
    }
    if (selected) tree_->setCurrentItem(selected);addModifier_->setEnabled(true);duplicate_->setEnabled(selected);remove_->setEnabled(selected);up_->setEnabled(selected);down_->setEnabled(selected);
    tree_->setFixedHeight(std::min(250,30*std::max(4,int(project_.modifiers.size()))+tree_->header()->height()+4));
    renderedModifiers_=visualState(project_);updating_=false;fitNames();
}
// As wide as the longest name, and never narrower than the minimum asked for.
void ModifierPanel::fitNames() {
    const int offset=textOffset();
    const int width=std::max(static_cast<QAbstractItemView *>(tree_)->sizeHintForColumn(0),offset+minimumNameWidth_+16);
    if (tree_->columnWidth(0)!=width) tree_->setColumnWidth(0,width);
}
void ModifierPanel::setMinimumNameWidth(int pixels) {if (minimumNameWidth_!=pixels) {minimumNameWidth_=pixels;fitNames();}}
QPair<int,int> ModifierPanel::trackSpan() const {
    // Column 2's bar, as the delegate draws it, in the tree viewport's coordinates.
    const auto *header=tree_->header();const int x=header->sectionViewportPosition(2);
    const QRect bar=CoverageDelegate::barRect(QRect(x,0,header->sectionSize(2),30));
    return {tree_->viewport()->mapToGlobal(QPoint(bar.left(),0)).x(),bar.width()};
}
int ModifierPanel::textOffset() const {
    const int painted=static_cast<CoverageDelegate *>(tree_->itemDelegate())->textOffset;
    return painted>=0 ? painted : 30; // before any row is painted: the eye and its spacing
}
int ModifierPanel::nameLeft() const {
    return tree_->viewport()->mapToGlobal(QPoint(tree_->header()->sectionViewportPosition(0)+textOffset(),0)).x();
}
QPair<double,double> ModifierPanel::view() const {const auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());return {delegate->viewFirst,delegate->viewLast};}
void ModifierPanel::setView(double first,double last) {
    auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());
    if (delegate->viewFirst==first && delegate->viewLast==last) return;
    delegate->viewFirst=first;delegate->viewLast=last;tree_->viewport()->update();
}
// Over the bars, Ctrl+wheel zooms the timeline and Shift+wheel, a horizontal wheel or a
// middle drag pans it; the plain wheel still scrolls the list.
bool ModifierPanel::eventFilter(QObject *watched,QEvent *event) {
    if (watched!=tree_->viewport()) return QWidget::eventFilter(watched,event);
    switch (event->type()) {
    case QEvent::Wheel: {
        auto *wheel=static_cast<QWheelEvent *>(event);const auto delta=wheel->angleDelta();
        if (delta.x()!=0 || (wheel->modifiers() & Qt::ShiftModifier)) {emit panRequested((delta.x()!=0 ? delta.x() : delta.y())/120.0*CoverageDelegate::barRect(QRect(0,0,tree_->header()->sectionSize(2),30)).width()*0.1);return true;}
        if (wheel->modifiers() & Qt::ControlModifier) {emit zoomRequested(std::pow(1.25,delta.y()/120.0),qRound(wheel->globalPosition().x()));return true;}
        break;
    }
    case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: {
        auto *mouse=static_cast<QMouseEvent *>(event);if (mouse->button()!=Qt::MiddleButton) break;
        if (event->type()==QEvent::MouseButtonDblClick) {emit resetViewRequested();return true;}
        panning_=true;panX_=mouse->position().x();tree_->viewport()->setCursor(Qt::ClosedHandCursor);return true;
    }
    case QEvent::MouseMove:
        if (panning_) {auto *mouse=static_cast<QMouseEvent *>(event);emit panRequested(mouse->position().x()-panX_);panX_=mouse->position().x();return true;}
        break;
    case QEvent::MouseButtonRelease:
        if (panning_ && static_cast<QMouseEvent *>(event)->button()==Qt::MiddleButton) {panning_=false;tree_->viewport()->unsetCursor();return true;}
        break;
    default: break;
    }
    return QWidget::eventFilter(watched,event);
}
void ModifierPanel::resizeEvent(QResizeEvent *event) {QWidget::resizeEvent(event);emit trackMoved();}
void ModifierPanel::setTimeline(int frame,int maximum) {
    if (frame_==frame && maximum_==maximum) return;frame_=frame;maximum_=maximum;QSignalBlocker blocker(tree_);
    for (int row=0;row<tree_->topLevelItemCount();++row) {auto *item=tree_->topLevelItem(row);item->setData(2,Qt::UserRole+5,maximum);item->setData(2,Qt::UserRole+6,frame);}
    tree_->viewport()->update();
}
void ModifierPanel::addModifier() {
    Modifier m;m.id=Project::newId();const int type=type_->currentData().toInt();
    m.type=type==2 ? ModifierType::RemoveGreen : type==3 ? ModifierType::AnimateTransform : type==4 ? ModifierType::PurgeIsolated : type==5 ? ModifierType::Walk
          : type==6 ? ModifierType::BakeAntialiasing : ModifierType::Crop;
    m.crop.enabled=true;m.crop.shape=CropShape::Cylinder; // Shape is chosen afterwards in the crop parameters.
    // The first of a kind keeps the bare name ("Crop"); later ones take the next free number ("Crop 2", "Crop 3"...).
    const QString base=m.type==ModifierType::Crop ? tr("Crop") : typeName(m);
    auto taken=[this](const QString &name) {return std::any_of(project_.modifiers.cbegin(),project_.modifiers.cend(),[&](const Modifier &other) {return other.name==name;});};
    m.name=base;for (int number=2;taken(m.name);++number) m.name=QString("%1 %2").arg(base).arg(number);
    project_.modifiers.append(m);project_.selectedModifier=m.id;emit stackChanged();if (m.type==ModifierType::Crop) emit cropAdded();
}
void ModifierPanel::removeSelection() {
    for (int i=0;i<project_.modifiers.size();++i) if (project_.modifiers[i].id==project_.selectedModifier) {
        project_.modifiers.removeAt(i);project_.selectedModifier=project_.modifiers.isEmpty() ? QString() : project_.modifiers[std::min(i,int(project_.modifiers.size()-1))].id;emit stackChanged();return;
    }
}
void ModifierPanel::duplicateSelection() {
    for (int i=0;i<project_.modifiers.size();++i) if (project_.modifiers[i].id==project_.selectedModifier) {
        auto m=project_.modifiers[i];m.id=Project::newId();m.name+=tr(" copy");project_.modifiers.insert(i+1,m);project_.selectedModifier=m.id;emit stackChanged();return;
    }
}
void ModifierPanel::moveSelection(int direction) {
    for (int i=0;i<project_.modifiers.size();++i) if (project_.modifiers[i].id==project_.selectedModifier) {
        const int next=i+direction;if (next>=0 && next<project_.modifiers.size()) project_.modifiers.swapItemsAt(i,next);emit stackChanged();return;
    }
}
