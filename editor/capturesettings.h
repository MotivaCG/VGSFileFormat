#pragma once
#include <QJsonObject>
#include <QStringList>

struct CaptureSettings {
    QString catalogueId,title,author = "SMN|The4DSCanner",projectName,takeName,studio = "Valladolid",copyright;
    QString softwareName = "VGS Editor",softwareVersion = "0.1",extraJson;
    QStringList tags;
    // plain: uncompressed (.pgs). Exports set it from the output file's extension; it is
    // only read from older projects and presets, never chosen in the editor.
    bool plain = false,despill = false,recoverSkin = true;
    int shDegree = -1,playbackMode = -1; // -1 preserves the source setting.
    double despillStrength = 1,greenGain = 0.97,viewChromaScale = 0.5;
    QJsonObject json() const;
    bool validate(QString *error) const;
    static bool fromJson(const QJsonObject &json,CaptureSettings *settings,QString *error);
};
