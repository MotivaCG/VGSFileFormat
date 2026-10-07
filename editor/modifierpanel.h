#pragma once
#include "project.h"
#include <QWidget>
class QTreeWidget;
class QToolButton;
class QComboBox;
class ModifierPanel : public QWidget {
    Q_OBJECT
public:
    explicit ModifierPanel(QWidget *parent=nullptr);
    void setProject(const Project &project);
    void setTimeline(int frame,int maximum);
    const Project &project() const {return project_;}
signals:
    void stackChanged();
    void selectionChanged();
    void cropAdded();
    void seekFrame(int frame);
private:
    void rebuild();
    void addModifier();
    void removeSelection();
    void duplicateSelection();
    void moveSelection(int direction);
    Project project_;
    QTreeWidget *tree_;
    QComboBox *type_;
    QToolButton *addModifier_,*remove_,*duplicate_,*up_,*down_;
    bool updating_=false;
    QJsonArray renderedModifiers_;
    int frame_=0,maximum_=0;
};
