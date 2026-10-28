// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "modifierpanel.h"
#include "captureworker.h"
#include <QShortcut>
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
#include <QPainterPath>
#include <QToolBar>
#include <QSignalBlocker>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPersistentModelIndex>
#include <QToolTip>
#include <QHelpEvent>
#include <functional>
#include <QIcon>
#include <QDateTime>
#include <algorithm>

namespace {
constexpr int ModifierRole=Qt::UserRole+1;
constexpr int CacheRole=Qt::UserRole+20;
constexpr int CacheProgressRole=Qt::UserRole+21;
// A bar's part of the timeline: start, end, fade in, fade out (frames; -1 an open end), and
// whether it has a range at all and whether it fades.
constexpr int RangeRole=Qt::UserRole+8;
// A track's clips, one map each: "range" (as RangeRole), "active", "type", "name", "id" and
// whether it is the selected modifier.
constexpr int ClipsRole=Qt::UserRole+9;
QVariantList rangeOf(const Modifier &m) {return {m.start,m.end,m.fadeIn,m.fadeOut,Modifier::timed(m.type),Modifier::fades(m.type)};}
// Its ends, as they show: an open one at the timeline's start or end.
QPair<int,int> rangeFrames(const QVariantList &range,int maximum) {
    const int start=std::max(0,range.value(0).toInt()),end=range.value(1).toInt()<0 ? maximum : std::max(start,range.value(1).toInt());return {start,end};
}
// Kept sensible after an edit: ends at the timeline's start or end open again, and the fades
// within the range.
void tidyRange(Modifier &m,int maximum) {
    if (!Modifier::fades(m.type)) m.fadeIn=m.fadeOut=0;
    if (m.start<=0) m.start=-1;
    if (m.end>=maximum && m.fadeOut==0) m.end=-1;
    const int start=std::max(0,m.start),end=m.end<0 ? maximum : m.end,length=std::max(0,end-start+1);
    m.fadeIn=std::clamp(m.fadeIn,0,length);m.fadeOut=std::clamp(m.fadeOut,0,std::max(0,length-m.fadeIn));
    if (m.end<0) m.fadeOut=0;
}
// 0: disabled; 1: empty; 2: complete; 3: working; 4: partial;
// 5: direct processing; 6: included in another layer's cache.
int previewDespill(const Project &project) {
    int index=-1;
    for (int i=0;i<project.modifiers.size();++i) {const auto &m=project.modifiers[i];if (m.active() && m.type==ModifierType::Colour && m.colourDespill!=Modifier::DespillNone) index=i;}
    return index;
}
// The keys drawn on a modifier's timeline: its transform keys, or an animated crop's.
QVariantList keyFrames(const Modifier &m) {
    QVariantList frames;
    if (m.type==ModifierType::Crop) {if (m.cropAnimation.animated) for (const auto &key:m.cropAnimation.keys) frames.append(key.frame);}
    else for (const auto &key:m.animation.keys) frames.append(key.frame);
    return frames;
}
// An Erase modifier's hand picks, per source chunk: [chunk, picked records] for each chunk
// that has some. They hold for every frame of their chunk, so the bar shows chunks, not keys.
QVariantList erasedChunks(const Modifier &m) {
    QVariantList chunks;
    if (m.type==ModifierType::Erase) for (const auto &[chunk,records]:m.erased) if (!records.empty()) chunks.append(QVariant(QVariantList{chunk,qulonglong(records.size())}));
    return chunks;
}
QJsonArray visualState(const Project &project) {
    QJsonArray items;for (const auto &m:project.modifiers) items.append(QJsonObject{{"id",m.id},{"name",m.name},{"active",m.active()},{"type",int(m.type)},{"shape",int(m.crop.shape)},{"keys",QJsonArray::fromVariantList(keyFrames(m))},{"erased",QJsonArray::fromVariantList(erasedChunks(m))},{"range",QJsonArray::fromVariantList(rangeOf(m))}});return items;
}
QString typeName(const Modifier &modifier) {
    switch (modifier.type) {
    case ModifierType::Crop: return ModifierPanel::tr(modifier.crop.shape==CropShape::Cylinder ? "Crop cylinder" : "Crop box");
    case ModifierType::RemoveGreen: return ModifierPanel::tr("Remove green");
    case ModifierType::AnimateTransform: return ModifierPanel::tr("Animate transform");
    case ModifierType::PurgeIsolated: return ModifierPanel::tr("Purge Isolated");
    case ModifierType::Walk: return ModifierPanel::tr("Walk");
    case ModifierType::BakeAntialiasing: return ModifierPanel::tr("Bake anti-aliasing");
    case ModifierType::PruneLowContribution: return ModifierPanel::tr("Prune low contribution");
    case ModifierType::Audio: return ModifierPanel::tr("Audio");
    case ModifierType::Colour: return ModifierPanel::tr("Color");
    case ModifierType::Erase: return ModifierPanel::tr("Eraser");
    case ModifierType::LoopMorph: return ModifierPanel::tr("Loop morph");
    }
    return {};
}
// What each kind of modifier does, for its tooltips: the type list, the add menu and the rows.
QString typeDescription(ModifierType type) {
    switch (type) {
    case ModifierType::Crop: return ModifierPanel::tr("Keeps the splats inside a cylinder or box, or removes them in Remove mode.\nSeveral Keep crops add up, and Remove wins where they overlap.\nWhen animated, it follows keys on the timeline.");
    case ModifierType::RemoveGreen: return ModifierPanel::tr("Removes splats whose base color is green-screen spill:\nsaturated colors within a hue range around green.");
    case ModifierType::AnimateTransform: return ModifierPanel::tr("Moves, rotates and scales the whole capture over time with keys.\nVGS/PGS exports store the movement as motion samples. MINT cannot hold it.");
    case ModifierType::PurgeIsolated: return ModifierPanel::tr("Removes stray splats: those whose Nth nearest neighbor\nis farther than a multiple of the frame's median distance.");
    case ModifierType::Walk: return ModifierPanel::tr("Marks the capture as walking at a speed along +Z.\nThe preview slides the floor under it.\nExports write the speed to the header and leave the data in place.");
    case ModifierType::BakeAntialiasing: return ModifierPanel::tr("Prepares a capture trained with anti-aliasing (every Gracia .mint)\nfor renderers that do not compensate for it.\nThin splats grow to about a pixel at a chosen viewing distance and fade by as much,\nso they no longer draw as solid lines.");
    case ModifierType::PruneLowContribution: return ModifierPanel::tr("Removes the splats that add least to the image:\nthe hidden, the faint and the tiny, measured by rendering each chunk\nfrom around the capture. Up to a share of them, and never one\nthat covers more than a set area.");
    case ModifierType::Audio: return ModifierPanel::tr("The soundtrack: the capture's own or a file, with an offset.\nThe editor plays it with the timeline; exports carry it in sync as AAC (VGS/PGS) or MP3 (MINT).");
    case ModifierType::Colour: return ModifierPanel::tr("Exposure, white balance and saturation for the whole capture,\nview-dependent color included, and green spill removal.\nShown in the viewport; exports keep the capture's own color tables.");
    case ModifierType::Erase: return ModifierPanel::tr("Removes splats picked by hand with a brush or a lasso:\nfloaters, stray pieces, whatever the filters miss.\nA splat is itself only within one chunk, so each chunk keeps its own picks.");
    case ModifierType::LoopMorph: return ModifierPanel::tr("Adds frames after End that morph the End frame back into the Start frame,\nso a loop plays without a jump. Numbered markers on both frames guide it.\nThe cleaning modifiers above it shape those two frames; the ones below apply to its frames.");
    }
    return {};
}
// One family of hues per group, lively without glaring: reds for cleaning (Crop the deepest),
// grey for the look, sky to violet for motion, amber for sound, green to teal for delivery.
QColor modifierColour(ModifierType type) {
    switch (type) {
    case ModifierType::Crop: return QColor(QRgb(0xEF3450u)); // the editor's own red, as its headings
    case ModifierType::RemoveGreen: return QColor(QRgb(0xDB3C6Cu));
    case ModifierType::PurgeIsolated: return QColor(QRgb(0xEF4A6Cu));
    case ModifierType::Erase: return QColor(QRgb(0xF7637Bu));
    case ModifierType::Colour: return QColor(QRgb(0x8A95A3u));
    case ModifierType::AnimateTransform: return QColor(QRgb(0x1DA8E8u));
    case ModifierType::Walk: return QColor(QRgb(0x2F7CF6u));
    case ModifierType::LoopMorph: return QColor(QRgb(0x8657F5u));
    case ModifierType::Audio: return QColor(QRgb(0xEAB308u));
    case ModifierType::PruneLowContribution: return QColor(QRgb(0x22C55Eu));
    case ModifierType::BakeAntialiasing: return QColor(QRgb(0x0EA58Fu));
    }
    return {75,80,86};
}
// The list's separators as the menus draw theirs, a thin line: the theme's item style would
// otherwise paint them as rows like the others, and the groups would not show.
class KindListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static bool separator(const QModelIndex &index) {return index.data(Qt::AccessibleDescriptionRole).toString()==QLatin1String("separator");}
    void paint(QPainter *painter,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        if (!separator(index)) {QStyledItemDelegate::paint(painter,option,index);return;}
        painter->save();painter->setPen(QPen(QColor(70,73,77),1));const int y=option.rect.center().y();
        painter->drawLine(option.rect.left()+6,y,option.rect.right()-6,y);painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        return separator(index) ? QSize(option.rect.width(),9) : QStyledItemDelegate::sizeHint(option,index);
    }
};
// A kind's colour as a small rounded square: what its bar will look like, in front of its name.
QIcon swatch(const QColor &colour) {
    QPixmap pixmap(32,32);pixmap.setDevicePixelRatio(2);pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Qt::NoPen);painter.setBrush(colour);
    painter.drawRoundedRect(QRectF(3.5,3.5,9,9),2.5,2.5);painter.end();
    return QIcon(pixmap);
}
QIcon whiteIcon(const QIcon &source,const QColor &colour=Qt::white) {
    QIcon icon;for (auto mode:{QIcon::Normal,QIcon::Active,QIcon::Selected,QIcon::Disabled}) {
        auto pixmap=source.pixmap(24,24);QPainter painter(&pixmap);painter.setCompositionMode(QPainter::CompositionMode_SourceIn);painter.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#606060") : colour);painter.end();icon.addPixmap(pixmap,mode);
    }return icon;
}
// The kinds in the order a capture is usually worked on - clean it, its look, its motion, its
// sound, then what prepares it for delivery - by group. `data` is the type list's own number.
struct Kind {const char *name;int data;ModifierType type;};
struct KindGroup {const char *title;QVector<Kind> kinds;};
const QVector<KindGroup> &kindGroups() {
    static const QVector<KindGroup> groups{
        {QT_TRANSLATE_NOOP("ModifierPanel","Clean"),{{QT_TRANSLATE_NOOP("ModifierPanel","Crop"),0,ModifierType::Crop},{QT_TRANSLATE_NOOP("ModifierPanel","Remove green points"),2,ModifierType::RemoveGreen},
                          {QT_TRANSLATE_NOOP("ModifierPanel","Purge Isolated"),4,ModifierType::PurgeIsolated},{QT_TRANSLATE_NOOP("ModifierPanel","Eraser"),10,ModifierType::Erase}}},
        {QT_TRANSLATE_NOOP("ModifierPanel","Look"),{{QT_TRANSLATE_NOOP("ModifierPanel","Color"),9,ModifierType::Colour}}},
        {QT_TRANSLATE_NOOP("ModifierPanel","Motion"),{{QT_TRANSLATE_NOOP("ModifierPanel","Animate transform"),3,ModifierType::AnimateTransform},{QT_TRANSLATE_NOOP("ModifierPanel","Walk"),5,ModifierType::Walk},
                           {QT_TRANSLATE_NOOP("ModifierPanel","Loop morph"),11,ModifierType::LoopMorph}}},
        {QT_TRANSLATE_NOOP("ModifierPanel","Sound"),{{QT_TRANSLATE_NOOP("ModifierPanel","Audio"),8,ModifierType::Audio}}},
        {QT_TRANSLATE_NOOP("ModifierPanel","Delivery"),{{QT_TRANSLATE_NOOP("ModifierPanel","Prune low contribution"),7,ModifierType::PruneLowContribution},
                             {QT_TRANSLATE_NOOP("ModifierPanel","Bake anti-aliasing"),6,ModifierType::BakeAntialiasing}}}};
    return groups;
}
ModifierType typeOfData(int data) {
    for (const auto &group:kindGroups()) for (const auto &kind:group.kinds) if (kind.data==data) return kind.type;
    return ModifierType::Crop;
}
// A line drawn over the bars, letting the mouse through to them.
class PlayheadLine : public QWidget {
public:
    explicit PlayheadLine(QWidget *parent):QWidget(parent) {setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);}
    double x=-1;
protected:
    void paintEvent(QPaintEvent *) override {
        if (x<0) return;QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(0,0,0,90),3));painter.drawLine(QPointF(x,0),QPointF(x,height()));
        painter.setPen(QPen(Qt::white,2));painter.drawLine(QPointF(x,0),QPointF(x,height()));
    }
};
class CoverageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(int)> seek;
    std::function<void()> textMoved;
    double viewFirst=0,viewLast=0; // the frames the bars show
    QVector<int> chunkStarts; // the first frame of each source chunk after the first
    // A chunk's frames, inclusive; the last chunk runs to `maximum`.
    QPair<int,int> chunkFrames(int chunk,int maximum) const {
        const int first=chunk<=0 ? 0 : chunk-1<chunkStarts.size() ? chunkStarts[chunk-1] : maximum;
        const int last=chunk<chunkStarts.size() ? chunkStarts[chunk]-1 : maximum;
        return {first,std::max(first,last)};
    }
    int chunkOf(int frame) const {return int(std::upper_bound(chunkStarts.begin(),chunkStarts.end(),frame)-chunkStarts.begin());}
    double frameX(const QRect &bar,double frame) const {return bar.left()+bar.width()*(frame-viewFirst)/std::max(1e-9,viewLast-viewFirst);}
    bool inView(double frame) const {return frame>=viewFirst-1e-6 && frame<=viewLast+1e-6;}
    mutable int textOffset=-1; // unknown until a row has been painted
    QString hoverId; // the clip under the mouse: its fade handles show
    // The cache dot sits left of the eye: what follows is drawn this much further right.
    static constexpr int dotRoom=12;
    static QStyleOptionViewItem pastDot(QStyleOptionViewItem option) {option.rect.adjust(dotRoom,0,0,0);return option;}
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
                QStyleOptionViewItem copy(pastDot(option));initStyleOption(&copy,index);
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
        option=pastDot(option);initStyleOption(&option,index);const auto *style=option.widget ? option.widget->style() : QApplication::style();
        // Qt draws the text inside that rect, past a focus-frame margin of its own.
        const int margin=style->pixelMetric(QStyle::PM_FocusFrameHMargin,&option,option.widget)+1;
        return style->subElementRect(QStyle::SE_ItemViewItemText,&option,option.widget).adjusted(margin,0,-margin,0);
    }
    // Where a bar's range, its fades and their handles are, in pixels.
    struct Clip {double left=0,right=0,fadeIn=0,fadeOut=0;bool full=true;};
    Clip clipGeometry(const QRect &rect,const QVariantList &range,int maximum) const {
        Clip c;c.left=rect.left();c.right=rect.right()+1;
        if (!range.value(4).toBool()) return c;
        const auto [start,end]=rangeFrames(range,maximum);
        c.full=range.value(0).toInt()<0 && range.value(1).toInt()<0 && range.value(2).toInt()==0 && range.value(3).toInt()==0;
        const double left=frameX(rect,start-0.5),right=frameX(rect,end+0.5);
        c.left=std::clamp(left,double(rect.left()),double(rect.right()+1));c.right=std::clamp(right,double(rect.left()),double(rect.right()+1));
        c.fadeIn=std::max(0.,std::min(c.right-c.left,frameX(rect,start-0.5+range.value(2).toInt())-c.left));
        c.fadeOut=std::max(0.,std::min(c.right-c.left-c.fadeIn,c.right-frameX(rect,end+0.5-range.value(3).toInt())));
        return c;
    }
    // Thin, centred range bar; keyframe hits use the same horizontal extent.
    static QRect barRect(const QRect &cell) {const int height=std::max(13,cell.height()-9);return {cell.left()+8,cell.center().y()-height/2,cell.width()-16,height};}
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
        if (index.column()!=2) {
            auto copy=option;
            if (index.column()==0) {
                // Reserve room for the dot's content, while selection/hover
                // still paints the background across the entire cell.
                auto background=option;initStyleOption(&background,index);background.text.clear();background.icon={};
                background.features &= ~(QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDecoration);
                const auto *style=option.widget ? option.widget->style() : QApplication::style();
                style->drawControl(QStyle::CE_ItemViewItem,&background,painter,option.widget);
                copy=pastDot(option);
            }
            QStyledItemDelegate::paint(painter,copy,index);
            const int state=index.data(CacheRole).toInt();
            if (index.column()==0 && state) {
                const QPointF centre(option.rect.left()+dotRoom/2+2,option.rect.center().y());
                QColor colour=state==2 || state==4 ? QColor("#41b018") : state==3 ? QColor("#e8b04a") : state==5 || state==6 ? QColor("#0e0f10") : QColor("#737b83");
                if (state==3) colour.setAlphaF(.75+.25*(.5+.5*std::sin(QDateTime::currentMSecsSinceEpoch()/400.)));
                painter->save();painter->setRenderHint(QPainter::Antialiasing);painter->setPen(QPen(colour,1.2));
                painter->setBrush(state==1 || state==4 ? Qt::NoBrush : QBrush(colour));painter->drawEllipse(centre,3.5,3.5);
                if (state==4) {painter->setBrush(colour);painter->drawPie(QRectF(centre.x()-3.5,centre.y()-3.5,7,7),90*16,-180*16);}painter->restore();
            }
            return;
        }
        QStyleOptionViewItem copy(option);initStyleOption(&copy,index);copy.text.clear();
        const QWidget *widget=option.widget;(widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem,&copy,painter,widget);
        const bool active=index.data(Qt::UserRole+2).toBool() && (option.state & QStyle::State_Enabled);const auto type=ModifierType(index.data(Qt::UserRole+3).toInt());
        painter->save();const auto rect=barRect(option.rect);painter->setPen(Qt::NoPen);
        const int current=index.data(Qt::UserRole+6).toInt();
        // The track's clips, nearly as tall as the row, plain (the row names them);
        // the rest of the row a dark lane. A fade slopes a clip's top from nothing to full, its
        // handles showing on the selected clip and the one under the mouse. The selected clip
        // is brighter and outlined; a disabled one grey and hatched.
        const int maximum=index.data(Qt::UserRole+5).toInt();const auto clips=index.data(ClipsRole).toList();
        const bool rowEnabled=option.state & QStyle::State_Enabled;
        painter->setBrush(QColor(28,30,33));painter->drawRoundedRect(rect,3,3);
        for (const auto &entry:clips) {
            const auto c=entry.toMap();const auto range=c.value("range").toList();const auto clip=clipGeometry(rect,range,maximum);if (clip.right<=clip.left) continue;
            const bool on=c.value("active").toBool() && rowEnabled,chosen=c.value("selected").toBool();const auto kind=ModifierType(c.value("type").toInt());
            QColor fill=on ? modifierColour(kind) : QColor(70,74,79);if (chosen && on) fill=fill.lighter(112);
            const QPolygonF shape{{clip.left,double(rect.bottom()+1)},{clip.left+clip.fadeIn,double(rect.top())},{clip.right-clip.fadeOut,double(rect.top())},{clip.right,double(rect.bottom()+1)}};
            painter->setRenderHint(QPainter::Antialiasing,true);
            QPainterPath outline;outline.addPolygon(shape);outline.closeSubpath();
            if (clip.fadeIn==0 && clip.fadeOut==0) {outline=QPainterPath();outline.addRoundedRect(QRectF(clip.left,rect.top(),clip.right-clip.left,rect.height()+1),3,3);}
            painter->setPen(Qt::NoPen);painter->setBrush(fill);painter->drawPath(outline);
            if (!on) {painter->setBrush(QBrush(QColor(255,255,255,28),Qt::BDiagPattern));painter->drawPath(outline);}
            if (chosen) {painter->setPen(QPen(Qt::white,1.5));painter->setBrush(Qt::NoBrush);painter->drawPath(outline);painter->setPen(Qt::NoPen);}
            if (range.value(5).toBool() && (chosen || c.value("id").toString()==hoverId)) {
                painter->setPen(QPen(QColor(0,0,0,150),1));painter->setBrush(QColor("#e7eaeb"));
                for (const double x:{clip.left+clip.fadeIn,clip.right-clip.fadeOut}) painter->drawEllipse(QPointF(x,rect.top()+1.5),3.2,3.2);
                painter->setPen(Qt::NoPen);
            }
            painter->setRenderHint(QPainter::Antialiasing,false);
        }
        // An Erase's picked chunks: spans over their frames in the bar's own colour, a shade
        // darker; not diamonds, since a pick holds for its whole chunk rather than at one time.
        for (const auto &entry:index.data(Qt::UserRole+7).toList()) {
            const auto [first,last]=chunkFrames(entry.toList().value(0).toInt(),maximum);
            const double left=std::max<double>(rect.left(),frameX(rect,first)),right=std::min<double>(rect.right()+1,frameX(rect,last+1));
            if (right<=left) continue;
            painter->setBrush((!active ? QColor(75,80,86) : modifierColour(type)).darker(135));
            painter->drawRect(QRectF(left,rect.top()+2,right-left,rect.height()-4));
        }
        painter->setRenderHint(QPainter::Antialiasing,true);
        // The current time is one line over all the rows (the panel's PlayheadLine); dragging it
        // in any row moves the time (see the panel's event filter).
        for (const auto &key:index.data(Qt::UserRole+4).toList()) {
            const int frame=key.toInt();if (!inView(frame)) continue;const double x=frameX(rect,frame),y=rect.center().y();
            painter->setPen(QPen(QColor(0,0,0,170),1.2));painter->setBrush(active ? frame==current ? QColor("#ffe27a") : Qt::white : QColor("#9a9fa5"));painter->drawPolygon(QPolygonF{{x,y-5},{x+4,y},{x,y+5},{x-4,y}});
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
    // The kinds in the order a capture is usually worked on - clean it, its look, its motion,
    // its sound, then what prepares it for delivery - with separators between the groups. F1 to
    // F11 add them in that order.
    type_=new QComboBox;type_->setObjectName("newModifierType");type_->setItemDelegate(new KindListDelegate(type_));
    {
        int key=1;
        for (const auto &group:kindGroups()) {
            if (type_->count()) type_->insertSeparator(type_->count());
            for (const auto &kind:group.kinds) {
                const QString shortcut=QStringLiteral("F%1").arg(key++);
                type_->addItem(swatch(modifierColour(kind.type)),QStringLiteral("%1  (%2)").arg(tr(kind.name),shortcut),kind.data);
                // Each kind says what it does, in the list and in the context menu's Add modifier.
                type_->setItemData(type_->count()-1,typeDescription(kind.type)+"\n"+tr("Shortcut: %1").arg(shortcut),Qt::ToolTipRole);
                auto *add=new QShortcut(QKeySequence(shortcut),this);add->setContext(Qt::WindowShortcut);
                connect(add,&QShortcut::activated,this,[this,data=kind.data] {if (!isEnabled()) return;type_->setCurrentIndex(type_->findData(data));addModifier();});
            }
        }
    }
    // The swatches smaller here than in the menu, and the whole list open at once: eleven kinds and
    // the separators between their groups, no scrolling.
    type_->setIconSize({12,12});type_->setMaxVisibleItems(type_->count());
    toolbar->addWidget(type_);
    type_->setToolTip(tr("Choose a modifier to add.\nHover a kind to see what it does."));
    addModifier_=button(tr("+ Modifier"),"addModifier",tr("Add the selected modifier type to the stack. It acts on the whole timeline; drag its bar's ends to limit it to part of it."),[this] {addModifier();});
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
    tree_->setUniformRowHeights(true);tree_->setRootIsDecorated(false);tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);tree_->setMinimumHeight(90);tree_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    // Lanes: one shade, a dark line between tracks.
    tree_->setAlternatingRowColors(false);
    auto palette=tree_->palette();palette.setColor(QPalette::Base,EditorTheme::field());palette.setColor(QPalette::AlternateBase,EditorTheme::panel());
    palette.setColor(QPalette::Highlight,QColor("#333a40"));palette.setColor(QPalette::HighlightedText,EditorTheme::text());tree_->setPalette(palette);
    tree_->setStyleSheet("QTreeWidget {border-radius: 4px;} QTreeWidget::item {padding: 2px 5px; border-bottom: 2px solid #0e0f10;}"
        "QTreeWidget::item:selected {background: #333a40; color: #e7eaeb;} QTreeWidget::item:hover:!selected {background: #1b1e21;}");
    tree_->header()->setSectionResizeMode(0,QHeaderView::Fixed);tree_->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);tree_->header()->setSectionResizeMode(2,QHeaderView::Stretch);
    tree_->viewport()->installEventFilter(this);tree_->viewport()->setMouseTracking(true); // the playhead's hover cursor
    playheadLine_=new PlayheadLine(tree_->viewport());
    // Del or Backspace on the list removes the selected modifier, as in T4DEditor (not while
    // a name is being edited: the editor has the keys then).
    for (const QKeySequence &key:{QKeySequence(QKeySequence::Delete),QKeySequence(Qt::Key_Backspace)}) {
        auto *remove=new QShortcut(key,tree_);remove->setContext(Qt::WidgetShortcut);
        connect(remove,&QShortcut::activated,this,[this] {if (isEnabled() && !selectedIds().isEmpty()) removeSelection();});
    }
    connect(tree_->header(),&QHeaderView::sectionResized,this,&ModifierPanel::placePlayhead);
    connect(tree_->header(),&QHeaderView::geometriesChanged,this,&ModifierPanel::placePlayhead);
    connect(tree_->header(),&QHeaderView::sectionResized,this,&ModifierPanel::trackMoved);
    connect(tree_->header(),&QHeaderView::geometriesChanged,this,&ModifierPanel::trackMoved);
    auto *delegate=new CoverageDelegate(tree_);delegate->seek=[this](int frame) {emit seekFrame(frame);};
    // Measured while painting: refit and realign afterwards, not in the middle of a paint.
    delegate->textMoved=[this] {QMetaObject::invokeMethod(this,[this] {fitNames();emit trackMoved();},Qt::QueuedConnection);};tree_->setItemDelegate(delegate);tree_->setToolTip(tr("Click the eye to enable or disable a modifier.\nSelect a modifier to edit its properties in Tools.\nDrag a bar's ends to limit the modifier to part of the timeline, and the bar to move that part;\non Color and Audio the handles on its top corners fade it in and out.\nClick a keyframe diamond to seek, or drag the white time line.\nCtrl+wheel zooms the timeline, Shift+wheel or a middle drag pans it."));layout->addWidget(tree_,1); /* as tall as the timeline lets it be */
    // Rows added to or taken from the selection: their clips outlined or not.
    connect(tree_,&QTreeWidget::itemSelectionChanged,this,[this] {if (!updating_) refreshRows();});
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
        // A row already in the selection keeps it whole (the menu acts on all of it).
        auto *item=tree_->itemAt(position);
        if (item) {if (item->isSelected()) tree_->setCurrentItem(item,0,QItemSelectionModel::NoUpdate);else tree_->setCurrentItem(item);item=tree_->currentItem();}
        QMenu menu(this);auto *add=menu.addMenu(tr("Add modifier"));add->setToolTipsVisible(true);
        for (int type=0;type<type_->count();++type) {
            if (!type_->itemData(type).isValid()) {add->addSeparator();continue;} // the list's separators
            add->addAction(type_->itemIcon(type),type_->itemText(type),this,[this,type] {type_->setCurrentIndex(type);addModifier();})->setToolTip(type_->itemData(type,Qt::ToolTipRole).toString());
        }
        if (item) {
            menu.addSeparator();const bool enabled=item->checkState(0)==Qt::Checked;const auto ids=selectedIds();
            menu.addAction(enabled ? tr("Disable temporarily") : tr("Enable"),this,[this,enabled,ids] {
                // Every selected modifier at once, one step to undo.
                for (auto &m:project_.modifiers) if (ids.contains(m.id)) {m.enabled=!enabled;if (m.type==ModifierType::Crop) m.crop.enabled=m.enabled;}
                emit stackChanged();
            });
            menu.addAction(tr("Rename"),this,[this] {if (auto *current=tree_->currentItem()) tree_->editItem(current,0);});
            const int row=tree_->indexOfTopLevelItem(item);
            // Alone on its track, a clip can take the whole timeline back.
            if (row>=0 && row<rows_.size() && rows_[row].size()==1 && (project_.modifiers[rows_[row][0]].ranged() || project_.modifiers[rows_[row][0]].fadeIn>0))
                menu.addAction(tr("Whole timeline"),this,[this,id=project_.modifiers[rows_[row][0]].id] {
                    for (auto &m:project_.modifiers) if (m.id==id) {m.start=m.end=-1;m.fadeIn=m.fadeOut=0;emit stackChanged();return;}
                })->setToolTip(tr("Make it act on the whole timeline again, without fades."));
            menu.addAction(tr("Duplicate"),this,[this] {duplicateSelection();});
            menu.addAction(tr("Move up"),this,[this] {moveSelection(-1);});menu.addAction(tr("Move down"),this,[this] {moveSelection(1);});
            menu.addSeparator();menu.addAction(tr("Remove"),this,[this] {removeSelection();});
        }
        menu.exec(tree_->viewport()->mapToGlobal(position));
    });
    rebuild();
    cachePulse_=new QTimer(this);cachePulse_->setInterval(100);
    connect(cachePulse_,&QTimer::timeout,this,[this] {
        showCacheState();tree_->viewport()->update();
    });
}
// ---------------------------------------------------------------- kinds as tools
namespace {
// Line icons on a 32-point grid, drawn at four times the resolution, as T4DEditor's: white
// strokes of one width with rounded ends, and one touch of the kind's colour.
template<class Draw> QIcon lineIcon(const QColor &ink,Draw draw) {
    QPixmap pixmap(128,128);pixmap.setDevicePixelRatio(4);pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ink,2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);draw(p);p.end();return QIcon(pixmap);
}
const QColor toolInk(0xe7,0xea,0xeb);
// Switches the pen to the accent for what follows (the same width and ends); a dark kind's
// colour lifted so it still shows on the dark bar.
void accent(QPainter &p,const QColor &colour) {QPen pen=p.pen();pen.setColor(colour.lightnessF()<0.5 ? colour.lighter(150) : colour);p.setPen(pen);}
// Sparkles: the cleaning group, its small ones in the cleaning red.
QIcon cleaningIcon() {
    return lineIcon(toolInk,[](QPainter &p) {
        auto star=[&](QPointF c,double r) {QPainterPath s;s.moveTo(c.x(),c.y()-r);s.quadTo(c,QPointF(c.x()+r,c.y()));s.quadTo(c,QPointF(c.x(),c.y()+r));s.quadTo(c,QPointF(c.x()-r,c.y()));s.quadTo(c,QPointF(c.x(),c.y()-r));p.drawPath(s);};
        star({13,17},9);accent(p,modifierColour(ModifierType::Crop));star({24,8},4.5);star({24,24},3.5);
    });
}
// A package: the delivery group, its lid in the delivery green.
QIcon deliveryIcon() {
    return lineIcon(toolInk,[](QPainter &p) {
        p.drawPolygon(QPolygonF{{16,4},{27,9.5},{27,22.5},{16,28},{5,22.5},{5,9.5}});
        p.drawPolyline(QPolygonF{{5,9.5},{16,15},{27,9.5}});p.drawLine(QPointF(16,15),QPointF(16,28));
        accent(p,modifierColour(ModifierType::PruneLowContribution));p.drawLine(QPointF(10.5,6.8),QPointF(21.5,12.3));
    });
}
// The colour a group's icon and its menu's icons share: its first kind's (F1, F6, F11...).
QColor groupTint(int,ModifierType first) {return modifierColour(first);}
QIcon groupIcon(int group) {
    switch (group) {
    case 0: return cleaningIcon();
    case 1: return ModifierPanel::modifierIcon(ModifierType::Colour);
    case 2: return ModifierPanel::modifierIcon(ModifierType::AnimateTransform,QColor(),groupTint(2,ModifierType::AnimateTransform));
    case 3: return ModifierPanel::modifierIcon(ModifierType::Audio);
    default: return deliveryIcon();
    }
}
}
QIcon ModifierPanel::modifierIcon(ModifierType type,const QColor &ink,const QColor &tint) {
    const QColor c=tint.isValid() ? tint : modifierColour(type);
    return lineIcon(ink.isValid() ? ink : toolInk,[&](QPainter &p) {
        switch (type) {
        case ModifierType::Crop: // crop marks, one of them in its red
            p.drawPolyline(QPolygonF{{9,4},{9,23},{28,23}});accent(p,c);p.drawPolyline(QPolygonF{{4,9},{23,9},{23,28}});
            break;
        case ModifierType::RemoveGreen: { // a drop, struck through
            QPainterPath drop;drop.moveTo(16,4);drop.cubicTo(22,11,25,16,25,19.5);drop.cubicTo(25,24.5,21,28,16,28);drop.cubicTo(11,28,7,24.5,7,19.5);drop.cubicTo(7,16,10,11,16,4);
            p.drawPath(drop);accent(p,c);p.drawLine(QPointF(5,5),QPointF(27,27));
            break;
        }
        case ModifierType::PurgeIsolated: // a group kept, a stray crossed out
            for (const QPointF &at:{QPointF(9,13),QPointF(15,9),QPointF(14,17),QPointF(9,21)}) p.drawEllipse(at,2.4,2.4);
            accent(p,c);p.drawLine(QPointF(21,20),QPointF(27,26));p.drawLine(QPointF(27,20),QPointF(21,26));
            break;
        case ModifierType::Erase: // an eraser, its rubber in colour
            p.drawPolygon(QPolygonF{{11,27},{4.5,20.5},{18,7},{25.5,14.5},{13,27}});p.drawLine(QPointF(13,27),QPointF(28,27));
            accent(p,c);p.drawLine(QPointF(10,15),QPointF(17.5,22.5));
            break;
        case ModifierType::Colour: { // a contrast disc, its dark half in colour
            p.drawEllipse(QPointF(16,16),11,11);
            QPainterPath half;half.moveTo(16,5);half.arcTo(QRectF(5,5,22,22),90,180);half.closeSubpath();
            accent(p,c);p.setBrush(p.pen().color());p.drawPath(half);p.setBrush(Qt::NoBrush);
            break;
        }
        case ModifierType::AnimateTransform: { // a path between two keys
            QPainterPath path;path.moveTo(8,23);path.cubicTo(12,10,20,22,24,9);p.drawPath(path);
            accent(p,c);for (const QPointF &at:{QPointF(8,23),QPointF(24,9)}) p.drawPolygon(QPolygonF{at+QPointF(0,-3.5),at+QPointF(3.5,0),at+QPointF(0,3.5),at+QPointF(-3.5,0)});
            break;
        }
        case ModifierType::Walk: // moving forward along the floor
            p.drawLine(QPointF(4,26),QPointF(28,26));
            accent(p,c);p.drawPolyline(QPolygonF{{9,8},{16,15},{9,22}});p.drawPolyline(QPolygonF{{17,8},{24,15},{17,22}});
            break;
        case ModifierType::LoopMorph: // round and back to the start
            p.drawArc(QRectF(6,6,20,20),70*16,290*16);accent(p,c);p.drawPolyline(QPolygonF{{18,3},{21.5,6.5},{18,10}});
            break;
        case ModifierType::Audio: // a speaker and its sound
            p.drawPolygon(QPolygonF{{5,12},{10,12},{16,6.5},{16,25.5},{10,20},{5,20}});
            accent(p,c);p.drawArc(QRectF(13,10.5,9,11),-55*16,110*16);p.drawArc(QRectF(13,6,15,20),-55*16,110*16);
            break;
        case ModifierType::PruneLowContribution: // scissors, their handles in colour
            p.drawLine(QPointF(11.5,20.5),QPointF(22,5));p.drawLine(QPointF(20.5,20.5),QPointF(10,5));
            accent(p,c);p.drawEllipse(QPointF(9,23),3.6,3.6);p.drawEllipse(QPointF(23,23),3.6,3.6);
            break;
        case ModifierType::BakeAntialiasing: // a stepped edge made smooth
            p.drawPolyline(QPolygonF{{5,27},{5,21},{11,21},{11,15},{17,15},{17,9},{23,9},{23,4}});accent(p,c);p.drawLine(QPointF(10,28),QPointF(28,10));
            break;
        }
    });
}
void ModifierPanel::fillToolbar(QToolBar *bar) {
    int key=1,group=0;
    for (const auto &kinds:kindGroups()) {
        auto tip=[&](const Kind &kind,int number) {return QStringLiteral("<b>%1</b> (F%2)<br>%3").arg(tr(kind.name)).arg(number).arg(typeDescription(kind.type).toHtmlEscaped().replace("\n","<br>"));};
        if (kinds.kinds.size()==1) {
            // A group of one adds it straight away.
            const auto &kind=kinds.kinds.first();
            auto *action=bar->addAction(modifierIcon(kind.type),tr(kind.name));action->setObjectName(QStringLiteral("addModifier%1").arg(kind.data));
            action->setToolTip(tip(kind,key++));connect(action,&QAction::triggered,this,[this,type=kind.type] {addModifierOfType(type);});
        } else {
            auto *button=new QToolButton(bar);button->setObjectName(QStringLiteral("modifierGroup%1").arg(group));button->setIcon(groupIcon(group));
            button->setPopupMode(QToolButton::InstantPopup);button->setAutoRaise(true);
            auto *menu=new QMenu(button);menu->setToolTipsVisible(true);QStringList names;
            // The kinds in the menu take the group icon's colour, its first kind's.
            const QColor tint=groupTint(group,kinds.kinds.first().type);
            for (const auto &kind:kinds.kinds) {
                // The key after a tab: menus show it in their shortcut column.
                auto *action=menu->addAction(modifierIcon(kind.type,QColor(),tint),QStringLiteral("%1\tF%2").arg(tr(kind.name)).arg(key));
                action->setToolTip(tip(kind,key++));names << tr(kind.name);
                connect(action,&QAction::triggered,this,[this,type=kind.type] {addModifierOfType(type);});
            }
            button->setMenu(menu);button->setToolTip(QStringLiteral("<b>%1</b><br>%2").arg(tr(kinds.title),names.join(", ")));
            bar->addWidget(button);
        }
        ++group;
    }
}
void ModifierPanel::clearCacheState() {
    if (cachePulse_) cachePulse_->stop();
    cacheWorkingUntil_={};cacheState_={};cacheState_.enabled=cacheEnabled_;
    const QSignalBlocker blocker(tree_);
    const int despill=previewDespill(project_);
    const auto layers=cacheLayers();
    for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) {
        const int i=shownIn(row);auto *item=tree_->topLevelItem(row);const auto &m=std::as_const(project_.modifiers)[i];
        const bool eligible=m.active() && (m.type==ModifierType::PurgeIsolated || m.type==ModifierType::PruneLowContribution ||
            m.type==ModifierType::LoopMorph || (m.type==ModifierType::Erase && !m.erased.empty()) ||
            (m.type==ModifierType::Colour && m.colourDespill==Modifier::DespillAlways && i==despill));
        const bool direct=m.active() && (m.type==ModifierType::Crop || m.type==ModifierType::RemoveGreen || m.type==ModifierType::Colour);
        item->setData(0,CacheRole,!cacheEnabled_ ? 5 : layers[i].shared ? 6 : eligible ? 1 : direct ? 5 : 0);
        item->setToolTip(0,typeName(m)+"\n"+typeDescription(m.type)+(layers[i].shared ? "\n"+layers[i].detail : eligible ? cacheEnabled_ ? tr("\nCache: no frames retained yet.") : tr("\nCache disabled.") : direct ? tr("\nApplied directly on the GPU; no frame cache is needed.") : QString()));
    }
}
void ModifierPanel::setCacheEnabled(bool enabled) {cacheEnabled_=enabled;clearCacheState();}
void ModifierPanel::invalidateCacheState(bool visibility,bool sourceVisibility,bool morph,bool colour) {
    if (!cacheEnabled_) {clearCacheState();return;}
    const QSignalBlocker blocker(tree_);bool above=true;
    for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) {
        const auto &m=std::as_const(project_.modifiers)[shownIn(row)];auto *item=tree_->topLevelItem(row);
        if (!m.active()) {item->setData(0,CacheRole,0);continue;}
        if (item->data(0,CacheRole).toInt()==6) continue;
        if (m.active() && m.type==ModifierType::LoopMorph) {above=false;if (morph) item->setData(0,CacheRole,1);}
        else if (m.type==ModifierType::Colour && m.colourDespill==Modifier::DespillAlways) {if (colour) item->setData(0,CacheRole,1);}
        else if (m.type==ModifierType::PurgeIsolated || m.type==ModifierType::PruneLowContribution || m.type==ModifierType::Erase) {
            if (cacheShowsMorph_ && above ? sourceVisibility : visibility) item->setData(0,CacheRole,1);
        }
    }
}
void ModifierPanel::setCacheWorking(int groups) {
    if (!cacheEnabled_) return;
    const auto until=QDateTime::currentMSecsSinceEpoch()+650;
    for (int i=0;i<3;++i) if (groups & (1<<i)) cacheWorkingUntil_[i]=until;
    if (cachePulse_) cachePulse_->start();
    const QSignalBlocker blocker(tree_);
    for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) {
        auto *item=tree_->topLevelItem(row);const auto type=std::as_const(project_.modifiers)[shownIn(row)].type;
        const int group=type==ModifierType::LoopMorph ? 4 : type==ModifierType::Colour ? 2 : 1;
        const int old=item->data(0,CacheRole).toInt();if ((groups & group) && old>=1 && old<=4) item->setData(0,CacheRole,3);
    }
}
void ModifierPanel::setCacheState(const PreviewCacheState &state) {
    cacheState_=state;showCacheState();
}
void ModifierPanel::showCacheState() {
    const auto &state=cacheState_;
    cacheShowsMorph_=state.morph;
    if (!state.enabled || !cacheEnabled_) {clearCacheState();return;}
    const auto now=QDateTime::currentMSecsSinceEpoch();
    if (cachePulse_ && std::all_of(cacheWorkingUntil_.begin(),cacheWorkingUntil_.end(),[now](qint64 until) {return until<=now;})) cachePulse_->stop();
    const QSignalBlocker blocker(tree_);
    const auto layers=cacheLayers();
    for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) {
        const int i=shownIn(row);auto *item=tree_->topLevelItem(row);const auto &m=std::as_const(project_.modifiers)[i];const auto &layer=layers[i];
        int status=0;QString detail=layer.detail;
        if (layer.shared) status=6;
        else if (layer.cacheable) {
            status=layer.expected && layer.retained>=layer.expected ? 2 : layer.retained ? 4 : 1;
            item->setData(0,CacheProgressRole,layer.expected ? std::clamp(double(layer.retained)/layer.expected,0.,1.) : 0.);
            detail=tr("Cache: %1\n%2 frames / entries, %3 MiB\n%4").arg(layer.status,layer.frames).arg(layer.bytes/1048576.,0,'f',1).arg(layer.detail);
            if (m.type==ModifierType::LoopMorph && state.morph) detail+=state.shape ? tr("\nDisplayed morph frame is cached.") : tr("\nDisplayed morph frame is not cached.");
        } else if (m.active() && (m.type==ModifierType::Crop || m.type==ModifierType::RemoveGreen || m.type==ModifierType::Colour)) status=5;
        const int group=m.type==ModifierType::LoopMorph ? 2 : m.type==ModifierType::Colour ? 1 : 0;
        if (status>=1 && status<=4 && cacheWorkingUntil_[group]>now) status=3;
        item->setData(0,CacheRole,status);
        item->setToolTip(0,typeName(m)+"\n"+typeDescription(m.type)+(detail.isEmpty() ? QString() : "\n\n"+detail));
    }
}
QVector<ModifierPanel::CacheLayerInfo> ModifierPanel::cacheLayers() const {
    const auto &s=cacheState_;QVector<CacheLayerInfo> result;
    int morph=-1,despill=previewDespill(project_);std::array<int,3> owner{-1,-1,-1};std::array<bool,3> purge{};
    for (int i=0;i<project_.modifiers.size();++i) if (project_.modifiers[i].active() && project_.modifiers[i].type==ModifierType::LoopMorph) {morph=i;break;}
    auto mask=[](const Modifier &m) {return m.type==ModifierType::PurgeIsolated || m.type==ModifierType::PruneLowContribution || (m.type==ModifierType::Erase && !m.erased.empty());};
    for (int i=0;i<project_.modifiers.size();++i) {
        const auto &m=project_.modifiers[i];if (!m.active()) continue;
        if (mask(m)) {owner[0]=i;if (morph>=0 && i<morph) owner[1]=i;if (morph>=0 && i>morph && m.type==ModifierType::PurgeIsolated) owner[2]=i;}
        if (m.type==ModifierType::PurgeIsolated) {purge[0]=true;if (morph>=0) purge[i<morph ? 1 : 2]=true;}
        // With preview despill, green removal must test the source colour on
        // the CPU, even in the absence of a Purge/Erase/Prune layer.
        if (m.type==ModifierType::RemoveGreen && despill>=0 && project_.modifiers[despill].colourDespill==Modifier::DespillAlways) {
            if (owner[0]<0 || !mask(project_.modifiers[owner[0]])) owner[0]=i;
            if (morph>=0 && i<morph && (owner[1]<0 || !mask(project_.modifiers[owner[1]]))) owner[1]=i;
        }
    }
    auto name=[&](int i) {return project_.modifiers[i].name;};
    for (int i=0;i<project_.modifiers.size();++i) {
        const auto &m=project_.modifiers[i];CacheLayerInfo row;row.name=m.name;row.frames=QStringLiteral("\u2014");
        const int stage=morph>=0 && i>morph ? 2 : 1;
        int shared=-1;
        if (!m.active()) {row.status=tr("Disabled");row.detail=tr("This layer is disabled.");}
        else if (!cacheEnabled_) {row.status=tr("Cache disabled");row.detail=tr("Enable cache in the Cache menu to retain results.");}
        else if (m.type==ModifierType::LoopMorph) {
            row.cacheable=true;row.retained=s.morphFrames;row.expected=size_t(std::max(0,s.morphTotalFrames));
            row.bytes=s.morphBytes;row.frames=tr("%1 / %2").arg(qulonglong(s.morphFrames)).arg(s.morphTotalFrames);
            row.status=s.morphTotalFrames>0 && s.morphFrames>=size_t(s.morphTotalFrames) ? tr("Cached") : s.morphFrames ? tr("Partially cached") : tr("Not cached");
            row.detail=tr("Deformed shapes and shared SH endpoints. Only frames actually evaluated are retained; skipped playback frames may still be missing.");
        } else if (m.type==ModifierType::Colour && i==despill && m.colourDespill==Modifier::DespillAlways) {
            row.cacheable=true;row.retained=s.colourFrames;row.expected=size_t(std::max(0,s.timelineFrames-s.morphTotalFrames));
            row.bytes=s.colourBytes;row.frames=row.expected ? tr("%1 / %2").arg(qulonglong(row.retained)).arg(qulonglong(row.expected)) : QString::number(qulonglong(s.colourFrames));
            row.status=row.expected && row.retained>=row.expected ? tr("Cached") : row.retained ? tr("Partially cached") : tr("Not cached");
            row.detail=tr("Full precision despill RGB%1. Other color adjustments run on the GPU. SH is corrected on demand, without a full SH copy in each cache entry.").arg(m.colourRecoverSkin ? tr(" and skin recovery") : QString());
        } else if (mask(m) || (m.type==ModifierType::RemoveGreen && (owner[0]==i || owner[1]==i))) {
            size_t frames=0;
            size_t expected=0;
            for (int context=0;context<3;++context) if (owner[context]==i) {row.bytes+=s.visibilityStageBytes[context];frames+=s.visibilityStageFrames[context];}
            if (owner[0]==i) expected+=size_t(std::max(0,s.timelineFrames-s.morphTotalFrames));
            if (owner[2]==i) expected+=size_t(s.morphTotalFrames);
            if (row.bytes || owner[0]==i || owner[stage]==i) {
                const size_t timelineFrames=(owner[0]==i ? s.visibilityStageFrames[0] : 0)+(owner[2]==i ? s.visibilityStageFrames[2] : 0);
                row.cacheable=true;row.retained=expected ? timelineFrames : frames;row.expected=expected;
                row.frames=expected ? tr("%1 / %2").arg(qulonglong(timelineFrames)).arg(qulonglong(expected)) : QString::number(qulonglong(frames));
                row.status=expected && timelineFrames>=expected ? tr("Cached") : frames ? tr("Partially cached") : tr("Not cached");
                row.detail=tr("Shared visibility masks for Crop, Remove green, Eraser, Prune and Purge Isolated. Source, morph input and final morph masks are stored separately. Memory is charged once, to the last mask layer at each stage.");
                if (owner[1]==i) row.detail+=tr(" Also retains %1 morph input frame(s).").arg(qulonglong(s.visibilityStageFrames[1]));
            } else shared=owner[0];
        } else if (m.type==ModifierType::Crop || m.type==ModifierType::RemoveGreen) {
            if (purge[0]) shared=owner[stage]>=0 ? owner[stage] : owner[0];
            if (shared<0) {row.status=tr("Direct / GPU");row.detail=tr("Applied directly; no additional per-frame cache is needed.");}
        } else if (m.type==ModifierType::Colour) {
            row.status=tr("Direct / GPU");row.detail=tr("Opacity, exposure, white balance and saturation run on the GPU and need no frame cache.");
            if (m.colourDespill==Modifier::DespillOnExport) row.detail+=tr(" Despill and skin recovery are set to export only; they are not calculated or cached during Play.");
            else if (m.colourDespill==Modifier::DespillAlways && despill>=0) {
                row.detail+=tr(" Despill settings are superseded by %1.").arg(name(despill));
                if (project_.modifiers[despill].colourDespill==Modifier::DespillAlways) {row.shared=true;row.status=tr("Included in %1").arg(name(despill));}
            }
        } else {row.status=tr("No frame cache");row.detail=tr("This layer does not produce a per-frame preview cache.");}
        if (shared>=0) {row.shared=true;row.status=tr("Included in %1").arg(name(shared));row.detail=tr("Uses the visibility result retained by %1. No duplicate cache or additional memory is required.").arg(name(shared));}
        if (s.memoryLimited && m.active() && cacheEnabled_) row.detail+=tr(" Cache growth is paused to leave RAM available to the system.");
        result.append(row);
    }
    return result;
}
void ModifierPanel::computeRows() {
    rows_.clear();QString track;
    for (int i=0;i<project_.modifiers.size();++i) {
        const auto id=project_.modifiers[i].trackId();
        if (rows_.isEmpty() || id!=track) {rows_.append(QVector<int>{});track=id;}
        rows_.last().append(i);
    }
}
int ModifierPanel::rowOf(const QString &id) const {
    for (int row=0;row<rows_.size();++row) for (int i:rows_[row]) if (project_.modifiers[i].id==id) return row;
    return -1;
}
int ModifierPanel::shownIn(int row) const {
    const auto &clips=rows_[row];
    for (int i:clips) if (project_.modifiers[i].id==project_.selectedModifier) return i;
    for (int i:clips) if (project_.modifiers[i].covers(frame_)) return i;
    return clips.first();
}
void ModifierPanel::refreshRows() {
    // itemChanged can be emitted from inside QTreeWidgetItem::setData.
    // Keep these items alive while updating activation, name or appearance.
    const bool wasUpdating=updating_;updating_=true;const QSignalBlocker blocker(tree_);
    for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) {
        auto *item=tree_->topLevelItem(row);const auto &m=project_.modifiers[shownIn(row)];
        item->setText(0,m.name);item->setText(1,typeName(m));item->setToolTip(0,typeName(m)+"\n"+typeDescription(m.type));item->setToolTip(2,item->toolTip(0));
        item->setData(0,ModifierRole,m.id);item->setCheckState(0,m.active() ? Qt::Checked : Qt::Unchecked);
        item->setData(2,Qt::UserRole+2,m.active());item->setData(2,Qt::UserRole+3,int(m.type));item->setData(2,Qt::UserRole+5,maximum_);item->setData(2,Qt::UserRole+6,frame_);
        QVariantList clips,keys,erased;const bool rowSelected=item->isSelected();
        for (int i:rows_[row]) {
            const auto &clip=project_.modifiers[i];const bool chosen=clip.id==project_.selectedModifier || (rowSelected && clip.id==m.id);
            clips.append(QVariantMap{{"range",rangeOf(clip)},{"active",clip.active()},{"type",int(clip.type)},{"name",clip.name},{"id",clip.id},{"selected",chosen}});
            keys+=keyFrames(clip);erased+=erasedChunks(clip);
        }
        item->setData(2,ClipsRole,clips);item->setData(2,Qt::UserRole+4,keys);item->setData(2,Qt::UserRole+7,erased);
    }
    updating_=wasUpdating;tree_->viewport()->update();
}
void ModifierPanel::setProject(const Project &project) {
    const bool changed=renderedModifiers_!=visualState(project);const bool selected=project_.selectedModifier!=project.selectedModifier;
    project_=project;
    if (changed || selected) {
        // The same tracks: their rows stay, refreshed; others, the list built again.
        QStringList before;for (int row=0;row<tree_->topLevelItemCount() && row<rows_.size();++row) before << project_.modifiers.value(rows_[row].first()).trackId();
        const auto oldRows=rows_.size();computeRows();
        QStringList after;for (const auto &clips:rows_) after << project_.modifiers[clips.first()].trackId();
        if (oldRows!=rows_.size() || tree_->topLevelItemCount()!=rows_.size() || before!=after) rebuild();
        else {refreshRows();renderedModifiers_=visualState(project_);fitNames();}
    }
    if (selected) {
        // The selected modifier in sight: one just added shows even with the list scrolled away.
        updating_=true;const int row=rowOf(project_.selectedModifier);
        if (row>=0 && row<tree_->topLevelItemCount()) {tree_->setCurrentItem(tree_->topLevelItem(row));tree_->scrollToItem(tree_->topLevelItem(row));}
        updating_=false;
    }
}
void ModifierPanel::rebuild() {
    updating_=true;tree_->clear();computeRows();QTreeWidgetItem *selected=nullptr;
    for (int row=0;row<rows_.size();++row) {
        auto *item=new QTreeWidgetItem(tree_,{QString(),QString(),QString()});item->setFlags(item->flags()|Qt::ItemIsEditable|Qt::ItemIsUserCheckable);
        for (int i:rows_[row]) if (project_.selectedModifier==project_.modifiers[i].id) selected=item;
    }
    refreshRows();updating_=true;
    if (selected) {tree_->setCurrentItem(selected);tree_->scrollToItem(selected);}addModifier_->setEnabled(true);duplicate_->setEnabled(selected);remove_->setEnabled(selected);up_->setEnabled(selected);down_->setEnabled(selected);
    renderedModifiers_=visualState(project_);updating_=false;clearCacheState();fitNames();placePlayhead();
}
// As wide as the longest name, and never narrower than the minimum asked for.
void ModifierPanel::fitNames() {
    const int offset=textOffset();
    const int width=std::max(static_cast<QAbstractItemView *>(tree_)->sizeHintForColumn(0)+18,offset+minimumNameWidth_+34);
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
    delegate->viewFirst=first;delegate->viewLast=last;tree_->viewport()->update();placePlayhead();
}
// The bars' extent in the tree viewport, and the frame under an x there.
QPair<double,double> ModifierPanel::barSpan() const {
    const auto *header=tree_->header();
    const QRect bar=CoverageDelegate::barRect(QRect(header->sectionViewportPosition(2),0,header->sectionSize(2),30));
    return {double(bar.left()),double(bar.width())};
}
int ModifierPanel::frameAt(double x) const {
    const auto [left,width]=barSpan();const auto [first,last]=view();
    return std::clamp(int(std::lround(first+(x-left)/std::max(1.0,width)*(last-first))),0,std::max(0,maximum_));
}
void ModifierPanel::setChunks(const QVector<double> &boundaries) {
    QVector<int> starts;for (double frame:boundaries) starts.append(int(std::lround(frame)));
    auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());
    if (delegate->chunkStarts!=starts) {delegate->chunkStarts=starts;tree_->viewport()->update();}
}
// An Erase row's picked chunk under a point over the bars: its chunk index and picked records,
// or {-1,0}.
QPair<int,qulonglong> ModifierPanel::erasedChunkAt(const QPointF &at) const {
    const auto *item=tree_->itemAt(at.toPoint());const auto [left,width]=barSpan();
    if (!item || at.x()<left || at.x()>left+width) return {-1,0};
    const auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());const int chunk=delegate->chunkOf(frameAt(at.x()));
    for (const auto &entry:item->data(2,Qt::UserRole+7).toList()) if (entry.toList().value(0).toInt()==chunk) return {chunk,entry.toList().value(1).toULongLong()};
    return {-1,0};
}
ModifierPanel::RangePart ModifierPanel::rangePartAt(const QPointF &at,int *row,double *edge) const {
    // `row` comes back as the stack index of the clip found.
    auto *item=tree_->itemAt(at.toPoint());if (!item) return RangePart::None;
    const int line=tree_->indexOfTopLevelItem(item);if (line<0 || line>=rows_.size()) return RangePart::None;
    const QRect cell=tree_->visualRect(tree_->model()->index(line,2));const QRect rect=CoverageDelegate::barRect(cell);
    if (at.y()<rect.top()-5 || at.y()>rect.bottom()+5) return RangePart::None;
    const auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());const double x=at.x();
    // The fade handles first (they sit on the corners), then the nearest end, then a clip itself.
    RangePart best=RangePart::None;double bestDistance=1e9,found=x;int bestIndex=-1;
    for (int i:rows_[line]) {
        const auto &m=project_.modifiers[i];if (!Modifier::timed(m.type)) continue;
        const auto range=rangeOf(m);const auto clip=delegate->clipGeometry(rect,range,maximum_);
        auto consider=[&](RangePart part,double position,double reach) {
            const double distance=std::abs(x-position);
            if (distance<=reach && distance<bestDistance) {best=part;bestDistance=distance;found=position;bestIndex=i;}
        };
        if (range.value(5).toBool() && at.y()<rect.center().y()) {consider(RangePart::FadeIn,clip.left+clip.fadeIn,6);consider(RangePart::FadeOut,clip.right-clip.fadeOut,6);}
        if (best!=RangePart::FadeIn && best!=RangePart::FadeOut) {consider(RangePart::Start,clip.left,5);consider(RangePart::End,clip.right,5);}
    }
    if (best==RangePart::None)
        for (int i:rows_[line]) {
            const auto &m=project_.modifiers[i];if (!Modifier::timed(m.type)) continue;
            const auto clip=delegate->clipGeometry(rect,rangeOf(m),maximum_);
            if (x>clip.left && x<clip.right) {best=RangePart::Body;bestIndex=i;break;}
        }
    if (edge) *edge=found;
    if (best!=RangePart::None) *row=bestIndex;
    return best;
}
// The clip being dragged, from how it was at the press and how far the mouse went: never past
// its neighbours on the track.
void ModifierPanel::dragRange(double x) {
    if (rangeRow_<0 || rangeRow_>=project_.modifiers.size()) return;
    const auto &o=rangeOriginal_;Modifier m=project_.modifiers[rangeRow_];
    int low=0,high=maximum_;
    const int line=rowOf(o.id);
    if (line>=0) {
        const auto &clips=rows_[line];const int at=int(clips.indexOf(rangeRow_));
        if (at>0) {const auto &before=project_.modifiers[clips[at-1]];low=(before.end<0 ? maximum_ : before.end)+1;}
        if (at>=0 && at+1<clips.size()) {const auto &after=project_.modifiers[clips[at+1]];high=std::max(0,after.start)-1;}
    }
    const int start=std::max(0,o.start),end=o.end<0 ? maximum_ : o.end,length=end-start,frame=frameAt(x),delta=frame-frameAt(rangeFromX_);
    m.start=o.start;m.end=o.end;m.fadeIn=o.fadeIn;m.fadeOut=o.fadeOut;
    switch (rangePart_) {
    case RangePart::Body: m.start=std::clamp(start+delta,low,std::max(low,high-length));m.end=m.start+length;break;
    case RangePart::Start: m.start=std::clamp(start+delta,low,end);break;
    case RangePart::End: m.end=std::clamp(end+delta,start,high);break;
    case RangePart::FadeIn: m.fadeIn=std::clamp(frame-start+1,0,length+1-o.fadeOut);break;
    case RangePart::FadeOut: m.end=end;m.fadeOut=std::clamp(end-frame+1,0,length+1-o.fadeIn);break;
    case RangePart::None: return;
    }
    tidyRange(m,maximum_);
    // With neighbours, an end against the timeline's edge stays where it is.
    if (line>=0 && rows_[line].size()>1) {if (m.start<0 && low>0) m.start=low;if (m.end<0 && high<maximum_) m.end=high;}
    auto &target=project_.modifiers[rangeRow_];target.start=m.start;target.end=m.end;target.fadeIn=m.fadeIn;target.fadeOut=m.fadeOut;
    refreshRows();renderedModifiers_=visualState(project_);
}
void ModifierPanel::placePlayhead() {
    if (!playheadLine_) return;
    auto *line=static_cast<PlayheadLine *>(playheadLine_);
    const auto [left,width]=barSpan();const auto [first,last]=view();
    const bool shown=tree_->topLevelItemCount()>0 && frame_>=first-1e-6 && frame_<=last+1e-6 && last>first;
    line->x=shown ? left+width*(frame_-first)/(last-first) : -1;
    line->setGeometry(tree_->viewport()->rect());line->raise();line->update();
}
// Over the playhead line, in any modifier's row.
bool ModifierPanel::onPlayhead(const QPointF &at) const {
    const auto [left,width]=barSpan();const auto [first,last]=view();
    if (!tree_->itemAt(at.toPoint()) || frame_<first-1e-6 || frame_>last+1e-6) return false;
    return std::abs(at.x()-(left+width*(frame_-first)/std::max(1e-9,last-first)))<=6;
}
// Over the bars, Ctrl+wheel zooms the timeline and Shift+wheel, a horizontal wheel or a
// middle drag pans it; the plain wheel still scrolls the list. The playhead line drags to
// another time, for every modifier and the timeline at once.
bool ModifierPanel::eventFilter(QObject *watched,QEvent *event) {
    if (watched!=tree_->viewport()) return QWidget::eventFilter(watched,event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: case QEvent::MouseMove: case QEvent::MouseButtonRelease: {
        auto *mouse=static_cast<QMouseEvent *>(event);
        // The playhead, unless a bar's end or fade handle is nearer the mouse (at the first
        // frame both sit together).
        int nearRow=-1;double edge=0;const auto nearPart=rangePartAt(mouse->position(),&nearRow,&edge);
        const auto [barLeft,barWidth]=barSpan();const auto [viewFirst,viewLast]=view();
        const double playheadX=barLeft+barWidth*(frame_-viewFirst)/std::max(1e-9,viewLast-viewFirst);
        const bool endNearer=nearPart!=RangePart::None && nearPart!=RangePart::Body && std::abs(mouse->position().x()-edge)<std::abs(mouse->position().x()-playheadX);
        if (event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton && onPlayhead(mouse->position()) && !endNearer) {
            scrubbing_=true;tree_->viewport()->setCursor(Qt::SizeHorCursor);return true;
        }
        if (scrubbing_) {
            if (event->type()==QEvent::MouseMove) {const int frame=frameAt(mouse->position().x());if (frame!=frame_) emit seekFrame(frame);}
            else if (event->type()==QEvent::MouseButtonRelease && mouse->button()==Qt::LeftButton) {scrubbing_=false;if (!onPlayhead(mouse->position())) tree_->viewport()->unsetCursor();}
            return true;
        }
        // A bar dragged by its ends, its fade handles or itself: once the mouse has gone a few
        // pixels, so a click still selects the row (and goes to a key or an erased chunk).
        if (event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton) {
            int row=-1;rangePart_=rangePartAt(mouse->position(),&row);rangeDragging_=false;
            if (rangePart_!=RangePart::None) {
                rangeRow_=row;rangeFromX_=mouse->position().x();rangeOriginal_=project_.modifiers[row];
                // The clip pressed is the one selected, of the several on its track.
                if (project_.selectedModifier!=project_.modifiers[row].id) {project_.selectedModifier=project_.modifiers[row].id;refreshRows();emit selectionChanged();}
            }
        }
        if (event->type()==QEvent::MouseMove && rangePart_!=RangePart::None && (mouse->buttons() & Qt::LeftButton)) {
            if (!rangeDragging_ && std::abs(mouse->position().x()-rangeFromX_)<4) return true;
            if (!rangeDragging_ && rangePart_==RangePart::Body) tree_->viewport()->setCursor(Qt::ClosedHandCursor);
            rangeDragging_=true;dragRange(mouse->position().x());return true;
        }
        if (event->type()==QEvent::MouseButtonRelease && mouse->button()==Qt::LeftButton && rangePart_!=RangePart::None) {
            const bool dragged=rangeDragging_;rangePart_=RangePart::None;rangeDragging_=false;tree_->viewport()->unsetCursor();
            if (dragged) emit stackChanged(); // the view still sees the release, as after any press
        }
        // A click on an Erase's picked chunk goes to its first frame, unless the time is already
        // in it; the row is selected as usual.
        if (event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton) {
            const auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());
            const int chunk=erasedChunkAt(mouse->position()).first;
            if (chunk>=0 && delegate->chunkOf(frame_)!=chunk) emit seekFrame(delegate->chunkFrames(chunk,maximum_).first);
        }
        if (event->type()==QEvent::MouseMove && !panning_ && mouse->buttons()==Qt::NoButton) {
            // Arrows at a clip's ends and fade handles (and on the playhead), a hand over a clip
            // that can be moved; its fade handles show while the mouse is on it.
            const auto part=onPlayhead(mouse->position()) && !endNearer ? RangePart::Start : nearPart;
            if (part==RangePart::Start || part==RangePart::End || part==RangePart::FadeIn || part==RangePart::FadeOut) tree_->viewport()->setCursor(Qt::SizeHorCursor);
            else if (part==RangePart::Body) tree_->viewport()->setCursor(Qt::OpenHandCursor);
            else tree_->viewport()->unsetCursor();
            auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());
            const QString hovered=nearPart!=RangePart::None && nearRow>=0 && nearRow<project_.modifiers.size() ? project_.modifiers[nearRow].id : QString();
            if (delegate->hoverId!=hovered) {delegate->hoverId=hovered;tree_->viewport()->update();}
        }
        break;
    }
    case QEvent::Resize: placePlayhead();break;
    case QEvent::Leave: {auto *delegate=static_cast<CoverageDelegate *>(tree_->itemDelegate());if (!delegate->hoverId.isEmpty()) {delegate->hoverId.clear();tree_->viewport()->update();}break;}
    case QEvent::ToolTip: {
        auto *help=static_cast<QHelpEvent *>(event);const auto [chunk,records]=erasedChunkAt(help->pos());
        if (chunk<0) break;
        const auto [first,last]=static_cast<CoverageDelegate *>(tree_->itemDelegate())->chunkFrames(chunk,maximum_);
        QToolTip::showText(help->globalPos(),tr("Chunk %1 (frames %2\u2013%3): %4 splats erased by hand.\nPicks hold for the whole chunk; click to go to its first frame.").arg(chunk).arg(first).arg(last).arg(records),tree_->viewport());
        return true;
    }
    default: break;
    }
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
    // The name a track shows follows the clip at the time.
    refreshRows();placePlayhead();
}
void ModifierPanel::addModifier() {addModifierOfType(typeOfData(type_->currentData().toInt()));}
void ModifierPanel::addModifierOfType(ModifierType type) {
    if (!isEnabled()) return;
    Modifier m;m.id=Project::newId();m.type=type;
    m.crop.enabled=true;m.crop.shape=CropShape::Cylinder; // Shape is chosen afterwards in the crop parameters.
    // The first of a kind keeps the bare name ("Crop"); later ones take the next free number ("Crop 2", "Crop 3"...).
    const QString base=m.type==ModifierType::Crop ? tr("Crop") : typeName(m);
    auto taken=[this](const QString &name) {return std::any_of(project_.modifiers.cbegin(),project_.modifiers.cend(),[&](const Modifier &other) {return other.name==name;});};
    m.name=base;for (int number=2;taken(m.name);++number) m.name=QString("%1 %2").arg(base).arg(number);
    // On the selected modifier's track when it is of this kind and the time falls in a gap
    // there: the new clip starts at the time and runs to the next clip (or the end).
    const int line=rowOf(project_.selectedModifier);qsizetype at=project_.modifiers.size();
    if (line>=0 && Modifier::timed(type) && project_.modifiers[rows_[line].first()].type==type) {
        int low=0,high=maximum_;bool free=true;
        for (int i:rows_[line]) {
            const auto &clip=project_.modifiers[i];const int start=std::max(0,clip.start),end=clip.end<0 ? maximum_ : clip.end;
            if (frame_>=start && frame_<=end) {free=false;break;}
            if (end<frame_) low=std::max(low,end+1);
            if (start>frame_) high=std::min(high,start-1);
        }
        if (free && low<=high) {
            m.track=project_.modifiers[rows_[line].first()].trackId();m.start=frame_;m.end=high;
            for (int i:rows_[line]) if (project_.modifiers[i].track.isEmpty()) project_.modifiers[i].track=m.track;
            at=rows_[line].last()+1;
            for (int i:rows_[line]) if (std::max(0,project_.modifiers[i].start)>high) {at=i;break;}
            tidyRange(m,maximum_);
            // Open ends belong to a lone clip: with neighbours, they are frames.
            if (m.start<0 && frame_>0) m.start=frame_;if (m.end<0 && high<maximum_) m.end=high;
        }
    }
    project_.modifiers.insert(at,m);project_.selectedModifier=m.id;emit stackChanged();if (m.type==ModifierType::Crop) emit cropAdded();
}
QStringList ModifierPanel::selectedIds() const {
    QStringList ids;
    for (auto *item:tree_->selectedItems()) if (const auto id=item->data(0,ModifierRole).toString();!id.isEmpty() && !ids.contains(id)) ids << id;
    if (ids.isEmpty() && !project_.selectedModifier.isEmpty()) ids << project_.selectedModifier;
    return ids;
}
void ModifierPanel::removeSelection() {
    // Every selected modifier at once (one step to undo); then the one that takes the first
    // one's place: another clip of its track, else the row there now.
    const auto ids=selectedIds();if (ids.isEmpty()) return;
    int first=-1;QString track;
    for (int i=0;i<project_.modifiers.size();++i) if (ids.contains(project_.modifiers[i].id)) {first=i;track=project_.modifiers[i].trackId();break;}
    if (first<0) return;
    project_.modifiers.erase(std::remove_if(project_.modifiers.begin(),project_.modifiers.end(),[&](const Modifier &m) {return ids.contains(m.id);}),project_.modifiers.end());
    QString next;
    for (int j=std::max(0,first-1);j<std::min(int(project_.modifiers.size()),first+1);++j) if (project_.modifiers[j].trackId()==track) next=project_.modifiers[j].id;
    if (next.isEmpty() && !project_.modifiers.isEmpty()) next=project_.modifiers[std::min(first,int(project_.modifiers.size()-1))].id;
    project_.selectedModifier=next;emit stackChanged();
}
void ModifierPanel::duplicateSelection() {
    // Each selected modifier copied onto a track of its own, right below its original's track.
    const auto ids=selectedIds();QString last;
    for (const auto &id:ids) {
        computeRows();const int line=rowOf(id);if (line<0) continue;
        for (int i:rows_[line]) if (project_.modifiers[i].id==id) {
            auto m=project_.modifiers[i];m.id=Project::newId();m.track.clear();m.name+=tr(" copy");project_.modifiers.insert(rows_[line].last()+1,m);last=m.id;break;
        }
    }
    if (last.isEmpty()) return;
    project_.selectedModifier=last;emit stackChanged();
}
void ModifierPanel::moveSelection(int direction) {
    // The whole track, past the next one up or down.
    const int line=rowOf(project_.selectedModifier),other=line+direction;
    if (line<0 || other<0 || other>=rows_.size()) return;
    const int first=std::min(line,other),second=std::max(line,other);
    QVector<Modifier> reordered;
    for (int row=0;row<rows_.size();++row) {const int take=row==first ? second : row==second ? first : row;for (int i:rows_[take]) reordered.append(project_.modifiers[i]);}
    project_.modifiers=reordered;emit stackChanged();
}
