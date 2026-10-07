#pragma once
#include <QSize>
class QWidget;
double compactDisplayScale(const QSize &logicalScreenSize);
// Compact widget geometry while keeping Qt/Windows DPI and coordinates native.
void installCompactControls(QWidget *window,bool enabled);
