// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "animationpanel.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

namespace {
QString vectorText(QVector3D v) {return QString("%1, %2, %3").arg(v.x(),0,'g',6).arg(v.y(),0,'g',6).arg(v.z(),0,'g',6);}
}
AnimationPanel::AnimationPanel(QWidget *parent):QGroupBox(tr("Animate transform"),parent) {
    setObjectName("animationModifierProperties");setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);auto *layout=new QVBoxLayout(this);auto *buttons=new QHBoxLayout;
    auto *setKey=new QPushButton(tr("Set key"));setKey->setObjectName("setTransformKey");setKey->setToolTip(tr("Add or update a key at the current frame. Editing XYZ or the gizmo also sets a key while this modifier is selected."));buttons->addWidget(setKey);
    remove_=new QPushButton(tr("Remove key"));remove_->setObjectName("removeTransformKey");remove_->setToolTip(tr("Remove the selected key, or the key at the current frame."));buttons->addWidget(remove_);layout->addLayout(buttons);
    table_=new QTableWidget;table_->setObjectName("transformKeyTable");table_->setColumnCount(4);table_->setHorizontalHeaderLabels({tr("Frame"),tr("Position"),tr("Rotation"),tr("Scale")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setSelectionMode(QAbstractItemView::SingleSelection);table_->setAlternatingRowColors(true);table_->verticalHeader()->hide();table_->verticalHeader()->setDefaultSectionSize(28);table_->setMinimumHeight(140);table_->setMaximumHeight(220);
    auto palette=table_->palette();palette.setColor(QPalette::Highlight,QColor(73,73,73));palette.setColor(QPalette::HighlightedText,Qt::white);table_->setPalette(palette);
    table_->setStyleSheet("QTableWidget::item:selected { background: #494949; color: #ffffff; }");
    table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);for (int col=1;col<4;++col) table_->horizontalHeader()->setSectionResizeMode(col,QHeaderView::Stretch);
    table_->setToolTip(tr("Reference-space offsets. Double-click a cell to edit frame or XYZ values. Click a row to seek to its frame."));layout->addWidget(table_);
    auto *note=new QLabel(tr("Offsets from the reference pose. Position and scale interpolate linearly; rotation follows the shortest quaternion path. Moving at the current frame creates or updates its key."));note->setWordWrap(true);layout->addWidget(note);
    connect(setKey,&QPushButton::clicked,this,&AnimationPanel::setKeyRequested);
    connect(remove_,&QPushButton::clicked,this,[this] {const int row=table_->currentRow();animation_.removeKey(row>=0 && row<animation_.keys.size() ? animation_.keys[row].frame : frame_);emit animationChanged(animation_);});
    connect(table_,&QTableWidget::cellClicked,this,[this](int row,int) {if (!updating_ && row<animation_.keys.size()) emit seekFrame(animation_.keys[row].frame);});
    connect(table_,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item) {
        if (updating_ || item->row()>=animation_.keys.size()) return;const auto previous=animation_;auto &key=animation_.keys[item->row()];bool valid=true;
        if (item->column()==0) {bool ok;const int frame=item->text().toInt(&ok);valid=ok && frame>=0 && frame<=maximum_;for (int i=0;i<animation_.keys.size();++i) if (i!=item->row() && animation_.keys[i].frame==frame) valid=false;if (valid) key.frame=frame;}
        else {
            const auto fields=item->text().split(QRegularExpression("[\\s,;]+"),Qt::SkipEmptyParts);valid=fields.size()==3;QVector3D value;
            for (int axis=0;valid && axis<3;++axis) {bool ok;const double v=fields[axis].toDouble(&ok);valid=ok && std::isfinite(v) && std::abs(v)<= (item->column()==2 ? 36000 : 1e6) && (item->column()!=3 || (v>=.0001 && v<=10000));value[axis]=float(v);}
            if (valid) (item->column()==1 ? key.offset.position : item->column()==2 ? key.offset.rotation : key.offset.scale)=value;
        }
        if (!valid) {setAnimation(previous,frame_,maximum_);return;}
        std::sort(animation_.keys.begin(),animation_.keys.end(),[](const auto &a,const auto &b) {return a.frame<b.frame;});emit animationChanged(animation_);
    });
}
void AnimationPanel::setAnimation(const TransformAnimation &animation,int frame,int maximum) {
    updating_=true;QSignalBlocker blocker(table_);animation_=animation;frame_=frame;maximum_=maximum;table_->setRowCount(animation.keys.size());
    for (int row=0;row<animation.keys.size();++row) {
        const auto &key=animation.keys[row];const QString values[]={QString::number(key.frame),vectorText(key.offset.position),vectorText(key.offset.rotation),vectorText(key.offset.scale)};
        for (int col=0;col<4;++col) {auto *item=table_->item(row,col);if (!item) {item=new QTableWidgetItem;table_->setItem(row,col,item);}if (item->text()!=values[col]) item->setText(values[col]);}
        if (key.frame==frame) table_->setCurrentCell(row,0);
    }
    remove_->setEnabled(!animation.keys.isEmpty());updating_=false;
    table_->setFixedHeight(std::clamp(table_->horizontalHeader()->height()+4+28*std::max(1,int(animation.keys.size())),100,220));
}
