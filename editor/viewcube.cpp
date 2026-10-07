#include "viewcube.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

static const ViewPreset presets[] = {ViewPreset::Front,ViewPreset::Back,ViewPreset::Left,ViewPreset::Right,ViewPreset::Top,ViewPreset::Bottom};
static const char *names[] = {"FRONT","BACK","LEFT","RIGHT","TOP","BOTTOM"};
ViewCube::ViewCube(QWidget *parent) : QWidget(parent) {
    setFixedSize(156,160); setMouseTracking(true); setCursor(Qt::PointingHandCursor);
    setObjectName("viewCube"); setToolTip(tr("Click a face or a view button. Orbiting returns to perspective."));
}
void ViewCube::setCamera(const Camera &camera) { camera_ = camera; update(); }
void ViewCube::setDisplayControls(QWidget *controls) {
    controls->setParent(this);controls->setFixedWidth(144);controls->adjustSize();controls->move(6,161);controls->show();
    setFixedHeight(161+controls->height()+8);
}
QRectF ViewCube::buttonRect(int i) const { return {7.0+(i%2)*73,88.0+(i/2)*23,69,20}; }
std::vector<ViewCube::Face> ViewCube::faces() const {
    const int indices[][4] = {{4,5,7,6},{0,2,3,1},{0,4,6,2},{1,3,7,5},{2,6,7,3},{0,1,5,4}};
    const QVector3D normals[] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    const auto view = camera_.viewMatrix(); std::vector<Face> result;
    QVector3D corners[8];
    for (int i=0; i<8; ++i) corners[i] = view.mapVector({(i&1)?1.0f:-1.0f,(i&2)?1.0f:-1.0f,(i&4)?1.0f:-1.0f});
    for (int i=0; i<6; ++i) {
        if (view.mapVector(normals[i]).z()<0.001f) continue;
        Face face; face.preset = presets[i]; face.depth = 0;
        for (int k : indices[i]) { face.polygon << QPointF(78+corners[k].x()*26,44-corners[k].y()*26); face.depth += corners[k].z(); }
        result.push_back(face);
    }
    std::sort(result.begin(),result.end(),[](const Face &a,const Face &b) { return a.depth<b.depth; });
    return result;
}
ViewPreset ViewCube::hit(const QPointF &point) const {
    for (int i=0; i<6; ++i) if (buttonRect(i).contains(point)) return presets[i];
    auto polygons = faces();
    for (auto i=polygons.rbegin(); i!=polygons.rend(); ++i) if (i->polygon.containsPoint(point,Qt::OddEvenFill)) return i->preset;
    return ViewPreset::Free;
}
void ViewCube::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor("#555")); painter.setBrush(QColor("#252525")); painter.drawRoundedRect(rect().adjusted(1,1,-1,-1),6,6);
    painter.setFont(QFont("Segoe UI",8)); painter.setPen(QColor("#bbb"));
    for (const auto &face : faces()) {
        const bool active = face.preset==camera_.preset;
        painter.setBrush(face.preset==hover_ ? QColor("#497957") : active ? QColor("#2e6d4e") : QColor("#414141"));
        painter.setPen(QColor("#aaa")); painter.drawPolygon(face.polygon);
        const int index = int(std::find(std::begin(presets),std::end(presets),face.preset)-std::begin(presets));
        painter.setPen(Qt::white); painter.drawText(face.polygon.boundingRect(),Qt::AlignCenter,QString::fromLatin1(names[index]));
    }
    for (int i=0; i<6; ++i) {
        painter.setBrush(presets[i]==hover_ ? QColor("#497957") : presets[i]==camera_.preset ? QColor("#2e6d4e") : QColor("#353535"));
        painter.setPen(QColor("#555")); painter.drawRoundedRect(buttonRect(i),3,3);
        painter.setPen(Qt::white); painter.drawText(buttonRect(i),Qt::AlignCenter,QString::fromLatin1(names[i]));
    }
}
void ViewCube::leaveEvent(QEvent *event) {
    hover_ = ViewPreset::Free; update(); QWidget::leaveEvent(event);
}
void ViewCube::mousePressEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton) return;
    const auto preset = hit(event->position());
    if (preset!=ViewPreset::Free) emit viewSelected(preset);
    event->accept();
}
void ViewCube::mouseMoveEvent(QMouseEvent *event) {
    hover_ = hit(event->position());
    const char *keys[] = {"Numpad 1","Ctrl+Numpad 1","Ctrl+Numpad 3","Numpad 3","Numpad 7","Ctrl+Numpad 7"};
    const int index = int(std::find(std::begin(presets),std::end(presets),hover_)-std::begin(presets));
    setToolTip(index<6 ? tr("Switch to %1 view (%2). Orbiting returns to perspective.").arg(QString::fromLatin1(names[index]),QString::fromLatin1(keys[index]))
        : tr("Click a face to select a view. Reset user perspective with Numpad 0."));
    update();
}
