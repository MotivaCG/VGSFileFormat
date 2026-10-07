#pragma once
#include <QColor>

namespace EditorTheme {
// Surface colours adapted from Motiva Layama's local layamastyle.h.
inline QColor window() {return {28,29,30};}
inline QColor panel() {return {36,37,38};}
inline QColor panelBorder() {return {52,54,56};}
inline QColor field() {return {24,25,26};}
inline QColor fieldBorder() {return {64,66,68};}
inline QColor hoverBorder() {return {85,88,91};}
inline QColor sectionHeader() {return {45,46,48};}
inline QColor selection() {return {60,63,66};}
inline QColor itemHover() {return {76,79,82};}
inline QColor viewportBackground() {return {19,19,19};}
inline QColor text() {return {231,234,235};}
inline QColor mutedText() {return {166,169,172};}
inline QColor disabledText() {return {108,110,112};}
inline QColor accent() {return {240,60,90};}
void install();
}
