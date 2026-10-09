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
#include <QColor>

namespace EditorTheme {
// Surface colours adapted from Motiva Layama's local layamastyle.h.
inline QColor window() {return {24,25,26};}
inline QColor panel() {return {31,32,33};}
inline QColor panelBorder() {return {47,49,51};}
inline QColor field() {return {20,21,22};}
inline QColor fieldBorder() {return {46,48,50};}
inline QColor hoverBorder() {return {80,83,86};}
inline QColor sectionHeader() {return {40,41,43};}
inline QColor selection() {return {55,58,61};}
inline QColor itemHover() {return {71,74,77};}
inline QColor viewportBackground() {return {14,14,15};}
inline QColor text() {return {231,234,235};}
inline QColor mutedText() {return {166,169,172};}
inline QColor disabledText() {return {108,110,112};}
inline QColor accent() {return {240,60,90};}
// Buttons sit slightly above the panel without a border; fields stay recessed below it.
inline QColor button() {return {41,43,45};}
inline QColor buttonHover() {return {51,53,56};}
inline QColor buttonDisabled() {return {36,37,39};}
constexpr int controlRadius = 4; // buttons, fields, combos
constexpr int panelRadius = 6;   // groups and floating panels
void install();
}
