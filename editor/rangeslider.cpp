// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "rangeslider.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
RangeSlider::RangeSlider(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(40);
    setMouseTracking(true);
}

void RangeSlider::setFrameRange(int minimum, int maximum)
{
    maximum = std::max(minimum, maximum);
    const bool changed = minimum != m_minimum || maximum != m_maximum;
    m_minimum = minimum;
    m_maximum = maximum;
    if (changed)
        resetView();
    m_start = std::clamp(m_start, m_minimum, m_maximum);
    m_end = std::clamp(m_end, m_start, m_maximum);
    m_playhead = std::clamp(m_playhead, m_minimum, m_maximum);
    update();
}

void RangeSlider::setFrameRate(double fps)
{
    if (m_frameRate == fps)
        return;
    m_frameRate = fps;
    update();
}

void RangeSlider::setLabelsInSeconds(bool seconds)
{
    if (m_labelsInSeconds == seconds)
        return;
    m_labelsInSeconds = seconds;
    update();
}

QString RangeSlider::secondLabel(int second) const
{
    if (!m_labelsInSeconds)
        return QString::number(qRound(second * m_frameRate));
    if (second < 60)
        return QString("%1 s").arg(second);
    return QString("%1:%2").arg(second / 60).arg(second % 60, 2, 10, QChar('0'));
}

QVector<int> RangeSlider::labelledSeconds() const
{
    QVector<int> seconds;
    const QRect track = trackRect();
    const double span = m_viewLast - m_viewFirst;
    if (m_frameRate <= 0 || span <= 0)
        return seconds;
    const double perSecond = track.width() / span * m_frameRate;
    if (perSecond < 8)
        return seconds; // no second ticks to label
    QFont small = font(); small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1));
    const QFontMetrics metrics(small);
    // As wide as the longest label in view, plus air on both sides.
    const int widest = metrics.horizontalAdvance(secondLabel(int(m_viewLast / m_frameRate) + 1));
    const double wanted = widest + 36.0;
    static const int steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    int step = 0;
    for (int candidate : steps)
        if (candidate * perSecond >= wanted) { step = candidate; break; }
    if (!step)
        return seconds;
    for (int second = int(std::ceil(m_viewFirst / m_frameRate / step)) * step; second * m_frameRate <= m_viewLast; second += step) {
        const double x = frameX(second * m_frameRate);
        const int half = metrics.horizontalAdvance(secondLabel(second)) / 2 + 2;
        if (x - half < track.left() || x + half > track.right())
            continue; // it would hang past an end of the track
        if (std::abs(x - frameX(m_end)) < half + 10)
            continue; // the End marker hangs at the same height
        seconds.append(second);
    }
    return seconds;
}

void RangeSlider::setRangeValues(int start, int end)
{
    const int clampedStart = std::clamp(start, m_minimum, m_maximum);
    const int clampedEnd = std::clamp(end, clampedStart, m_maximum);
    if (m_start == clampedStart && m_end == clampedEnd)
        return;

    m_start = clampedStart;
    m_end = clampedEnd;
    m_playhead = std::clamp(m_playhead, m_start, m_end);
    update();
    emit rangeChanged(m_start, m_end, m_start);
}

void RangeSlider::setPlayheadValue(int value)
{
    const int clamped = std::clamp(value, m_minimum, m_maximum);
    if (m_playhead == clamped)
        return;

    m_playhead = clamped;
    // Zoomed in, the view pages along to keep the playhead in sight (playback, stepping).
    const double span = m_viewLast - m_viewFirst;
    if (m_playhead > m_viewLast)
        setView(m_playhead - span * 0.1, span);
    else if (m_playhead < m_viewFirst)
        setView(m_playhead - span * 0.9, span);
    update();
}

double RangeSlider::zoom() const
{
    const double span = m_viewLast - m_viewFirst;
    return span > 0 ? (m_maximum - m_minimum) / span : 1;
}

void RangeSlider::resetView()
{
    setView(m_minimum, m_maximum - m_minimum);
}

// The view keeps inside the capture and never narrower than two frames.
void RangeSlider::setView(double first, double span)
{
    const double full = m_maximum - m_minimum;
    span = std::clamp(span, std::min(full, 2.0), full);
    first = std::clamp(first, double(m_minimum), m_maximum - span);
    if (first == m_viewFirst && first + span == m_viewLast)
        return;
    m_viewFirst = first;
    m_viewLast = first + span;
    update();
    emit viewChanged(m_viewFirst, m_viewLast);
}

void RangeSlider::zoomAt(double factor, int x)
{
    const QRect track = trackRect();
    const double t = std::clamp((x - track.left()) / double(track.width()), 0.0, 1.0);
    const double span = m_viewLast - m_viewFirst, anchor = m_viewFirst + t * span;
    const double next = std::clamp(span / factor, std::min(double(m_maximum - m_minimum), 2.0), double(m_maximum - m_minimum));
    setView(anchor - t * next, next);
}

void RangeSlider::panByPixels(double pixels)
{
    const double span = m_viewLast - m_viewFirst;
    setView(m_viewFirst - pixels / std::max(1, trackRect().width()) * span, span);
}

QSize RangeSlider::sizeHint() const
{
    return QSize(640, 40);
}

void RangeSlider::setTrackInsets(int left, int right)
{
    // The end markers reach 8 px past the track on either side.
    left = std::max(9, left); right = std::max(9, right);
    if (m_leftInset == left && m_rightInset == right)
        return;
    m_leftInset = left; m_rightInset = right;
    update();
}

void RangeSlider::setLabel(const QString &text, int x)
{
    if (m_label == text && m_labelX == x)
        return;
    m_label = text; m_labelX = x;
    update();
}

// The track sits near the top: the Start marker above it, the End marker just below, and
// the row of second labels under that.
QRect RangeSlider::trackRect() const
{
    const int y = 14;
    return QRect(m_leftInset, y, std::max(1, width() - m_leftInset - m_rightInset), 4);
}

double RangeSlider::frameX(double value) const
{
    const QRect track = trackRect();
    const double span = m_viewLast - m_viewFirst;
    if (span <= 0)
        return track.left();
    return track.left() + (value - m_viewFirst) / span * track.width();
}

int RangeSlider::valueToX(int value) const
{
    return qRound(frameX(value));
}

int RangeSlider::xToValue(int x) const
{
    const QRect track = trackRect();
    if (m_maximum <= m_minimum)
        return m_minimum;

    const int clampedX = std::clamp(x, track.left(), track.right());
    const double t = static_cast<double>(clampedX - track.left()) / static_cast<double>(track.width());
    return std::clamp(int(std::lround(m_viewFirst + t * (m_viewLast - m_viewFirst))), m_minimum, m_maximum);
}

QRect RangeSlider::triangleHandleRect(int value, bool top) const
{
    constexpr int halfWidth = 8;
    constexpr int heightPx = 8;
    const int x = valueToX(value);
    const QRect track = trackRect();
    const int y = top ? track.top() - heightPx - 4 : track.bottom() + 3;
    return QRect(x - halfWidth, y, halfWidth * 2, heightPx);
}

QRect RangeSlider::playheadHandleRect() const
{
    const int x = valueToX(m_playhead);
    return QRect(x - 5, 0, 10, height());
}

void RangeSlider::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect track = trackRect();
    if (!m_label.isEmpty()) {
        // Centred on the track; elided rather than reaching the Start marker.
        const QRect area(m_labelX, track.center().y() - height() / 2, track.left() - 12 - m_labelX, height());
        if (area.width() > 0) {
            painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
            painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_label, Qt::ElideRight, area.width()));
            // Zoomed in, the level sits just left of the name, in its font but muted.
            if (zoom() > 1.001) {
                painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
                const QRect before(0, area.top(), std::max(0, m_labelX - 6), area.height());
                painter.drawText(before, Qt::AlignRight | Qt::AlignVCenter, QString(QChar(0x00d7)) + QString::number(zoom(), 'f', zoom() < 10 ? 1 : 0));
            }
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(70, 70, 70));
    painter.drawRoundedRect(track, 2, 2);

    QRect selected = track;
    selected.setLeft(std::max(track.left(), valueToX(m_start)));
    selected.setRight(std::min(track.right(), valueToX(m_end)));
    painter.setBrush(QColor(65, 176, 24));
    if (selected.width() > 0)
        painter.drawRoundedRect(selected, 2, 2);

    // A tick every second, in a much lighter green, and one per frame halfway between that and
    // the track's green - each only while they stay apart; packed closer they would smear.
    const double span = m_viewLast - m_viewFirst;
    if (m_frameRate > 0 && span > 0) {
        const double perFrame = track.width() / span, perSecond = perFrame * m_frameRate;
        const bool seconds = perSecond >= 8, frames = perFrame >= 6;
        painter.save(); painter.setRenderHint(QPainter::Antialiasing, false);
        auto tick = [&](double frame, int reach, const QColor &colour) {
            const int x = qRound(frameX(frame));
            painter.setPen(QPen(colour, 1)); painter.drawLine(x, track.top() - reach, x, track.bottom() + reach);
        };
        if (frames) {
            for (int frame = int(std::ceil(m_viewFirst)); frame <= m_viewLast; ++frame) {
                const double second = frame / m_frameRate;
                if (seconds && std::abs(second - std::round(second)) * m_frameRate < 0.5)
                    continue; // a second tick goes here
                tick(frame, 1, QColor(165, 222, 140)); // drawn alongside the seconds: a touch brighter
            }
        }
        if (seconds)
            for (int second = int(std::ceil(m_viewFirst / m_frameRate)); second * m_frameRate <= m_viewLast; ++second)
                tick(second * m_frameRate, 2, QColor(198, 236, 180));
        painter.restore();
        // Some of the seconds, named: time or frame, as the timeline counts.
        const auto labelled = labelledSeconds();
        if (!labelled.isEmpty()) {
            painter.save();
            QFont small = font(); small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1)); painter.setFont(small);
            QColor colour = palette().color(QPalette::Text); colour.setAlpha(150); painter.setPen(colour);
            const int top = track.bottom() + 7, height = QFontMetrics(small).height();
            for (int second : labelled) {
                const int x = qRound(frameX(second * m_frameRate));
                painter.drawText(QRect(x - 60, top, 120, height), Qt::AlignHCenter | Qt::AlignTop, secondLabel(second));
            }
            painter.restore();
        }
    }

    // Only what lies in the view is drawn; the markers still reach just past the track ends.
    auto visible = [&](int value) { return value >= m_viewFirst - 1e-6 && value <= m_viewLast + 1e-6; };
    const int playheadX = valueToX(m_playhead);
    painter.setPen(QPen(Qt::white, 2));
    if (visible(m_playhead))
        painter.drawLine(playheadX, track.top() - 7, playheadX, track.bottom() + 11);

    auto drawTriangle = [&](int value, bool top, const QColor& fill) {
        const QRect rect = triangleHandleRect(value, top);
        const int x = valueToX(value);
        QPolygon polygon;
        if (top) {
            polygon << QPoint(x, rect.bottom())
                    << QPoint(rect.left(), rect.top())
                    << QPoint(rect.right(), rect.top());
        } else {
            polygon << QPoint(x, rect.top())
                    << QPoint(rect.left(), rect.bottom())
                    << QPoint(rect.right(), rect.bottom());
        }

        painter.setBrush(fill);
        painter.setPen(QPen(QColor(30, 30, 30), 1));
        painter.drawPolygon(polygon);
    };

    if (visible(m_start))
        drawTriangle(m_start, true, QColor(245, 245, 245));
    if (visible(m_end))
        drawTriangle(m_end, false, QColor(245, 245, 245));
}

void RangeSlider::mousePressEvent(QMouseEvent* event)
{
    if (event->button()==Qt::MiddleButton) {m_panning=true;m_panX=event->position().x();setCursor(Qt::ClosedHandCursor);return;}
    if (event->button()!=Qt::LeftButton) {event->ignore();return;}
    // The name to the left is not part of the track.
    if (event->position().x() < trackRect().left() - 10) {event->ignore();return;}
    const int clickValue = xToValue(event->position().x());
    const QPoint pos = event->position().toPoint();

    if (triangleHandleRect(m_start, true).contains(pos)) {
        m_dragHandle = DragHandle::Start;
    } else if (triangleHandleRect(m_end, false).contains(pos)) {
        m_dragHandle = DragHandle::End;
    } else if (playheadHandleRect().contains(pos)) {
        m_dragHandle = DragHandle::Playhead;
    } else {
        const int startDistance = std::abs(clickValue - m_start);
        const int endDistance = std::abs(clickValue - m_end);
        const int playheadDistance = std::abs(clickValue - m_playhead);
        if (playheadDistance <= startDistance && playheadDistance <= endDistance)
            m_dragHandle = DragHandle::Playhead;
        else if (m_start == m_end)
            m_dragHandle = (clickValue >= m_end) ? DragHandle::End : DragHandle::Start;
        else
            m_dragHandle = (startDistance <= endDistance) ? DragHandle::Start : DragHandle::End;
    }

    setDraggedValue(clickValue);
}

void RangeSlider::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {panByPixels(event->position().x()-m_panX);m_panX=event->position().x();return;}
    if (m_dragHandle == DragHandle::None)
        return;

    setDraggedValue(xToValue(event->position().x()));
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button()==Qt::MiddleButton && m_panning) {m_panning=false;unsetCursor();return;}
    m_dragHandle = DragHandle::None;
}

// Double-click the name (or the middle button anywhere): back to the whole capture.
void RangeSlider::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button()==Qt::MiddleButton || (event->button()==Qt::LeftButton && event->position().x() < trackRect().left() - 10)) {resetView();return;}
    mousePressEvent(event);
}

// Wheel zooms about the pointer; Shift+wheel or a horizontal wheel pans.
void RangeSlider::wheelEvent(QWheelEvent* event)
{
    const QPoint delta = event->angleDelta();
    if (delta.x() != 0 || (event->modifiers() & Qt::ShiftModifier)) {
        const int steps = delta.x() != 0 ? delta.x() : delta.y();
        panByPixels(steps / 120.0 * trackRect().width() * 0.1);
    } else if (delta.y() != 0) {
        zoomAt(std::pow(1.25, delta.y() / 120.0), qRound(event->position().x()));
    }
    event->accept();
}

void RangeSlider::setDraggedValue(int value)
{
    int previewFrame = value;
    if (m_dragHandle == DragHandle::Start) {
        m_start = std::clamp(value, m_minimum, m_end);
        m_playhead = m_start;
        previewFrame = m_start;
    } else if (m_dragHandle == DragHandle::End) {
        m_end = std::clamp(value, m_start, m_maximum);
        m_playhead = m_end;
        previewFrame = m_end;
    } else if (m_dragHandle == DragHandle::Playhead) {
        m_playhead = std::clamp(value, m_minimum, m_maximum);
        previewFrame = m_playhead;
        update();
        emit playheadChanged(m_playhead);
        return;
    }

    update();
    emit rangeChanged(m_start, m_end, previewFrame);
}

