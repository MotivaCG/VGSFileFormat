#pragma once
#include "project.h"
#include <QPolygonF>
#include <QWidget>
#include <vector>

class ViewCube : public QWidget {
    Q_OBJECT
public:
    explicit ViewCube(QWidget *parent);
    void setCamera(const Camera &camera);
signals:
    void viewSelected(ViewPreset preset);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    struct Face { QPolygonF polygon; ViewPreset preset; float depth; };
    std::vector<Face> faces() const;
    ViewPreset hit(const QPointF &point) const;
    QRectF buttonRect(int index) const;
    Camera camera_;
    ViewPreset hover_ = ViewPreset::Free;
};
