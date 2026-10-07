#pragma once
#include <QWidget>
// Adapted from Gracia Converter frame range control.
class RangeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit RangeSlider(QWidget* parent = nullptr);

    void setFrameRange(int minimum, int maximum);
    void setRangeValues(int start, int end);
    void setPlayheadValue(int value);
    int startValue() const { return m_start; }
    int endValue() const { return m_end; }
    int playheadValue() const { return m_playhead; }

signals:
    void rangeChanged(int start, int end, int previewFrame);
    void playheadChanged(int frame);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    enum class DragHandle { None, Start, End, Playhead };

    int valueToX(int value) const;
    int xToValue(int x) const;
    QRect triangleHandleRect(int value, bool top) const;
    QRect playheadHandleRect() const;
    QRect trackRect() const;
    void setDraggedValue(int value);

    int m_minimum = 0;
    int m_maximum = 0;
    int m_start = 0;
    int m_end = 0;
    int m_playhead = 0;
    DragHandle m_dragHandle = DragHandle::None;
};

