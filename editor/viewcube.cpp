// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "viewcube.h"
#include "editortheme.h"
#include <QMouseEvent>
#include <QPainter>
#include <QFontMetricsF>
#include <QTransform>
#include <algorithm>
#include <cmath>

static const ViewPreset presets[] = {ViewPreset::Front,ViewPreset::Back,ViewPreset::Left,ViewPreset::Right,ViewPreset::Top,ViewPreset::Bottom};
static const char *names[] = {"FRONT","BACK","LEFT","RIGHT","TOP","BOTTOM"}; // cube faces
static const char *viewNames[] = {QT_TRANSLATE_NOOP("ViewCube","Front"),QT_TRANSLATE_NOOP("ViewCube","Back"),QT_TRANSLATE_NOOP("ViewCube","Left"),
    QT_TRANSLATE_NOOP("ViewCube","Right"),QT_TRANSLATE_NOOP("ViewCube","Top"),QT_TRANSLATE_NOOP("ViewCube","Bottom")};
// Every control below the cube shares these edges and row height, painted or widget.
static constexpr double controlLeft = 7, controlWidth = 142, firstRow = 88, gap = 4;
static constexpr int displayTop = int(firstRow+ViewCube::rowHeight+gap);
ViewCube::ViewCube(QWidget *parent) : QWidget(parent) {
    setFixedSize(156,int(firstRow+rowHeight+controlLeft)); setMouseTracking(true); setCursor(Qt::PointingHandCursor);
    setObjectName("viewCube"); setToolTip(tr("Click a face to select a view. Orbiting returns to perspective."));
}
void ViewCube::setCamera(const Camera &camera) { camera_ = camera; update(); }
void ViewCube::setDisplayControls(QWidget *controls) {
    controls->setParent(this);controls->setFixedWidth(int(controlWidth));controls->adjustSize();controls->move(int(controlLeft),displayTop);controls->show();
    setFixedHeight(displayTop+controls->height()+int(controlLeft));
}
// Full-width button under the cube that flips the viewport between Dark and Light.
QRectF ViewCube::backgroundRect() const { return {controlLeft,firstRow,controlWidth,rowHeight}; }
void ViewCube::setLightBackground(bool light) { if (light_!=light) { light_ = light; update(); } }
// Corner index bits: x=bit0, y=bit1, z=bit2. Each face lists top-left, top-right, bottom-right, bottom-left
// as read from outside with the camera's up for that preset, so labels are never mirrored or upside down.
static const int faceCorners[6][4] = {{6,7,5,4},{3,2,0,1},{2,6,4,0},{7,3,1,5},{2,3,7,6},{4,5,1,0}};
static const QVector3D faceNormals[6] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
std::vector<ViewCube::Face> ViewCube::faces() const {
    const auto view = camera_.viewMatrix();
    // The projected cube never exceeds sqrt(3) half-edges, so it stays clear of the frame and the controls.
    const QPointF centre(width()/2.0,45); const double scale = 40/std::sqrt(3.0);
    QPointF corners[8];
    for (int i=0; i<8; ++i) {
        const auto c = view.mapVector({(i&1)?1.0f:-1.0f,(i&2)?1.0f:-1.0f,(i&4)?1.0f:-1.0f});
        corners[i] = centre+QPointF(c.x(),-c.y())*scale;
    }
    std::vector<Face> result;
    for (int i=0; i<6; ++i) {
        const float facing = view.mapVector(faceNormals[i]).z();
        if (facing<0.001f) continue;
        Face face; face.preset = presets[i]; face.facing = facing;
        for (int k : faceCorners[i]) face.polygon << corners[k];
        result.push_back(face);
    }
    return result; // Convex and back-face culled, so paint order does not matter.
}
ViewPreset ViewCube::hit(const QPointF &point) const {
    auto polygons = faces();
    for (auto i=polygons.rbegin(); i!=polygons.rend(); ++i) if (i->polygon.containsPoint(point,Qt::OddEvenFill)) return i->preset;
    return ViewPreset::Free;
}
void ViewCube::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing);
    painter.setPen(EditorTheme::fieldBorder()); painter.setBrush(EditorTheme::panel()); painter.drawRoundedRect(rect().adjusted(1,1,-1,-1),EditorTheme::panelRadius,EditorTheme::panelRadius);
    painter.setFont(QFont("Segoe UI",8)); painter.setPen(QColor("#bbb"));
    for (const auto &face : faces()) {
        const bool active = face.preset==camera_.preset;
        painter.setBrush(face.preset==hover_ ? QColor("#497957") : active ? QColor("#2e6d4e") : EditorTheme::panelBorder());
        painter.setPen(QColor("#aaa")); painter.drawPolygon(face.polygon);
        const int index = int(std::find(std::begin(presets),std::end(presets),face.preset)-std::begin(presets));
        // Draw the label in face space so it turns and foreshortens with the face and never spills over it.
        const double alpha = std::clamp((face.facing-0.2)/0.4,0.0,1.0);
        QTransform toFace;
        if (alpha<=0 || !QTransform::quadToQuad(QPolygonF({{0,0},{100,0},{100,100},{0,100}}),face.polygon,toFace)) continue;
        const QString label = QString::fromLatin1(names[index]);
        QFont font("Segoe UI"); font.setBold(true); font.setPixelSize(22); font.setStyleStrategy(QFont::NoSubpixelAntialias);
        const double textWidth = QFontMetricsF(font).horizontalAdvance(label);
        if (textWidth>84) font.setPixelSize(std::max(1,int(22*84/textWidth)));
        QColor colour(Qt::white); colour.setAlphaF(alpha);
        painter.save(); painter.setTransform(toFace,true); painter.setFont(font); painter.setPen(colour);
        painter.drawText(QRectF(0,0,100,100),Qt::AlignCenter,label); painter.restore();
    }
    painter.setFont(QFont("Segoe UI",9));
    painter.setBrush(backgroundHover_ ? EditorTheme::buttonHover() : EditorTheme::button());
    painter.setPen(Qt::NoPen); painter.drawRoundedRect(backgroundRect(),EditorTheme::controlRadius,EditorTheme::controlRadius);
    painter.setPen(Qt::white); painter.drawText(backgroundRect(),Qt::AlignCenter,light_ ? tr("Background: Light") : tr("Background: Dark"));
}
void ViewCube::leaveEvent(QEvent *event) {
    hover_ = ViewPreset::Free; backgroundHover_ = false; update(); QWidget::leaveEvent(event);
}
void ViewCube::mousePressEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton) return;
    if (backgroundRect().contains(event->position())) { emit lightBackgroundSelected(!light_); event->accept(); return; }
    const auto preset = hit(event->position());
    if (preset!=ViewPreset::Free) emit viewSelected(preset);
    event->accept();
}
void ViewCube::mouseMoveEvent(QMouseEvent *event) {
    hover_ = hit(event->position()); backgroundHover_ = backgroundRect().contains(event->position());
    const char *keys[] = {"Numpad 1","Ctrl+Numpad 1","Ctrl+Numpad 3","Numpad 3","Numpad 7","Ctrl+Numpad 7"};
    const int index = int(std::find(std::begin(presets),std::end(presets),hover_)-std::begin(presets));
    if (backgroundHover_) setToolTip(tr("Switch the viewport background and grid between Dark and Light. Display only: the rest of the interface, projects and presets are unaffected."));
    else setToolTip(index<6 ? tr("Switch to %1 view (%2). Orbiting returns to perspective.").arg(tr(viewNames[index]),QString::fromLatin1(keys[index]))
        : tr("Click a face to select a view. Reset user perspective with Numpad 0."));
    update();
}
