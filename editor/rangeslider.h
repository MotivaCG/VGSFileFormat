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
#include <QVector>
#include <QWidget>
// Adapted from Gracia Converter frame range control.
class RangeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit RangeSlider(QWidget* parent = nullptr);

    void setFrameRange(int minimum, int maximum);
    // Frames per second, for the second and frame ticks.
    void setFrameRate(double fps);
    // Whether the second labels under the track read as times or as frame numbers.
    void setLabelsInSeconds(bool seconds);
    // The seconds labelled under the track: a regular step (1, 2, 5, 10, 15, 30 s, minutes...)
    // wide enough apart to read, none cut off at the track's ends; empty when the second
    // ticks themselves are too dense to draw.
    QVector<int> labelledSeconds() const;
    QString secondLabel(int second) const;
    void setRangeValues(int start, int end);
    void setPlayheadValue(int value);
    int startValue() const { return m_start; }
    int endValue() const { return m_end; }
    int playheadValue() const { return m_playhead; }
    // Where the track runs, from each side of the widget; it can line up with other tracks.
    void setTrackInsets(int left, int right);
    // A name drawn left of the track from x, like the modifier names under it.
    void setLabel(const QString &text, int x);
    QString label() const { return m_label; }
    int labelX() const { return m_labelX; }
    // Zoom and pan: the frames the track shows, first to last. Not saved; a newly opened
    // capture starts at 100%.
    double viewFirst() const { return m_viewFirst; }
    double viewLast() const { return m_viewLast; }
    double zoom() const;
    void resetView();
    void zoomAt(double factor, int x);
    void panByPixels(double pixels);
    QRect trackRect() const;

signals:
    void rangeChanged(int start, int end, int previewFrame);
    void playheadChanged(int frame);
    void viewChanged(double first, double last);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QSize sizeHint() const override;

private:
    enum class DragHandle { None, Start, End, Playhead };

    int valueToX(int value) const;
    int xToValue(int x) const;
    QRect triangleHandleRect(int value, bool top) const;
    QRect playheadHandleRect() const;
    void setDraggedValue(int value);
    void setView(double first, double span);
    double frameX(double value) const;

    int m_minimum = 0;
    int m_maximum = 0;
    int m_start = 0;
    int m_end = 0;
    int m_playhead = 0;
    int m_leftInset = 10, m_rightInset = 10;
    QString m_label;
    double m_viewFirst = 0, m_viewLast = 0;
    double m_frameRate = 0;
    bool m_labelsInSeconds = false;
    bool m_panning = false;
    double m_panX = 0;
    int m_labelX = 0;
    DragHandle m_dragHandle = DragHandle::None;
};

