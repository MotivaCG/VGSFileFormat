#include "rangeslider.h"
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
RangeSlider::RangeSlider(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(34);
    setMouseTracking(true);
}

void RangeSlider::setFrameRange(int minimum, int maximum)
{
    m_minimum = minimum;
    m_maximum = std::max(minimum, maximum);
    m_start = std::clamp(m_start, m_minimum, m_maximum);
    m_end = std::clamp(m_end, m_start, m_maximum);
    m_playhead = std::clamp(m_playhead, m_minimum, m_maximum);
    update();
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
    update();
}

QSize RangeSlider::sizeHint() const
{
    return QSize(640, 38);
}

QRect RangeSlider::trackRect() const
{
    constexpr int margin = 10;
    const int y = height() / 2 - 3;
    return QRect(margin, y, std::max(1,width() - margin * 2), 4);
}

int RangeSlider::valueToX(int value) const
{
    const QRect track = trackRect();
    if (m_maximum <= m_minimum)
        return track.left();

    const double t = static_cast<double>(value - m_minimum) / static_cast<double>(m_maximum - m_minimum);
    return track.left() + qRound(t * track.width());
}

int RangeSlider::xToValue(int x) const
{
    const QRect track = trackRect();
    if (m_maximum <= m_minimum)
        return m_minimum;

    const int clampedX = std::clamp(x, track.left(), track.right());
    const double t = static_cast<double>(clampedX - track.left()) / static_cast<double>(track.width());
    return m_minimum + qRound(t * (m_maximum - m_minimum));
}

QRect RangeSlider::triangleHandleRect(int value, bool top) const
{
    constexpr int halfWidth = 8;
    constexpr int heightPx = 8;
    const int x = valueToX(value);
    const int y = top ? 2 : height() - heightPx - 2;
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
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(70, 70, 70));
    painter.drawRoundedRect(track, 2, 2);

    QRect selected = track;
    selected.setLeft(valueToX(m_start));
    selected.setRight(valueToX(m_end));
    painter.setBrush(QColor(65, 176, 24));
    painter.drawRoundedRect(selected, 2, 2);

    const int playheadX = valueToX(m_playhead);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(playheadX, 10, playheadX, height() - 10);

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

    drawTriangle(m_start, true, QColor(245, 245, 245));
    drawTriangle(m_end, false, QColor(245, 245, 245));
}

void RangeSlider::mousePressEvent(QMouseEvent* event)
{
    if (event->button()!=Qt::LeftButton) {event->ignore();return;}
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
    if (m_dragHandle == DragHandle::None)
        return;

    setDraggedValue(xToValue(event->position().x()));
}

void RangeSlider::mouseReleaseEvent(QMouseEvent*)
{
    m_dragHandle = DragHandle::None;
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

