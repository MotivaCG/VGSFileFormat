// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "project.h"
#include "captureworker.h"
#include <QWidget>
class QTreeWidget;
class QToolButton;
class QComboBox;
class QTimer;
class ModifierPanel : public QWidget {
    Q_OBJECT
public:
    explicit ModifierPanel(QWidget *parent=nullptr);
    void setProject(const Project &project);
    void setTimeline(int frame,int maximum);
    void setCacheState(const PreviewCacheState &state);
    void clearCacheState();
    void setCacheWorking(int groups=7);
    void setCacheEnabled(bool enabled);
    void invalidateCacheState(bool visibility,bool sourceVisibility,bool morph,bool colour);
    struct CacheLayerInfo {QString name,status,frames,detail;size_t bytes=0,retained=0,expected=0;bool cacheable=false,shared=false;};
    QVector<CacheLayerInfo> cacheLayers() const;
    const PreviewCacheState &cacheState() const {return cacheState_;}
    // The capture's chunk boundaries, in frames: an Erase bar marks the chunks it has picks in.
    void setChunks(const QVector<double> &boundaries);
    const Project &project() const {return project_;}
    // The modifier bars' horizontal extent in global coordinates: left edge and width.
    QPair<int,int> trackSpan() const;
    // Where the modifier names' text starts, in global x.
    int nameLeft() const;
    // The names column is at least wide enough for a name of this many pixels: the timeline's
    // own name sits in that column's space above the list.
    void setMinimumNameWidth(int pixels);
    // The frames the bars show, matching the timeline's zoom and pan.
    void setView(double first,double last);
    QPair<double,double> view() const;
signals:
    void stackChanged();
    void selectionChanged();
    void cropAdded();
    void seekFrame(int frame);
    // The bars moved or changed width: the names column, the panel or its scroll bar changed.
    void trackMoved();
    // Zoom and pan asked for over the bars; the timeline owns the view and applies them.
    void zoomRequested(double factor,int globalX);
    void panRequested(double pixels);
    void resetViewRequested();
private:
    void rebuild();
    void fitNames();
    int textOffset() const;
    int minimumNameWidth_=0;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched,QEvent *event) override;
    bool panning_=false,scrubbing_=false;
    QPair<double,double> barSpan() const;
    int frameAt(double x) const;
    bool onPlayhead(const QPointF &at) const;
    QPair<int,qulonglong> erasedChunkAt(const QPointF &at) const;
    // A bar's part of the timeline, edited like a clip: the bar moves it, its ends trim it
    // and, on Color and Audio, the handles at its top corners set the fades.
    enum class RangePart {None,Body,Start,End,FadeIn,FadeOut};
    // `edge`: the x of the end or handle found, to tell it from the playhead when both are near.
    RangePart rangePartAt(const QPointF &at,int *row,double *edge=nullptr) const;
    void dragRange(double x);
    // The current time as one line down the whole bars area, over every row and the space
    // below them, as the timeline's playhead continued.
    QWidget *playheadLine_=nullptr;
    void placePlayhead();
    int rangeRow_=-1;RangePart rangePart_=RangePart::None;double rangeFromX_=0;bool rangeDragging_=false;Modifier rangeOriginal_;
    double panX_=0;
    void addModifier();
    void removeSelection();
    void duplicateSelection();
    void moveSelection(int direction);
    Project project_;
    QTreeWidget *tree_;
    QTimer *cachePulse_=nullptr;
    PreviewCacheState cacheState_;
    std::array<qint64,3> cacheWorkingUntil_{};
    void showCacheState();
    bool cacheEnabled_=true,cacheShowsMorph_=false;
    QComboBox *type_;
    QToolButton *addModifier_,*remove_,*duplicate_,*up_,*down_;
    bool updating_=false;
    QJsonArray renderedModifiers_;
    int frame_=0,maximum_=0;
};
