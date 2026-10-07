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
