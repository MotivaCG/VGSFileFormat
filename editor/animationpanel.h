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
#include <QGroupBox>
class QTableWidget;
class QPushButton;
class AnimationPanel : public QGroupBox {
    Q_OBJECT
public:
    explicit AnimationPanel(QWidget *parent=nullptr);
    void setAnimation(const TransformAnimation &,int frame,int maximum);
signals:
    void animationChanged(TransformAnimation animation);
    void setKeyRequested();
    void seekFrame(int frame);
private:
    TransformAnimation animation_;
    QTableWidget *table_;
    QPushButton *remove_;
    int frame_=0,maximum_=0;
    bool updating_=false;
};
