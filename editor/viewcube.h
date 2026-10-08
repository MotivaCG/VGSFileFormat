#pragma once
#include "project.h"
#include <QPolygonF>
#include <QWidget>
#include <vector>

class ViewCube : public QWidget {
    Q_OBJECT
public:
    static constexpr int rowHeight = 26; // shared by the painted Background button and the Point size field
    explicit ViewCube(QWidget *parent);
    void setCamera(const Camera &camera);
    void setDisplayControls(QWidget *controls);
    void setLightBackground(bool light);
signals:
    void viewSelected(ViewPreset preset);
    void lightBackgroundSelected(bool light);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    struct Face { QPolygonF polygon; ViewPreset preset; float facing; }; // facing: 1 = toward the camera, 0 = edge-on
    std::vector<Face> faces() const;
    ViewPreset hit(const QPointF &point) const;
    QRectF backgroundRect() const;
    Camera camera_;
    ViewPreset hover_ = ViewPreset::Free;
    bool backgroundHover_ = false;
    bool light_ = false;
};
