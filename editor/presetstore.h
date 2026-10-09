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
#include "project.h"
#include <QVector>

enum class PresetScope { Editor, Metadata };
struct PresetEntry { QString name, path; bool legacy = false; bool system = false; };
struct EditorPreset { QString name; Project settings; PresetScope scope = PresetScope::Editor; bool legacy = false; bool hasMetadata = false; };

// Portable configuration only: no capture path or capture-specific frame range.
// Presets live in the user's folder; a system folder - `presets` beside the program - adds
// read-only ones shipped with it. A user preset of the same name hides the system one.
class PresetStore {
public:
    explicit PresetStore(const QString &directory = {}, const QString &systemDirectory = {});
    QString systemDirectory() const { return systemDirectory_; }
    QString directory() const { return directory_; }
    bool ensureDirectory(QString *error) const;
    QVector<PresetEntry> list(PresetScope scope = PresetScope::Editor) const;
    bool read(const QString &path,EditorPreset *preset,QString *error) const;
    bool read(const QString &path,PresetScope scope,EditorPreset *preset,QString *error) const;
    bool save(const QString &name,const Project &settings,QString *path,QString *error,PresetScope scope) const;
private:
    QString presetPath(const QString &name,PresetScope scope) const;
    bool writePreset(const QString &path,const QString &name,const Project &settings,PresetScope scope,QString *error) const;
    void migrateLegacyPresets() const;
    QString directory_, systemDirectory_;
};
