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
#include <QSettings>
#include <QStringList>

// Uses the application's native QSettings store (the user registry on Windows).
// Accepts an explicit store so history tests can use an isolated temporary file.
class FileHistory {
public:
    explicit FileHistory(QSettings &settings) : settings_(settings) {}
    static constexpr int MaximumRecentFiles = 10;
    QStringList recent() const;
    void remember(const QString &path);
    void clearRecent();
    QString openPath(const QString &category) const;
    QString savePath(const QString &category, const QString &suggestedName) const;
    void rememberImage(const QString &path);
private:
    static QString normalized(const QString &path);
    static bool samePath(const QString &a, const QString &b);
    QString directory(const QString &category) const;
    QSettings &settings_;
};
